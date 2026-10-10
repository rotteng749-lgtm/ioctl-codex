// src/menu/overlay.cpp — Dear ImGui overlay lifecycle.
//
// We do NOT use ImGui_ImplAndroid (it wants an ANativeWindow the game process
// does not expose). Instead we drive ImGui IO ourselves:
//   * display size comes from eglQuerySurface on the game's EGL surface
//   * delta time comes from a monotonic clock
//   * touch/keys come from input.cpp (which feeds ImGui's event API)
//   * rendering is ImGui_ImplOpenGL3 into the game's current context
// This keeps the overlay independent of how the game created its surface.

#include "menu.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <chrono>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

namespace codex {

// Defined in egl_hook.cpp — the surface the game last swapped.
extern EGLDisplay g_egl_dpy;
extern EGLSurface g_egl_surf;

// Implemented in input.cpp.
void input_pump(ImGuiIO &io, float scale);
void input_set_capture(bool on);
void input_install_hooks();

namespace {

bool g_visible = false;       // main window shown?
bool g_bubble_visible = true; // floating toggle bubble shown?
bool g_initialized = false;
ImVec2 g_bubble_pos(24.f, 200.f);
std::chrono::steady_clock::time_point g_last_frame;
int g_width = 0, g_height = 0;

// ImClamp lives in imgui_internal.h; keep our own to avoid pulling it in.
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

bool overlay_visible() { return g_visible; }
void overlay_set_visible(bool v) { g_visible = v; }
void overlay_request_redraw() {} // swap-hook draws every frame anyway

void overlay_set_window(ANativeWindow *) {} // unused; kept for API stability

int overlay_init(JNIEnv *, jobject, ANativeWindow *) {
    if (g_initialized) return 0;

    IMGUI_CHECKVERSION();
    g_imgui_ctx = ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // never write imgui.ini

    ImGui::StyleColorsDark();
    // A slightly roomier style for touch.
    ImGuiStyle &st = ImGui::GetStyle();
    st.ScaleAllSizes(2.0f);
    st.TouchExtraPadding = ImVec2(4, 4);

    // Loader for GLES3 (imgui_impl_opengl3_loader.h is bundled).
    if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
        ilog("overlay", "ImGui_ImplOpenGL3_Init failed");
        return -1;
    }

    g_last_frame = std::chrono::steady_clock::now();
    g_initialized = true;
    input_install_hooks();
    ilog("overlay", "ImGui %s initialised", IMGUI_VERSION);
    return 0;
}

void overlay_shutdown() {
    if (!g_initialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext(g_imgui_ctx);
    g_imgui_ctx = nullptr;
    g_initialized = false;
}

void overlay_new_frame() {
    if (!g_initialized) return;

    // Surface size (authoritative for a GLES game).
    int w = 0, h = 0;
    if (g_egl_dpy != EGL_NO_DISPLAY && g_egl_surf != EGL_NO_SURFACE) {
        eglQuerySurface(g_egl_dpy, g_egl_surf, EGL_WIDTH, &w);
        eglQuerySurface(g_egl_dpy, g_egl_surf, EGL_HEIGHT, &h);
    }
    if (w <= 0 || h <= 0) { w = g_width ? g_width : 1080; h = g_height ? g_height : 2400; }
    g_width = w; g_height = h;

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last_frame).count();
    g_last_frame = now;
    if (dt <= 0 || dt > 0.5f) dt = 1.0f / 60.0f;

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = dt;

    input_pump(io, 1.0f);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}

void overlay_render() {
    if (!g_initialized) return;
    menu_draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void overlay_toggle_bubble() {
    g_visible = !g_visible;
    input_set_capture(g_visible);
}

// ---- the floating toggle bubble -------------------------------------------
// Drawn every frame, even when the main window is hidden, so the user always
// has a way back in. A tiny always-on-top window with no decoration.
static void draw_bubble() {
    if (!g_bubble_visible) return;
    ImGuiIO &io = ImGui::GetIO();
    const float r = 44.0f;

    ImGui::SetNextWindowPos(g_bubble_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(r * 2, r * 2));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##codex_bubble", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 center(p.x + r, p.y + r);
    dl->AddCircleFilled(center, r, IM_COL32(30, 30, 30, 200));
    dl->AddCircle(center, r, g_visible ? IM_COL32(90, 200, 90, 255) : IM_COL32(200, 90, 90, 255), 0, 4.0f);
    const char *txt = "IO";
    ImVec2 ts = ImGui::CalcTextSize(txt);
    dl->AddText(ImVec2(center.x - ts.x / 2, center.y - ts.y / 2), IM_COL32(255, 255, 255, 255), txt);

    // Tap toggles the main window; drag moves the bubble.
    ImGui::InvisibleButton("##bubble_btn", ImVec2(r * 2, r * 2));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) {
        ImVec2 d = io.MouseDelta;
        g_bubble_pos.x += d.x;
        g_bubble_pos.y += d.y;
        g_bubble_pos.x = clampf(g_bubble_pos.x, 0.0f, io.DisplaySize.x - r * 2);
        g_bubble_pos.y = clampf(g_bubble_pos.y, 0.0f, io.DisplaySize.y - r * 2);
    }
    if (ImGui::IsItemDeactivated() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        // Treat as a tap (no meaningful drag happened).
        overlay_toggle_bubble();
    }
    ImGui::End();
}

void overlay_draw_bubble() { draw_bubble(); }

} // namespace codex
