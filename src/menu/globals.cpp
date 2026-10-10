// src/menu/globals.cpp — process-wide state for the overlay, plus the small
// host services (log, writer, publish) exposed through the stable ABI.

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "menu.h"

#define LOG_TAG "ioctl-codex"

namespace codex {

TargetInfo g_target;
IoctlCodexCtx g_ctx{};
JavaVM *g_vm = nullptr;
::ImGuiContext *g_imgui_ctx = nullptr;

// ----- log ring ------------------------------------------------------------
namespace {
std::vector<LogLine> g_logs;
std::mutex g_log_mutex;
} // namespace

std::vector<LogLine> &log_lines() { return g_logs; }
std::mutex &log_mutex() { return g_log_mutex; }

void log_push(int level, const char *tag, const char *msg) {
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        if (g_logs.size() > 2000) g_logs.erase(g_logs.begin(), g_logs.begin() + 500);
        g_logs.push_back({level, tag ? tag : "", msg ? msg : ""});
    }
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case IOCTL_CODEX_LOG_DEBUG: prio = ANDROID_LOG_DEBUG; break;
        case IOCTL_CODEX_LOG_WARN:  prio = ANDROID_LOG_WARN;  break;
        case IOCTL_CODEX_LOG_ERROR: prio = ANDROID_LOG_ERROR; break;
        default: prio = ANDROID_LOG_INFO; break;
    }
    __android_log_print(prio, LOG_TAG, "[%s] %s", tag ? tag : "", msg ? msg : "");
}

void ilog(const char *tag, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_push(IOCTL_CODEX_LOG_INFO, tag, buf);
}

// ----- writer implementation ----------------------------------------------
namespace {

// A writer appends to a named in-memory buffer shown in the Dump tab. For v1 we
// keep the last buffer per title; the UI reads them directly. std::deque keeps
// element addresses stable across push_back, so the pointers handed to custom
// libs stay valid.
std::deque<DumpBuffer> g_buffers;
std::mutex g_buffers_mutex;

int writer_write(void *ctx, const char *data, size_t len) {
    auto *b = reinterpret_cast<DumpBuffer *>(ctx);
    if (!b || !data) return -1;
    std::lock_guard<std::mutex> lk(g_buffers_mutex);
    b->data.append(data, len);
    if (b->data.size() > (16u << 20)) b->data.erase(0, b->data.size() - (16u << 20));
    return 0;
}

IoctlCodexWriter *open_writer(const char *title) {
    std::lock_guard<std::mutex> lk(g_buffers_mutex);
    // Reuse an existing buffer with the same title (so re-running a dump
    // appends rather than leaking a new slot).
    for (auto &b : g_buffers) {
        if (b.title == (title ? title : "")) {
            auto *w = new IoctlCodexWriter{&b, writer_write};
            return w;
        }
    }
    g_buffers.push_back({title ? title : "Dump", ""});
    auto *w = new IoctlCodexWriter{&g_buffers.back(), writer_write};
    return w;
}

void close_writer(IoctlCodexWriter *w) {
    delete w; // the underlying buffer stays; UI reads it
}

} // namespace

std::deque<DumpBuffer> &dump_buffers() { return g_buffers; }
std::mutex &dump_buffers_mutex() { return g_buffers_mutex; }

// ----- host service wiring -------------------------------------------------
void ctx_init_services() {
    g_ctx.api_version = IOCTL_CODEX_API_VERSION;
    g_ctx.struct_size = sizeof(IoctlCodexCtx);
    g_ctx.package_name = g_target.package_name.c_str();
    g_ctx.version_name = g_target.version_name.empty() ? nullptr : g_target.version_name.c_str();
    g_ctx.version_code = g_target.version_code;
    g_ctx.app_data_dir = g_target.app_data_dir.c_str();
    g_ctx.target_uid = g_target.uid;
    g_ctx.log = [](int level, const char *tag, const char *msg) { log_push(level, tag, msg); };
    g_ctx.enabled_caps = 0;
    g_ctx.request_redraw = []() { overlay_request_redraw(); };
    g_ctx.open_writer = [](const char *title) { return open_writer(title); };
    g_ctx.close_writer = [](IoctlCodexWriter *w) { close_writer(w); };
    g_ctx.dump_root = "/sdcard/panxcz-dumper";
    g_ctx.stage_root = g_target.stage_root.c_str();
    g_ctx.publish_dir = [](const char *stage, const char *rel) {
        return storage_publish_dir(stage, rel);
    };
}

} // namespace codex
