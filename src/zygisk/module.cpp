// src/zygisk/module.cpp — the ioctl-codex Zygisk module entry point.
//
// Responsibilities (kept deliberately small and robust):
//   1. onLoad: remember the Api handle; never touch the filesystem here.
//   2. preAppSpecialize: we run as zygote (no sandbox yet). This is the only
//      place where getModuleDir()/connectCompanion() work, so we snapshot the
//      target list and open the root-companion socket here, then stash both on
//      the heap for the post-specialize stage.
//   3. postAppSpecialize: we are now inside the target's sandbox. If the
//      package matched targets.txt we dlopen the menu lib from the *staged*
//      copy (SELinux allows the app to read its own data dir, not /data/adb)
//      and call ioctl_codex_menu_init().
//
// Why stage the .so instead of dlopen'ing straight from the module dir:
// after specialization the process is an untrusted_app and SELinux forbids it
// from executing files under /data/adb/*. The only writable+executable spot we
// control is the app's own data dir, so in preAppSpecialize (still zygote
// privilege) we copy everything the target needs into
// /data/data/<pkg>/codex/ and dlopen from there.
//
// We never call setOption(DLCLOSE_MODULE_LIBRARY): we keep hooks installed in
// the process, so the module lib must stay mapped.

#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ioctl_codex.h"
#include "zygisk.hpp"

#define LOG_TAG "ioctl-codex"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char *kModuleRoot = "/data/adb/ioctl-codex";
constexpr const char *kTargetsFile = "/data/adb/ioctl-codex/targets.txt";

// ----- small helpers -------------------------------------------------------

std::string read_file(const char *path) {
    std::string out;
    FILE *f = fopen(path, "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// Trim leading/trailing ASCII whitespace.
std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// One target line may be either "com.foo.bar" or "com.foo.bar # comment".
std::string first_token(const std::string &line) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') return {};
    size_t sp = t.find_first_of(" \t");
    if (sp != std::string::npos) t = t.substr(0, sp);
    return t;
}

// Is `pkg` listed in targets.txt? An entry "*" matches everything (dev aid).
bool package_is_target(const std::string &pkg) {
    std::string body = read_file(kTargetsFile);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t nl = body.find('\n', pos);
        std::string line = body.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        std::string tok = first_token(line);
        if (tok == "*" || tok == pkg) return true;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return false;
}

// Copy `src` -> `dst` (regular file). Creates parent dirs. Returns 0 on ok.
int copy_file(const std::string &src, const std::string &dst) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    // Create any missing parent directories of dst.
    for (size_t i = 1; i < dst.size(); ++i) {
        if (dst[i] == '/') {
            std::string dir = dst.substr(0, i);
            mkdir(dir.c_str(), 0755); // ignore EEXIST
        }
    }
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, n - off);
            if (w <= 0) { rc = -1; goto done; }
            off += w;
        }
    }
    if (n < 0) rc = -1;
done:
    close(in);
    close(out);
    return rc;
}

// Recursively copy a directory (used to stage libs/ + config/ into the app's
// data dir). Best effort — a missing source dir is not fatal.
//
// IMPORTANT: this must use ONLY raw syscalls (open/read/write/mkdir/opendir).
// preAppSpecialize runs on a partially-initialized zygote; ART's libsigchain is
// not ready, so anything that forks — system(), popen(), posix_spawn() — will
// SIGSEGV inside posix_spawn -> ApplyAttrs -> sigaction64. We learned this the
// hard way (the crash showed up as a libsigchain backtrace with copy_dir on the
// stack). No subprocesses here, ever.
int copy_tree(const std::string &src, const std::string &dst) {
    struct stat st{};
    if (stat(src.c_str(), &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);

    if (mkdir(dst.c_str(), 0755) != 0 && errno != EEXIST) {
        // Try to create parents then retry once.
        for (size_t i = 1; i < dst.size(); ++i)
            if (dst[i] == '/') mkdir(dst.substr(0, i).c_str(), 0755);
        if (mkdir(dst.c_str(), 0755) != 0 && errno != EEXIST) return -1;
    }

    DIR *d = opendir(src.c_str());
    if (!d) return -1;
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string s = src + "/" + e->d_name;
        std::string t = dst + "/" + e->d_name;
        // Recurse; a single failure must not abort the whole staging.
        if (copy_tree(s, t) != 0) rc = -1;
    }
    closedir(d);
    return rc;
}

// ----- root companion ------------------------------------------------------

