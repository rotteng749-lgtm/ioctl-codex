// cap_string_dump.cpp — capability 3: string dump.
//
// Scans the writable/readable data mappings of the target for printable ASCII
// strings and reports the interesting ones (URLs, keys, tokens, flags). This is
// a heuristic "grep the process memory" tool; it deliberately filters noise.

#include "cap_util.h"

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace codex {

namespace {

bool interesting(const std::string &s) {
    if (s.size() < 6) return false;
    static const char *needles[] = {
        "http://", "https://", "api.", "token", "secret", "key", "password",
        "passwd", "Bearer", "Authorization", "flag{", "license", "admin",
        "debug", "root", "frida", "magisk", "xposed", "/data/", ".json",
    };
    for (auto *n : needles) {
        // case-insensitive contains
        std::string low = s;
        for (auto &c : low) c = (char)tolower((unsigned char)c);
        std::string nl = n;
        for (auto &c : nl) c = (char)tolower((unsigned char)c);
        if (low.find(nl) != std::string::npos) return true;
    }
    return false;
}

// Iterate readable private mappings of the process.
void scan_strings(std::vector<std::string> &out, size_t max_hits) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f) && out.size() < max_hits) {
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (perms[0] != 'r') continue;                    // must be readable
        if (strchr(perms, 'x')) continue;                 // skip code
        size_t len = end - start;
        if (len == 0 || len > (64UL << 20)) continue;     // skip huge/odd
        const char *p = reinterpret_cast<const char *>(start);
        std::string cur;
        for (size_t i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)p[i];
            if (c >= 0x20 && c < 0x7f) {
                cur.push_back((char)c);
            } else {
                if (interesting(cur)) out.push_back(cur);
                cur.clear();
                if (out.size() >= max_hits) break;
            }
        }
        if (interesting(cur)) out.push_back(cur);
    }
    fclose(f);
}

} // namespace

void cap_string_dump_apply(const IoctlCodexCtx *ctx) {
    ilog("string_dump", "scanning memory for interesting strings ...");
    std::vector<std::string> hits;
    scan_strings(hits, 2000);

    IoctlCodexWriter *w = ctx->open_writer ? ctx->open_writer("String Dump") : nullptr;
    auto emit = [&](const std::string &s) { if (w && w->write) w->write(w->ctx, s.data(), s.size()); };

    char buf[256];
    snprintf(buf, sizeof(buf), "// ioctl-codex string dump — %zu hit(s)\n\n", hits.size());
    emit(buf);
    for (auto &h : hits) emit(h + "\n");

    if (w && ctx->close_writer) ctx->close_writer(w);
    ilog("string_dump", "done (%zu hits)", hits.size());
}

} // namespace codex
