// cap_storage_dump.cpp — capability 8: storage dump.
//
// Copies the target's private files (databases, shared_prefs, files/, cache/)
// into the staging dir, then publishes them to /sdcard/panxcz-dumper/<pkg>/...
// via the root companion. Also reports the external files dir.
//
// We read from the app data dir (which we are allowed to read) and write to the
// staging dir (also allowed); the root companion does the final /sdcard move.

#include "cap_util.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>

namespace codex {

namespace {

// Recursively copy `src` into `dst`, skipping sockets/pipes. Returns file count.
int copy_tree(const std::string &src, const std::string &dst, int depth = 0) {
    if (depth > 8) return 0;
    DIR *d = opendir(src.c_str());
    if (!d) return 0;
    mkdir(dst.c_str(), 0755);
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string sp = src + "/" + e->d_name;
        std::string dp = dst + "/" + e->d_name;
        struct stat st;
        if (lstat(sp.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            count += copy_tree(sp, dp, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            std::string cmd = "cp -f '" + sp + "' '" + dp + "' 2>/dev/null";
            if (system(cmd.c_str()) == 0) count++;
        }
    }
    closedir(d);
    return count;
}

} // namespace

void cap_storage_dump_apply(const IoctlCodexCtx *ctx) {
    ilog("storage_dump", "dumping app storage ...");

    std::string base = ctx->app_data_dir ? ctx->app_data_dir : "";
    if (base.empty()) { ilog("storage_dump", "no app_data_dir; aborting"); return; }

    std::string stage = (ctx->stage_root ? ctx->stage_root : "/data/local/tmp/codex-dump");
    std::string out = stage + "/" + g_target.package_name + "/" + "storage";

    int total = 0;
    const char *subs[] = {"files", "shared_prefs", "databases", "cache", nullptr};
    for (int i = 0; subs[i]; ++i) {
        std::string s = base + "/" + subs[i];
        struct stat st;
        if (stat(s.c_str(), &st) != 0) continue;
        int n = copy_tree(s, out + "/" + subs[i]);
        ilog("storage_dump", "  %s: %d file(s)", subs[i], n);
        total += n;
    }

    if (ctx->publish_dir) {
        std::string rel = g_target.package_name + "/storage";
        int rc = ctx->publish_dir(out.c_str(), rel.c_str());
        ilog("storage_dump", "publish rc=%d (%d files)", rc, total);
    } else {
        ilog("storage_dump", "%d file(s) staged at %s", total, out.c_str());
    }
}

} // namespace codex