// The companion runs as root. It receives newline-delimited requests on the
// socket and performs privileged filesystem moves. Protocol is line-based so
// it is trivial to debug with `socat`.
//
// Request grammar:
//   MOVE <src>\t<dst>\t<mode>\n   -> "OK\n" or "ERR <msg>\n"
//   MKDIR <path>\n                -> "OK\n" or "ERR <msg>\n"
//   PING\n                        -> "OK\n"
void root_companion(int client) {
    std::string buf;
    char tmp[4096];
    for (;;) {
        ssize_t n = read(client, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            std::string reply = "ERR unknown\n";

            if (line.rfind("PING", 0) == 0) {
                reply = "OK\n";
            } else if (line.rfind("MKDIR ", 0) == 0) {
                std::string path = line.substr(6);
                if (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) reply = "OK\n";
                else reply = std::string("ERR mkdir: ") + strerror(errno) + "\n";
            } else if (line.rfind("MOVE ", 0) == 0) {
                // MOVE <src>\t<dst>\t<mode>
                std::string rest = line.substr(5);
                size_t t1 = rest.find('\t');
                size_t t2 = t1 == std::string::npos ? std::string::npos : rest.find('\t', t1 + 1);
                if (t1 == std::string::npos || t2 == std::string::npos) {
                    reply = "ERR bad move\n";
                } else {
                    std::string src = rest.substr(0, t1);
                    std::string dst = rest.substr(t1 + 1, t2 - t1 - 1);
                    std::string mode = rest.substr(t2 + 1);
                    // mkdir -p of dst's parent
                    for (size_t i = 1; i < dst.size(); ++i)
                        if (dst[i] == '/') mkdir(dst.substr(0, i).c_str(), 0777);
                    int rc = rename(src.c_str(), dst.c_str());
                    if (rc != 0) {
                        // Cross-device: fall back to copy + unlink.
                        std::string cp = "cp -rf " + src + " " + dst + " && rm -rf " + src;
                        rc = system(cp.c_str());
                    }
                    if (rc == 0) {
                        if (!mode.empty()) {
                            std::string ch = "chmod " + mode + " " + dst;
                            (void)system(ch.c_str());
                        }
                        // Make /sdcard output world-visible.
                        std::string own = "chown -R media_rw:media_rw " + dst + " 2>/dev/null";
                        (void)system(own.c_str());
                        reply = "OK\n";
                    } else {
                        reply = std::string("ERR move: ") + strerror(errno) + "\n";
                    }
                }
            }
            ssize_t off = 0;
            while (off < (ssize_t)reply.size()) {
                ssize_t w = write(client, reply.data() + off, reply.size() - off);
                if (w <= 0) break;
                off += w;
            }
        }
    }
    close(client);
}

} // namespace

// ---------------------------------------------------------------------------

class IoctlCodexModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
        // NOTE: getModuleDir() only works in pre[XXX]Specialize, so we do not
        // call it here.
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        // We are zygote here: no sandbox, full module-dir access.
        if (!args->nice_name) return;
        const char *nice = env_->GetStringUTFChars(args->nice_name, nullptr);
        std::string pkg = nice ? nice : "";
        if (nice) env_->ReleaseStringUTFChars(args->nice_name, nice);
        if (pkg.empty()) return;

        if (!package_is_target(pkg)) return;

        LOGI("preAppSpecialize: %s is a target", pkg.c_str());

        // The app's data dir is one of the guaranteed arguments.
        if (args->app_data_dir) {
            const char *dd = env_->GetStringUTFChars(args->app_data_dir, nullptr);
            if (dd) {
                staged_dir_ = std::string(dd) + "/codex";
                env_->ReleaseStringUTFChars(args->app_data_dir, dd);
            }
        }
        if (staged_dir_.empty()) {
            staged_dir_ = "/data/data/" + pkg + "/codex";
        }

        // Stage libs + config into the app data dir (we are still zygote, so
        // we can read /data/adb and write the app dir). Pure syscalls only —
        // see copy_tree() for why we cannot fork here.
        copy_tree(std::string(kModuleRoot) + "/libs", staged_dir_ + "/libs");
        copy_tree(std::string(kModuleRoot) + "/config", staged_dir_ + "/config");

        // Open the root companion socket while we still can.
        root_fd_ = api_->connectCompanion();
        if (root_fd_ < 0) {
            LOGI("root companion unavailable; /sdcard output will be disabled");
        }

        is_target_ = true;
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!is_target_) return;
        (void)args;

        // Recover the package name from the staged dir path or nice_name is no
        // longer valid; we kept the pieces we need as members. Extract pkg from
        // staged_dir_ (".../<pkg>/codex").
        std::string pkg;
        {
            std::string d = staged_dir_;
            const std::string suffix = "/codex";
            if (d.size() > suffix.size() &&
                d.compare(d.size() - suffix.size(), suffix.size(), suffix) == 0) {
                d.resize(d.size() - suffix.size());
            }
            size_t slash = d.find_last_of('/');
            pkg = (slash == std::string::npos) ? d : d.substr(slash + 1);
        }

        std::string lib = staged_dir_ + "/libs/libioctl_codex_menu.so";
        void *h = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h) {
            LOGE("dlopen(%s) failed: %s", lib.c_str(), dlerror());
            // Fall back to the module dir in case SELinux allows it.
            std::string alt = std::string(kModuleRoot) + "/libs/libioctl_codex_menu.so";
            h = dlopen(alt.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (!h) {
                LOGE("fallback dlopen(%s) failed: %s", alt.c_str(), dlerror());
                return;
            }
        }

        using init_fn = int (*)(const char *, const char *, int64_t,
                                const char *, int, int);
        auto init = reinterpret_cast<init_fn>(
            dlsym(h, "ioctl_codex_menu_init"));
        if (!init) {
            LOGE("ioctl_codex_menu_init not found in menu lib");
            return;
        }

        // version_name/code are filled by the menu via PackageManager; pass
        // NULL/-1 here and let the menu resolve them with the app's own
        // Context (which is more reliable across split APKs).
        int rc = init(pkg.c_str(), nullptr, -1, staged_dir_.c_str(),
                      getuid(), root_fd_);
        LOGI("ioctl_codex_menu_init -> %d", rc);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool is_target_ = false;
    int root_fd_ = -1;
    std::string staged_dir_;
};

REGISTER_ZYGISK_MODULE(IoctlCodexModule)
REGISTER_ZYGISK_COMPANION(root_companion)
