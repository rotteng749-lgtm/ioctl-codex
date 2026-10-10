// cap_mem_scan.cpp — capability 4: memory scanner with refinement.
//
// A small Cheat-Engine-style scanner. A scan searches all readable+writable
// private mappings for a value (int32/float/double) and remembers the hit
// addresses; refine() re-checks the previous hit list against a new value so
// repeated narrowing converges on the right address. Results are exposed in the
// "Scan" tab and can be written to via mem_write().

#include "cap_util.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace codex {

namespace {

struct Hit { uintptr_t addr; uint64_t raw; };
std::vector<Hit> g_hits;
std::mutex g_hits_mutex;

enum class ScanType { Int32, Float, Double };

bool readable_writable(const char *perms) {
    return perms[0] == 'r' && perms[1] == 'w' && perms[2] != 'x';
}

template <typename T>
void scan_value(std::vector<Hit> &hits, T value, size_t max_hits) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f) && hits.size() < max_hits) {
        unsigned long start = 0, end = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
        if (!readable_writable(perms)) continue;
        size_t len = end - start;
        if (len == 0 || len > (256UL << 20)) continue;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(start);
        for (size_t i = 0; i + sizeof(T) <= len; i += sizeof(T)) {
            T v;
            memcpy(&v, p + i, sizeof(T));
            if (v == value) {
                uint64_t raw = 0;
                memcpy(&raw, &v, sizeof(T));
                hits.push_back({start + i, raw});
                if (hits.size() >= max_hits) break;
            }
        }
    }
    fclose(f);
}

template <typename T>
void refine_value(std::vector<Hit> &hits, T value) {
    std::vector<Hit> keep;
    for (auto &h : hits) {
        T v;
        memcpy(&v, reinterpret_cast<void *>(h.addr), sizeof(T));
        if (v == value) keep.push_back(h);
    }
    hits.swap(keep);
}

} // namespace

// Public API used by the Scan tab.
void mem_scan_new(int32_t value) {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    g_hits.clear();
    scan_value<int32_t>(g_hits, value, 100000);
    ilog("mem_scan", "new scan int32=%d -> %zu hits", value, g_hits.size());
}

void mem_scan_refine(int32_t value) {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    refine_value<int32_t>(g_hits, value);
    ilog("mem_scan", "refine int32=%d -> %zu hits", value, g_hits.size());
}

void mem_scan_new_float(float value) {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    g_hits.clear();
    scan_value<float>(g_hits, value, 100000);
    ilog("mem_scan", "new scan float=%f -> %zu hits", value, g_hits.size());
}

void mem_scan_refine_float(float value) {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    refine_value<float>(g_hits, value);
    ilog("mem_scan", "refine float=%f -> %zu hits", value, g_hits.size());
}

size_t mem_scan_count() {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    return g_hits.size();
}

bool mem_scan_write(size_t index, int32_t value) {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    if (index >= g_hits.size()) return false;
    memcpy(reinterpret_cast<void *>(g_hits[index].addr), &value, sizeof(value));
    return true;
}

std::vector<std::pair<uintptr_t, int32_t>> mem_scan_list(size_t max) {
    std::lock_guard<std::mutex> lk(g_hits_mutex);
    std::vector<std::pair<uintptr_t, int32_t>> out;
    for (size_t i = 0; i < g_hits.size() && i < max; ++i) {
        int32_t v;
        memcpy(&v, reinterpret_cast<void *>(g_hits[i].addr), sizeof(v));
        out.push_back({g_hits[i].addr, v});
    }
    return out;
}

void cap_mem_scan_apply(const IoctlCodexCtx *ctx) {
    ilog("mem_scan", "scanner ready (use the Scan tab to search)");
    if (ctx->open_writer) {
        IoctlCodexWriter *w = ctx->open_writer("Memory Scan");
        if (w && w->write) {
            const char *hdr = "// ioctl-codex memory scanner ready.\n"
                              "// Enter a value in the Scan tab and hit New Scan.\n";
            w->write(w->ctx, hdr, strlen(hdr));
        }
        if (w && ctx->close_writer) ctx->close_writer(w);
    }
}

} // namespace codex
