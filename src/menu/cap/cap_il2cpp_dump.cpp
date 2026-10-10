// cap_il2cpp_dump.cpp — capability 11: IL2CPP / Unity dumper (front-end).
//
// The heavy lifting lives in libioctl_codex_dumper.so (a custom lib). This
// capability simply locates that lib among the loaded custom libs and calls its
// ioctl_codex_dumper_run() export with our ctx. Keeping the dumper as a custom
// lib means it can be hot-swapped and updated independently of the menu.

#include "cap_util.h"

#include <dlfcn.h>

namespace codex {

void cap_il2cpp_dump_apply(const IoctlCodexCtx *ctx) {
    ilog("il2cpp", "invoking IL2CPP dumper ...");

    // Prefer an already-loaded dumper lib (registered in the custom-lib list).
    void *h = dlopen("libioctl_codex_dumper.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) {
        std::string p = std::string(ctx->app_data_dir ? ctx->app_data_dir : "") +
                        "/codex/libs/libioctl_codex_dumper.so";
        h = dlopen(p.c_str(), RTLD_NOW);
    }
    if (!h) {
        ilog("il2cpp", "dumper lib not found: %s", dlerror());
        return;
    }

    using run_fn = int (*)(const IoctlCodexCtx *);
    auto run = reinterpret_cast<run_fn>(dlsym(h, "ioctl_codex_dumper_run"));
    if (!run) { ilog("il2cpp", "ioctl_codex_dumper_run missing"); return; }

    int rc = run(ctx);
    ilog("il2cpp", "dumper returned %d", rc);
}

} // namespace codex
