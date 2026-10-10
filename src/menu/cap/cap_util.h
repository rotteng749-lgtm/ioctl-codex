// src/menu/cap/cap_util.h — shared helpers for the 11 capabilities.
#pragma once

#include <jni.h>
#include <shadowhook.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../menu.h"

namespace codex {

// ----- hook helpers (thin wrappers over ShadowHook) ------------------------

// Hook a symbol by name. ShadowHook returns the opaque stub handle on success
// (pass it to shadowhook_unhook later) or nullptr on failure. `lib` may be
// nullptr to search every loaded library.
inline void *hook_sym(const char *lib, const char *sym, void *replace, void **origin) {
    void *stub = shadowhook_hook_sym_name(lib, sym, replace, origin);
    if (!stub) {
        ilog("hook", "shadowhook %s!%s failed: %d", lib ? lib : "*", sym,
             shadowhook_get_errno());
    }
    return stub;
}

// Hook an absolute address (e.g. resolved from an RVA).
inline void *hook_addr(void *addr, void *replace, void **origin) {
    void *stub = shadowhook_hook_sym_addr(addr, replace, origin);
    if (!stub) ilog("hook", "shadowhook addr %p failed: %d", addr, shadowhook_get_errno());
    return stub;
}

// ----- JNI helpers ---------------------------------------------------------

// Current JavaVM captured by menu_init. Set from overlay.cpp.
extern JavaVM *g_vm;

// Attach the calling thread to the JVM if needed. Returns a JNIEnv* and sets
// *did_attach. Always pair with detach_if_needed().
inline JNIEnv *attach_env(bool *did_attach) {
    *did_attach = false;
    if (!g_vm) return nullptr;
    JNIEnv *env = nullptr;
    jint rc = g_vm->GetEnv((void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
        *did_attach = true;
    }
    return env;
}

inline void detach_if_needed(bool did_attach) {
    if (did_attach && g_vm) g_vm->DetachCurrentThread();
}

// ----- hexdump helper used by several dump caps ---------------------------
inline void hexdump_append(std::string &out, const uint8_t *p, size_t n, size_t max = 256) {
    char line[80];
    for (size_t i = 0; i < n && i < max; i += 16) {
        int off = snprintf(line, sizeof(line), "%08zx  ", i);
        for (size_t j = 0; j < 16 && i + j < n; ++j)
            off += snprintf(line + off, sizeof(line) - off, "%02x ", p[i + j]);
        out += line;
        out += "\n";
    }
}

} // namespace codex
