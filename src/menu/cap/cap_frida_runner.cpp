// cap_frida_runner.cpp — capability 10: custom Frida JS runner.
//
// Loads the embedded Frida gadget (shipped in the module as
// frida-gadget-arm64.so.xz, extracted to <app_data_dir>/codex/libs/). The
// gadget self-initialises from JNI_OnLoad and, per its config file, either
// listens on a TCP port or loads a script we point it at.
//
// This keeps the framework SELF-CONTAINED — no frida-server binary and no
// separate process are required. The gadget runs inside the target process.
//
// Config discovery: the gadget looks for a file next to the .so named
// "<libname>.config" (e.g. libfrida-gadget.config). We write that file with a
// "listen" interaction so you can attach with `frida -H 127.0.0.1:27042`,
// and optionally embed a script from <app_data_dir>/codex/config/script.js.

#include "cap_util.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace codex {

namespace {

bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string read_file(const std::string &p) {
    std::string out;
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// JSON-escape a string for embedding in the gadget config.
std::string json_escape(const std::string &s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default: o += c;
        }
    }
    return o;
}

} // namespace

void cap_frida_runner_apply(const IoctlCodexCtx *ctx) {
    std::string libdir = std::string(ctx->app_data_dir ? ctx->app_data_dir : "") + "/codex/libs";
    std::string gadget = libdir + "/libfrida-gadget.so";
    // Fall back to the staged arm64 name.
    if (!file_exists(gadget)) gadget = libdir + "/frida-gadget-arm64.so";

    if (!file_exists(gadget)) {
        ilog("frida", "gadget not found at %s", gadget.c_str());
        ilog("frida", "run scripts/extract-frida.sh, then reload libs");
        return;
    }

    // Write the gadget config next to the lib so the gadget finds it.
    std::string cfg_path = gadget + ".config";
    std::string script_path = std::string(ctx->app_data_dir ? ctx->app_data_dir : "") + "/codex/config/script.js";
    std::string cfg;
    if (file_exists(script_path)) {
        std::string js = read_file(script_path);
        cfg = "{\n  \"interaction\": { \"type\": \"script\", \"path\": \"" + json_escape(script_path) + "\" },\n"
              "  \"script\": \"" + json_escape(js) + "\"\n}\n";
        ilog("frida", "using script %s (%zu bytes)", script_path.c_str(), js.size());
    } else {
        cfg = "{\n  \"interaction\": { \"type\": \"listen\", \"address\": \"127.0.0.1\", \"port\": 27042 }\n}\n";
        ilog("frida", "no script.js; gadget will listen on 127.0.0.1:27042");
    }
    FILE *f = fopen(cfg_path.c_str(), "wb");
    if (f) { fwrite(cfg.data(), 1, cfg.size(), f); fclose(f); }

    void *h = dlopen(gadget.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        ilog("frida", "dlopen(%s) failed: %s", gadget.c_str(), dlerror());
        return;
    }
    ilog("frida", "gadget loaded; attach with: frida -H 127.0.0.1:27042 -n %s",
         g_target.package_name.c_str());
}

} // namespace codex
