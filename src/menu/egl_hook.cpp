// src/menu/egl_hook.cpp — the render-loop hook.
//
// The overlay must draw into the same GL context the game uses, on the same
// thread. The single reliable chokepoint for "a frame just finished" is
// eglSwapBuffers. We inline-hook it (ShadowHook); our replacement runs BEFORE
// the real swap, so the menu is composited on top of the game's frame for that
// same buffer. We do NOT start our own GL thread — everything happens on the
// game's render thread, which is what makes the overlay correct and cheap.
//
// GL state is saved/restored around our draw so the game sees an untouched
// context afterwards. When the overlay is hidden we do nothing but call through
// (near-zero overhead — the whole point of hooking swap rather than every draw).

#include "menu.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>

#include "cap/cap_util.h"   // hook_sym / shadowhook wrappers

namespace codex {

// The display/surface the game last swapped. Read by overlay.cpp to size the
// ImGui framebuffer. Non-static so it can be shared.
EGLDisplay g_egl_dpy = EGL_NO_DISPLAY;
EGLSurface g_egl_surf = EGL_NO_SURFACE;

namespace {

EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
void *g_stub = nullptr;
bool g_installed = false;

// Minimal GL state we disturb and must restore.
struct GlState {
    GLint program = 0;
    GLint active_texture = 0;
    GLint texture_binding_2d = 0;
    GLint array_buffer = 0;
    GLint viewport[4] = {0, 0, 0, 0};
    GLint blend_src_rgb = 0, blend_dst_rgb = 0;
    GLint blend_src_a = 0, blend_dst_a = 0;
    GLboolean blend = GL_FALSE;
    GLboolean scissor = GL_FALSE;
    GLboolean cull = GL_FALSE;
    GLint depth_test = 0;
    GLint framebuffer = 0;
};

void save_gl(GlState &s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.texture_binding_2d);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.array_buffer);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blend_dst_a);
    s.blend = glIsEnabled(GL_BLEND);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.cull = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_DEPTH_TEST, &s.depth_test);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);
}

void restore_gl(const GlState &s) {
    glUseProgram(s.program);
    glBindBuffer(GL_ARRAY_BUFFER, s.array_buffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s.texture_binding_2d);
    glActiveTexture(s.active_texture);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glBlendFuncSeparate(s.blend_src_rgb, s.blend_dst_rgb, s.blend_src_a, s.blend_dst_a);
    if (s.blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s.scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s.cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (s.depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
}

EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    g_egl_dpy = dpy;
    g_egl_surf = surf;

    if (overlay_visible()) {
        GlState st;
        save_gl(st);
        overlay_new_frame();
        overlay_render();
        restore_gl(st);
    }
    return orig_eglSwapBuffers ? orig_eglSwapBuffers(dpy, surf) : EGL_FALSE;
}

} // namespace

bool egl_hook_active() { return g_installed; }

int egl_hook_install() {
    if (g_installed) return 0;

    // eglSwapBuffers lives in libEGL.so; hook by name.
    void *stub = hook_sym("libEGL.so", "eglSwapBuffers",
                          (void *)my_eglSwapBuffers, (void **)&orig_eglSwapBuffers);
    if (!stub) {
        ilog("egl", "failed to hook eglSwapBuffers");
        return -1;
    }
    g_stub = stub;
    g_installed = true;
    ilog("egl", "eglSwapBuffers hooked; overlay will composite each frame");
    return 0;
}

void egl_hook_remove() {
    if (!g_installed || !g_stub) return;
    shadowhook_unhook(g_stub);
    g_stub = nullptr;
    g_installed = false;
}

} // namespace codex
