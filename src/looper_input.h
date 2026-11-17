#pragma once

#include <android/input.h>
#include <android/looper.h>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

// One Android input event (key or motion), handed to the app as AInputEvent*.
struct FakeInputEvent {
    struct Pointer {
        int32_t id = 0;
        float x = 0, y = 0;
        int32_t toolType = AMOTION_EVENT_TOOL_TYPE_FINGER;
    };

    int32_t type = AINPUT_EVENT_TYPE_KEY;
    int32_t source = AINPUT_SOURCE_KEYBOARD;
    int32_t deviceId = 1;
    int32_t action = 0;
    int32_t flags = 0;
    int32_t metaState = 0;
    int64_t downTime = 0, eventTime = 0;
    // key
    int32_t keyCode = 0, scanCode = 0, repeatCount = 0;
    // motion
    int32_t buttonState = 0;
    std::vector<Pointer> pointers;
    float axes[48] = {};
};

// AInputQueue implementation. Events are produced on the window thread and
// consumed by the app thread through ALooper/AInputQueue_getEvent.
class FakeInputQueue {
public:
    static FakeInputQueue& instance();

    void push(std::unique_ptr<FakeInputEvent> ev);
    bool hasEvents();
    int32_t getEvent(FakeInputEvent** out);
    void finishEvent(FakeInputEvent* ev);

    // looper attachment (AInputQueue_attachLooper)
    void* looper = nullptr;
    int ident = 0;
    ALooper_callbackFunc callback = nullptr;
    void* data = nullptr;

private:
    std::mutex mutex_;
    std::deque<std::unique_ptr<FakeInputEvent>> pending_;
    std::vector<std::unique_ptr<FakeInputEvent>> inflight_;
};

namespace input {
// Helpers used by the window callbacks.
void sendKey(int32_t keyCode, int32_t action, int32_t metaState, int32_t source = AINPUT_SOURCE_KEYBOARD, int32_t deviceId = 1, int32_t repeat = 0);
void sendTouch(int32_t action, int32_t pointerId, float x, float y);  // single touch emulation
void sendMouse(int32_t action, float x, float y, int32_t buttonState, float vscroll = 0, float hscroll = 0);
void sendJoystick(int32_t deviceId, const float* axes /* indexed by AMOTION_EVENT_AXIS_* */);
void touchStart(int id, float x, float y);
void touchMove(int id, float x, float y);
void touchEnd(int id, float x, float y);
}  // namespace input

// Per-thread ALooper
void runChoreographerCallbacks();
namespace looper {
// Gives the calling thread an ALooper (like the Android UI thread has one).
void prepareCurrentThread();
// Runs the calling thread's looper until exit is requested (Android UI thread).
void loopUntilExit();
}  // namespace looper
