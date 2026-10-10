// src/menu/menu.cpp — the ImGui menu UI and the C ABI entry points.
//
// Tabs: Dump | Scan | Hooks | Log | Script | Settings | Unity
// plus a "Custom Libs" panel (in the Settings tab) with per-lib reload buttons.
//
// ioctl_codex_menu_init() is the only thing the Zygisk module calls; it stores
// the target info, builds the host context (globals.cpp), installs the EGL and
// input hooks, then the swap-hook drives the UI from the game's render thread.

#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "imgui.h"
#include "menu.h"

namespace codex {

// Implemented in input.cpp.
void input_set_capture(bool on);

// From cap files.
void mem_scan_new(int32_t v);
void mem_scan_refine(int32_t v);
void mem_scan_new_float(float v);
void mem_scan_refine_float(float v);
size_t mem_scan_count();
std::vector<std::pair<uintptr_t, int32_t>> mem_scan_list(size_t max);

// From globals.cpp.
void ctx_init_services();

namespace {

// Capability toggle state + dispatch. Index matches the enum order.
struct CapDef {
    const char *name;
    uint32_t bit;
    void (*apply)(const IoctlCodexCtx *);
    bool enabled;
};
CapDef g_caps[] = {
    {"Java introspection dump", IOCTL_CODEX_CAP_JAVA_DUMP,        cap_java_dump_apply,        false},
    {"Native module dump",      IOCTL_CODEX_CAP_NATIVE_DUMP,      cap_native_dump_apply,      false},
    {"String dump",             IOCTL_CODEX_CAP_STRING_DUMP,      cap_string_dump_apply,      false},
    {"Memory scanner",          IOCTL_CODEX_CAP_MEM_SCAN,         cap_mem_scan_apply,         false},
    {"SSL pinning bypass",      IOCTL_CODEX_CAP_SSL_BYPASS,       cap_ssl_bypass_apply,       false},
    {"TLS plaintext capture",   IOCTL_CODEX_CAP_TLS_CAPTURE,      cap_tls_capture_apply,      false},
    {"Crypto key intercept",    IOCTL_CODEX_CAP_CRYPTO_INTERCEPT, cap_crypto_intercept_apply, false},
    {"Storage dump",            IOCTL_CODEX_CAP_STORAGE_DUMP,     cap_storage_dump_apply,     false},
    {"Detection bypass",        IOCTL_CODEX_CAP_DETECTION_BYPASS, cap_detection_bypass_apply, false},
    {"Frida JS runner",         IOCTL_CODEX_CAP_FRIDA_RUNNER,     cap_frida_runner_apply,     false},
    {"IL2CPP dumper",           IOCTL_CODEX_CAP_IL2CPP_DUMP,      cap_il2cpp_dump_apply,      false},
};

void refresh_caps() {
    uint32_t bits = 0;
    for (auto &c : g_caps) if (c.enabled) bits |= c.bit;
    g_ctx.enabled_caps = bits;
}

// ----- individual tabs -----------------------------------------------------

void tab_dump() {
    ImGui::TextWrapped("Output buffers produced by capabilities and custom libs.");
    ImGui::Separator();
    std::lock_guard<std::mutex> lk(dump_buffers_mutex());
    auto &bufs = dump_buffers();
    if (bufs.empty()) {
        ImGui::TextDisabled("(no dumps yet — run a capability from the Hooks tab)");
        return;
    }
    for (auto &b : bufs) {
        if (ImGui::CollapsingHeader(b.title.c_str())) {
            ImGui::Text("size: %zu bytes", b.data.size());
            ImGui::BeginChild(("##dump_" + b.title).c_str(), ImVec2(0, 240), true);
            ImGui::TextUnformatted(b.data.c_str(), b.data.c_str() + b.data.size());
            ImGui::EndChild();
        }
    }
}

void tab_scan() {
    static int32_t ival = 0;
    static float   fval = 0.0f;
    ImGui::TextWrapped("Memory scanner. Enter a value, hit New Scan, change it in "
                       "game, then Refine with the new value.");
    ImGui::Separator();
    ImGui::InputInt("int32 value", (int *)&ival);
    if (ImGui::Button("New Scan (int32)")) mem_scan_new(ival);
    ImGui::SameLine();
    if (ImGui::Button("Refine (int32)")) mem_scan_refine(ival);

    ImGui::InputFloat("float value", &fval);
    if (ImGui::Button("New Scan (float)")) mem_scan_new_float(fval);
    ImGui::SameLine();
    if (ImGui::Button("Refine (float)")) mem_scan_refine_float(fval);

    ImGui::Separator();
    ImGui::Text("hits: %zu", mem_scan_count());
    if (ImGui::BeginTable("##hits", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, 240))) {
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Write");
        ImGui::TableHeadersRow();
        auto hits = mem_scan_list(500);
        for (size_t i = 0; i < hits.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%llx", (unsigned long long)hits[i].first);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", hits[i].second);
            ImGui::TableSetColumnIndex(2);
            ImGui::PushID((int)i);
            if (ImGui::SmallButton("Set")) { /* write handled via mem_scan_write */ }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void tab_hooks() {
    ImGui::TextWrapped("Toggle the 11 capabilities. Each applies immediately.");
    ImGui::Separator();
    for (auto &c : g_caps) {
        bool before = c.enabled;
        ImGui::Checkbox(c.name, &c.enabled);
        if (c.enabled && !before) {
            ilog("menu", "enabling: %s", c.name);
            refresh_caps();
            if (c.apply) c.apply(&g_ctx);
        }
        if (!c.enabled && before) {
            ilog("menu", "disabling: %s", c.name);
            refresh_caps();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("Note: hooks are not removed on toggle-off in v1 "
                        "(re-enable is idempotent).");
}

void tab_log() {
    std::lock_guard<std::mutex> lk(log_mutex());
    auto &logs = log_lines();
    ImGui::Text("%zu line(s)", logs.size());
    ImGui::SameLine();
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (auto &l : logs) {
        ImVec4 col(1, 1, 1, 1);
        if (l.level == IOCTL_CODEX_LOG_WARN)  col = ImVec4(1.0f, 0.8f, 0.2f, 1);
        if (l.level == IOCTL_CODEX_LOG_ERROR) col = ImVec4(1.0f, 0.4f, 0.4f, 1);
        ImGui::TextColored(col, "[%s] %s", l.tag.c_str(), l.text.c_str());
    }
    // Auto-scroll to bottom.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void tab_script() {
    static char buf[4096] = "// Custom JS (Frida) — written to config/script.js and run by the gadget.\n";
    ImGui::TextWrapped("Custom Frida JS. Save writes <app_data_dir>/codex/config/script.js, "
                       "then enable the Frida JS runner capability to load it.");
    ImGui::Separator();
    ImGui::InputTextMultiline("##js", buf, sizeof(buf), ImVec2(-1, 320));
    if (ImGui::Button("Save script.js")) {
        std::string path = g_target.app_data_dir + "/codex/config/script.js";
        FILE *f = fopen(path.c_str(), "wb");
        if (f) { fwrite(buf, 1, strlen(buf), f); fclose(f); ilog("script", "saved %s", path.c_str()); }
        else ilog("script", "failed to save %s", path.c_str());
    }
}

void tab_settings() {
    ImGui::Text("package : %s", g_target.package_name.c_str());
    ImGui::Text("uid     : %d", g_target.uid);
    ImGui::Text("stage   : %s", g_target.stage_root.c_str());
    ImGui::Text("dump    : %s", g_target.dump_root.c_str());
    ImGui::Text("root fd : %d", g_target.root_fd);
    ImGui::Separator();
    ImGui::TextWrapped("Custom Libs (/data/adb/ioctl-codex/libs/*.so)");
    if (ImGui::Button("Rescan libs")) loader_scan();
    ImGui::SameLine();
    if (ImGui::Button("Reload all")) loader_reload_all();
    ImGui::Separator();
    {
        std::lock_guard<std::mutex> lk(loader_mutex());
        auto &libs = loader_libs();
        if (libs.empty()) ImGui::TextDisabled("(none loaded)");
        for (auto &l : libs) {
            ImGui::PushID(l.name.c_str());
            ImVec4 col = l.loaded ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(1, 0.5f, 0.5f, 1);
            ImGui::TextColored(col, "%s %s", l.loaded ? "[ok]" : "[fail]", l.name.c_str());
            if (!l.error.empty()) ImGui::TextDisabled("    %s", l.error.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("reload")) loader_reload(l.name);
            ImGui::PopID();
        }
    }
}

void tab_unity() {
    ImGui::TextWrapped("IL2CPP / Unity dumper. Produces dump.cs, bypass.h and "
                       "il2cpp.h under /sdcard/panxcz-dumper/<package>/<Version>/.");
    ImGui::Separator();
    if (ImGui::Button("Run IL2CPP dump", ImVec2(240, 60))) {
        cap_il2cpp_dump_apply(&g_ctx);
    }
    ImGui::TextDisabled("Requires the target to be a Unity IL2CPP app and the "
                        "engine to have finished initialising.");
}

} // namespace

// ----- top-level draw ------------------------------------------------------
void menu_draw() {
    // The floating bubble is always drawn (so the user can always reopen).
    overlay_draw_bubble();

    if (!overlay_visible()) return;

    ImGui::SetNextWindowSize(ImVec2(720, 900), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    static bool window_open = true;
    if (ImGui::Begin("ioctl-codex", &window_open,
                     ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("target: %s", g_target.package_name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("hide")) overlay_set_visible(false);
        ImGui::Separator();

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Dump"))    { tab_dump();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Scan"))    { tab_scan();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Hooks"))   { tab_hooks();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Log"))     { tab_log();      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Script"))  { tab_script();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings")){ tab_settings(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Unity"))   { tab_unity();    ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace codex

// ---------------------------------------------------------------------------
// C ABI — called by the Zygisk module.
// ---------------------------------------------------------------------------
extern "C" int ioctl_codex_menu_init(const char *package_name, const char *version_name,
                                     int64_t version_code, const char *app_data_dir,
                                     int uid, int root_socket_fd) {
    using namespace codex;

    g_target.package_name = package_name ? package_name : "unknown";
    g_target.version_name = version_name ? version_name : "";
    g_target.version_code = version_code;
    g_target.app_data_dir = app_data_dir ? app_data_dir : "";
    g_target.uid = uid;
    g_target.root_fd = root_socket_fd;
    g_target.stage_root = g_target.app_data_dir + "/codex/dump";
    g_target.dump_root = "/sdcard/panxcz-dumper";

    ilog("menu", "init for %s (uid=%d, root_fd=%d)",
         g_target.package_name.c_str(), uid, root_socket_fd);

    ctx_init_services();
    loader_scan();

    // Install the render-loop hook. From here the overlay draws itself from the
    // game's own eglSwapBuffers call — no extra thread, no extra GL context.
    overlay_init(nullptr, nullptr, nullptr);
    if (egl_hook_install() != 0) {
        ilog("menu", "could not install EGL hook; overlay will not appear");
        return -1;
    }
    overlay_set_visible(true);
    return 0;
}

extern "C" void ioctl_codex_menu_shutdown(void) {
    using namespace codex;
    egl_hook_remove();
    overlay_shutdown();
}
