// src/menu/menu.h — internal declarations for the ioctl-codex overlay.
//
// Everything here is PRIVATE to libioctl_codex_menu.so. The only public
// surface is the C ABI in common/ioctl_codex.h (ioctl_codex_menu_init /
// ioctl_codex_menu_shutdown) plus the stable custom-lib ABI.
#pragma once

#include <android/native_window.h>
#include <jni.h>
#include <stdint.h>

#include <EGL/egl.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "ioctl_codex.h"

// Forward-declare ImGui's context type at GLOBAL scope. If this lived inside
// namespace codex it would create a distinct codex::ImGuiContext, which does
// not match the ::ImGuiContext returned by ImGui::CreateContext().
struct ImGuiContext;

namespace codex {

// ---------------------------------------------------------------------------
// Global overlay state. A single instance lives inside the target process.
// ---------------------------------------------------------------------------
struct TargetInfo {
    std::string package_name;
    std::string version_name;
    int64_t     version_code = -1;
    std::string app_data_dir;
    std::string stage_root;   // <app_data_dir>/codex/dump
    std::string dump_root;    // /sdcard/panxcz-dumper
    int         uid = -1;
    int         root_fd = -1;
};

// The context handed to custom libs. Kept alive for the process lifetime.
extern TargetInfo g_target;
extern IoctlCodexCtx g_ctx;

// UI-visible log ring buffer (also mirrored to logcat).
struct LogLine { int level; std::string tag; std::string text; };
void log_push(int level, const char *tag, const char *msg);
std::vector<LogLine> &log_lines();
std::mutex &log_mutex();

// In-memory dump buffers produced by capabilities and custom libs. Held in a
// std::deque so element addresses stay stable (pointers are handed to custom
// libs as writer context).
struct DumpBuffer { std::string title; std::string data; };
std::deque<DumpBuffer> &dump_buffers();
std::mutex &dump_buffers_mutex();

// Convenience for internal callers.
void ilog(const char *tag, const char *fmt, ...);

// ---------------------------------------------------------------------------
// overlay.cpp — the ImGui overlay lifecycle and per-frame draw.
// ---------------------------------------------------------------------------
int  overlay_init(JNIEnv *env, jobject activity, ANativeWindow *window);
void overlay_shutdown();
void overlay_new_frame();
void overlay_render();          // draws + eglSwapBuffers wrapper (called from hook)
void overlay_set_window(ANativeWindow *window);
bool overlay_visible();
void overlay_set_visible(bool v);
void overlay_toggle_bubble();   // toggle the collapsed floating bubble
void overlay_request_redraw();
void overlay_draw_bubble();     // the always-available floating toggle

// The ImGui context pointer (opaque here to avoid leaking imgui.h everywhere).
extern ::ImGuiContext *g_imgui_ctx;

// Draw the whole menu UI (tabs, panels, custom-lib list). Called each frame
// between ImGui::NewFrame() and ImGui::Render() from overlay.cpp.
void menu_draw();

// ---------------------------------------------------------------------------
// egl_hook.cpp — EGL/input hooking.
// ---------------------------------------------------------------------------
int  egl_hook_install();        // hooks eglSwapBuffers + input, starts loop
void egl_hook_remove();
void egl_hook_set_display(EGLDisplay d, EGLSurface s); // not required, informational
bool egl_hook_active();

// ---------------------------------------------------------------------------
// input.cpp — touch event plumbing into ImGui.
// ---------------------------------------------------------------------------
void input_queue_motion(int action, float x, float y, int pointer_id);
void input_queue_key(int keycode, bool down);

// ---------------------------------------------------------------------------
// loader.cpp — custom lib loader (/data/adb/ioctl-codex/libs/*.so).
// ---------------------------------------------------------------------------
struct CustomLib {
    std::string path;
    std::string name;
    void       *handle = nullptr;
    bool        loaded = false;
    std::string error;
    int         (*init)(const IoctlCodexCtx *) = nullptr;
    void        (*deinit)() = nullptr;
};

void loader_scan();                         // discover libs on disk
void loader_reload_all();                   // hot-swap: deinit + dlclose + reload
void loader_reload(const std::string &name); // one lib
std::vector<CustomLib> &loader_libs();
std::mutex &loader_mutex();

// ---------------------------------------------------------------------------
// storage_bridge.cpp — stage->publish via the root companion socket.
// ---------------------------------------------------------------------------
int  storage_publish_dir(const char *stage_path, const char *final_rel);
// Low-level request to the root companion; returns reply string (empty on err).
std::string root_request(const std::string &line);

// ---------------------------------------------------------------------------
// cap/*.cpp — the 11 independently-toggleable capabilities.
// Each exposes an apply(ctx) used when the user flips the toggle on.
// ---------------------------------------------------------------------------
void cap_java_dump_apply(const IoctlCodexCtx *ctx);
void cap_native_dump_apply(const IoctlCodexCtx *ctx);
void cap_string_dump_apply(const IoctlCodexCtx *ctx);
void cap_mem_scan_apply(const IoctlCodexCtx *ctx);
void cap_ssl_bypass_apply(const IoctlCodexCtx *ctx);
void cap_tls_capture_apply(const IoctlCodexCtx *ctx);
void cap_crypto_intercept_apply(const IoctlCodexCtx *ctx);
void cap_storage_dump_apply(const IoctlCodexCtx *ctx);
void cap_detection_bypass_apply(const IoctlCodexCtx *ctx);
void cap_frida_runner_apply(const IoctlCodexCtx *ctx);
void cap_il2cpp_dump_apply(const IoctlCodexCtx *ctx);

} // namespace codex
