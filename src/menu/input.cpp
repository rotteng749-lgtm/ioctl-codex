// src/menu/input.cpp — touch/keyboard plumbing into ImGui.
//
// Capture path: the NDK AInputQueue (AInputQueue_getEvent / preDispatchEvent).
// This is the stable, documented input path used by NativeActivity, SDL and
// most game engines. We parse events with the AMotionEvent_* / AKeyEvent_*
// NDK accessors and push them into a small thread-safe queue; ImGui consumes
// the queue at NewFrame time (input_pump), which avoids calling ImGui from the
// input thread.
//
// When the overlay is visible we still let the events through to the game (we
// do not consume them) so a mis-tap cannot soft-lock the game; ImGui simply
// also sees them. This is the safer default for v1.

#include "menu.h"

#include <android/input.h>
#include <android/keycodes.h>

#include <deque>
#include <mutex>
#include <vector>

#include "imgui.h"
#include "cap/cap_util.h"   // hook_sym / shadowhook wrappers

namespace codex {

namespace {

struct Event {
    enum Kind { Motion, Key } kind;
    int action;       // AMOTION_EVENT_ACTION_* / AKEY_EVENT_ACTION_*
    float x, y;
    int pointer_id;
    int keycode;
};

std::deque<Event> g_queue;
std::mutex g_queue_mutex;
bool g_capture = false;

// Touch → ImGui. We map the primary pointer to MousePos/MouseDown and ignore
// multi-touch for v1 (single-finger UI).
void feed_motion(ImGuiIO &io, const Event &e) {
    io.MousePos = ImVec2(e.x, e.y);
    switch (e.action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            io.MouseDown[0] = true;
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            io.MouseDown[0] = false;
            break;
        default:
            break;
    }
}

void feed_key(ImGuiIO &io, const Event &e) {
    bool down = (e.action == AKEY_EVENT_ACTION_DOWN);
    // Minimal mapping; extend as needed for the Script tab.
    // ImGui 1.92 uses the AddKeyEvent() API (the old io.KeysDown[] array is
    // gone). Map a couple of Android keys onto ImGui keys.
    switch (e.keycode) {
        case AKEYCODE_BACK:
            io.AddKeyEvent(ImGuiKey_Escape, down);
            break;
        case AKEYCODE_ENTER:
            io.AddKeyEvent(ImGuiKey_Enter, down);
            break;
        case AKEYCODE_DEL:
            io.AddKeyEvent(ImGuiKey_Backspace, down);
            break;
        default:
            break;
    }
}

int32_t (*orig_getEvent)(AInputQueue *, AInputEvent **) = nullptr;

int32_t my_getEvent(AInputQueue *queue, AInputEvent **outEvent) {
    int32_t rc = orig_getEvent ? orig_getEvent(queue, outEvent) : -1;
    if (rc < 0 || !outEvent || !*outEvent) return rc;

    AInputEvent *ev = *outEvent;
    int32_t type = AInputEvent_getType(ev);
    std::lock_guard<std::mutex> lk(g_queue_mutex);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
        float x = AMotionEvent_getX(ev, 0);
        float y = AMotionEvent_getY(ev, 0);
        int32_t pid = AMotionEvent_getPointerId(ev, 0);
        g_queue.push_back({Event::Motion, action, x, y, pid, 0});
    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t action = AKeyEvent_getAction(ev);
        int32_t code = AKeyEvent_getKeyCode(ev);
        g_queue.push_back({Event::Key, action, 0, 0, 0, code});
    }
    if (g_queue.size() > 512) g_queue.pop_front();
    return rc;
}

} // namespace

void input_set_capture(bool on) { g_capture = on; }

void input_install_hooks() {
    if (hook_sym("libandroid.so", "AInputQueue_getEvent",
                 (void *)my_getEvent, (void **)&orig_getEvent) != nullptr) {
        ilog("input", "AInputQueue_getEvent hooked");
    } else {
        ilog("input", "input hook unavailable (game may not use AInputQueue)");
    }
}

// Called from overlay.cpp each frame, before ImGui::NewFrame().
void input_pump(ImGuiIO &io, float) {
    std::vector<Event> batch;
    {
        std::lock_guard<std::mutex> lk(g_queue_mutex);
        batch.assign(g_queue.begin(), g_queue.end());
        g_queue.clear();
    }
    for (const auto &e : batch) {
        if (e.kind == Event::Motion) feed_motion(io, e);
        else feed_key(io, e);
    }
    // If the overlay is not capturing, keep the pointer "up" so ImGui does not
    // steal hover state from a game that is still receiving touches.
    if (!g_capture) io.MouseDown[0] = false;
}

} // namespace codex
