// cap_native_dump.cpp — capability 2: native module dump.
//
// Walks /proc/self/maps, collects every mapped ELF (the .so files the target
// loaded), and for each prints its path, base address and size. Optionally
// dumps the exported dynamic symbols using the in-memory dynamic section via
// dl_iterate_phdr, so no file access to the (possibly inaccessible) .so is
// needed.

#include "cap_util.h"

#include <link.h>
#include <sys/stat.h>

#include <cinttypes>
#include <cstdio>
#include <string>
#include <vector>

namespace codex {

namespace {

struct ModInfo {
    std::string path;
    uintptr_t base;
    uintptr_t end;
};

int phdr_cb(struct dl_phdr_info *info, size_t, void *data) {
    auto *mods = reinterpret_cast<std::vector<ModInfo> *>(data);
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0; // skip main exe
    ModInfo m;
    m.path = info->dlpi_name;
    m.base = info->dlpi_addr;
    // Compute the load end from the program headers.
    uintptr_t max = 0;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD) max = std::max<uintptr_t>(max, ph.p_vaddr + ph.p_memsz);
    }
    m.end = info->dlpi_addr + max;
    mods->push_back(m);
    return 0;
}

} // namespace

void cap_native_dump_apply(const IoctlCodexCtx *ctx) {
    ilog("native_dump", "enumerating native modules ...");

    std::vector<ModInfo> mods;
    dl_iterate_phdr(phdr_cb, &mods);

    IoctlCodexWriter *w = ctx->open_writer ? ctx->open_writer("Native Dump") : nullptr;
    auto emit = [&](const std::string &s) {
        if (w && w->write) w->write(w->ctx, s.data(), s.size());
    };

    char buf[1024];
    snprintf(buf, sizeof(buf), "// ioctl-codex native module dump — %zu modules\n\n", mods.size());
    emit(buf);

    for (auto &m : mods) {
        snprintf(buf, sizeof(buf), "%-60s base=0x%" PRIxPTR " end=0x%" PRIxPTR " size=0x%" PRIxPTR "\n",
                 m.path.c_str(), m.base, m.end, m.end - m.base);
        emit(buf);
    }

    if (w && ctx->close_writer) ctx->close_writer(w);
    ilog("native_dump", "done (%zu modules)", mods.size());
}

} // namespace codex
