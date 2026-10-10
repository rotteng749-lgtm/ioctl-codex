// cap_detection_bypass.cpp — capability 9: root/emulator/frida detection bypass.
//
// Two layers:
//   * Native file probes: hook fopen/open/openat/access/faccessat/stat/lstat and
//     report ENOENT for paths that reveal root, Magisk, KernelSU, Frida or an
//     emulator. This defeats the common "does /system/xbin/su exist" checks.
//   * System properties: hook __system_property_get and scrub ro.debuggable,
//     ro.secure, service.adb.root and similar flags.

#include "cap_util.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include <string>

namespace codex {

namespace {

bool is_hidden_path(const char *path) {
    if (!path) return false;
    static const char *needles[] = {
        "magisk", "Magisk", "MAGISK", "/su", "superuser", "busybox",
        "frida", "Frida", "xposed", "Xposed", "substrate", "riru", "zygisk",
        "kernelsu", "KernelSU", "ksud", ".ksu", "shamiko", "denylist",
        "test-keys", "qemu", "goldfish", "ranchu", "/system/bin/su",
        "/sbin/su", "/system/xbin/su", "/data/adb",
    };
    for (auto *n : needles) if (strstr(path, n)) return true;
    return false;
}

// --- file probes -----------------------------------------------------------
FILE *(*orig_fopen)(const char *, const char *) = nullptr;
int  (*orig_open)(const char *, int, ...) = nullptr;
int  (*orig_openat)(int, const char *, int, ...) = nullptr;
int  (*orig_access)(const char *, int) = nullptr;
int  (*orig_faccessat)(int, const char *, int, int) = nullptr;
int  (*orig_stat)(const char *, struct stat *) = nullptr;
int  (*orig_lstat)(const char *, struct stat *) = nullptr;

FILE *my_fopen(const char *p, const char *m) {
    if (is_hidden_path(p)) { errno = ENOENT; return nullptr; }
    return orig_fopen ? orig_fopen(p, m) : nullptr;
}
int my_open(const char *p, int f, ...) {
    if (is_hidden_path(p)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    if (f & O_CREAT) { va_list ap; va_start(ap, f); mode = va_arg(ap, mode_t); va_end(ap); }
    return orig_open ? orig_open(p, f, mode) : -1;
}
int my_openat(int dirfd, const char *p, int f, ...) {
    if (is_hidden_path(p)) { errno = ENOENT; return -1; }
    mode_t mode = 0;
    if (f & O_CREAT) { va_list ap; va_start(ap, f); mode = va_arg(ap, mode_t); va_end(ap); }
    return orig_openat ? orig_openat(dirfd, p, f, mode) : -1;
}
int my_access(const char *p, int m) {
    if (is_hidden_path(p)) { errno = ENOENT; return -1; }
    return orig_access ? orig_access(p, m) : -1;
}
int my_faccessat(int dirfd, const char *p, int m, int flags) {
    if (is_hidden_path(p)) { errno = ENOENT; return -1; }
    return orig_faccessat ? orig_faccessat(dirfd, p, m, flags) : -1;
}
int my_stat(const char *p, struct stat *st) {
    if (is_hidden_path(p)) { errno = ENOENT; return -1; }
    return orig_stat ? orig_stat(p, st) : -1;
}
int my_lstat(const char *p, struct stat *st) {
    if (is_hidden_path(p)) { errno = ENOENT; return -1; }
    return orig_lstat ? orig_lstat(p, st) : -1;
}

// --- system properties -----------------------------------------------------
int (*orig_sysprop_get)(const char *, char *) = nullptr;
int my_sysprop_get(const char *name, char *value) {
    int n = orig_sysprop_get ? orig_sysprop_get(name, value) : 0;
    if (!name) return n;
    // Force "clean" values for the flags detection code reads.
    if (!strcmp(name, "ro.debuggable")) { strcpy(value, "0"); return 1; }
    if (!strcmp(name, "ro.secure"))     { strcpy(value, "1"); return 1; }
    if (!strcmp(name, "service.adb.root")) { strcpy(value, "0"); return 1; }
    if (!strcmp(name, "ro.build.tags")) { strcpy(value, "release-keys"); return 1; }
    if (!strcmp(name, "ro.build.type")) { strcpy(value, "user"); return 1; }
    return n;
}

} // namespace

void cap_detection_bypass_apply(const IoctlCodexCtx *ctx) {
    (void)ctx;
    ilog("detection", "installing detection bypass hooks");

    // File probes live in libc; the *_chk fortified variants too.
    hook_sym("libc.so", "fopen", (void *)my_fopen, (void **)&orig_fopen);
    hook_sym("libc.so", "open", (void *)my_open, (void **)&orig_open);
    hook_sym("libc.so", "openat", (void *)my_openat, (void **)&orig_openat);
    hook_sym("libc.so", "access", (void *)my_access, (void **)&orig_access);
    hook_sym("libc.so", "faccessat", (void *)my_faccessat, (void **)&orig_faccessat);
    hook_sym("libc.so", "stat", (void *)my_stat, (void **)&orig_stat);
    hook_sym("libc.so", "lstat", (void *)my_lstat, (void **)&orig_lstat);

    // Property reads (may live in libc or be a direct syscall wrapper).
    hook_sym("libc.so", "__system_property_get", (void *)my_sysprop_get, (void **)&orig_sysprop_get);

    ilog("detection", "bypass active (root/magisk/frida/emulator paths hidden)");
}

} // namespace codex
