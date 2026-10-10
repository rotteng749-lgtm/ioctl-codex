// src/menu/loader.cpp — custom lib discovery, loading and hot-swap.
//
// A "custom lib" is any .so dropped into /data/adb/ioctl-codex/libs/. It is
// staged into <app_data_dir>/codex/libs/ by the Zygisk module (SELinux forbids
// executing from /data/adb once sandboxed), and loaded here with the stable
// ABI from common/ioctl_codex.h:
//
//     int  ioctl_codex_custom_init(const IoctlCodexCtx *ctx);
//     void ioctl_codex_custom_deinit(void);          // optional
//
// On load we call init(ctx). A non-zero return (or a missing init symbol) is
// reported in the overlay log and the handle is closed again. Hot-swap reloads
// a single lib without disturbing the others.

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "menu.h"

namespace codex {

namespace {
std::vector<CustomLib> g_libs;
std::mutex g_lib_mutex;
} // namespace

std::vector<CustomLib> &loader_libs() { return g_libs; }
std::mutex &loader_mutex() { return g_lib_mutex; }

// Directory where the Zygisk module staged the libs for this app.
static std::string libs_dir() {
    return g_target.stage_root.empty()
        ? std::string("/data/local/tmp/codex-libs")
        : (g_target.stage_root + "/../libs"); // <app_data_dir>/codex/libs
}

static void unload_one(CustomLib &lib) {
    if (!lib.handle) return;
    if (lib.deinit) {
        ilog("loader", "deinit %s", lib.name.c_str());
        lib.deinit();
    }
    dlclose(lib.handle);
    lib.handle = nullptr;
    lib.loaded = false;
    lib.init = nullptr;
    lib.deinit = nullptr;
}

static void load_one(CustomLib &lib) {
    lib.error.clear();
    lib.handle = dlopen(lib.path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.handle) {
        lib.error = dlerror() ? dlerror() : "dlopen failed";
        ilog("loader", "FAILED %s: %s", lib.name.c_str(), lib.error.c_str());
        return;
    }
    lib.init = reinterpret_cast<int (*)(const IoctlCodexCtx *)>(
        dlsym(lib.handle, "ioctl_codex_custom_init"));
    lib.deinit = reinterpret_cast<void (*)()>(
        dlsym(lib.handle, "ioctl_codex_custom_deinit"));
    if (!lib.init) {
        lib.error = "missing ioctl_codex_custom_init";
        ilog("loader", "FAILED %s: %s", lib.name.c_str(), lib.error.c_str());
        dlclose(lib.handle);
        lib.handle = nullptr;
        return;
    }
    int rc = lib.init(&g_ctx);
    if (rc != 0) {
        lib.error = "init returned " + std::to_string(rc);
        ilog("loader", "FAILED %s: %s", lib.name.c_str(), lib.error.c_str());
        if (lib.deinit) lib.deinit();
        dlclose(lib.handle);
        lib.handle = nullptr;
        lib.init = nullptr;
        lib.deinit = nullptr;
        return;
    }
    lib.loaded = true;
    ilog("loader", "loaded %s", lib.name.c_str());
}

void loader_scan() {
    std::lock_guard<std::mutex> lk(g_lib_mutex);
    std::string dir = libs_dir();
    DIR *d = opendir(dir.c_str());
    if (!d) {
        ilog("loader", "no libs dir %s", dir.c_str());
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.size() < 4 || name.substr(name.size() - 3) != ".so") continue;

        // Skip libs that are already tracked (do not double-load).
        bool seen = false;
        for (auto &l : g_libs) if (l.name == name) { seen = true; break; }
        if (seen) continue;

        CustomLib lib;
        lib.name = name;
        lib.path = dir + "/" + name;
        load_one(lib);
        g_libs.push_back(lib);
    }
    closedir(d);
    ilog("loader", "scan complete: %zu lib(s)", g_libs.size());
}

void loader_reload_all() {
    ilog("loader", "hot-swap: reloading all libs");
    std::lock_guard<std::mutex> lk(g_lib_mutex);
    for (auto &l : g_libs) unload_one(l);
    g_libs.clear();
    // Re-scan on the next tick; releasing the lock here would deadlock, so we
    // inline the scan instead of calling loader_scan().
    std::string dir = libs_dir();
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.size() < 4 || name.substr(name.size() - 3) != ".so") continue;
        CustomLib lib;
        lib.name = name;
        lib.path = dir + "/" + name;
        load_one(lib);
        g_libs.push_back(lib);
    }
    closedir(d);
}

void loader_reload(const std::string &name) {
    std::lock_guard<std::mutex> lk(g_lib_mutex);
    for (auto &l : g_libs) {
        if (l.name != name) continue;
        unload_one(l);
        load_one(l);
        return;
    }
    ilog("loader", "reload: %s not tracked", name.c_str());
}

} // namespace codex
