// ALooper, AInputQueue, AInputEvent / AKeyEvent / AMotionEvent and
// AChoreographer implementations exported through the fake libandroid.so.

#include "looper_input.h"
#include "host.h"

#include <log.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Input queue
// ---------------------------------------------------------------------------
FakeInputQueue& FakeInputQueue::instance() {
    static FakeInputQueue q;
    return q;
}

void FakeInputQueue::push(std::unique_ptr<FakeInputEvent> ev) {
    std::lock_guard<std::mutex> l(mutex_);
    // Collapse floods of move events so a slow app never falls far behind
    if(pending_.size() > 256)
        pending_.pop_front();
    pending_.push_back(std::move(ev));
}

bool FakeInputQueue::hasEvents() {
    std::lock_guard<std::mutex> l(mutex_);
    return !pending_.empty();
}

int32_t FakeInputQueue::getEvent(FakeInputEvent** out) {
    std::lock_guard<std::mutex> l(mutex_);
    if(pending_.empty())
        return -1;
    inflight_.push_back(std::move(pending_.front()));
    pending_.pop_front();
    *out = inflight_.back().get();
    return 0;
}

void FakeInputQueue::finishEvent(FakeInputEvent* ev) {
    std::lock_guard<std::mutex> l(mutex_);
    auto it = std::find_if(inflight_.begin(), inflight_.end(), [&](auto& p) { return p.get() == ev; });
    if(it != inflight_.end())
        inflight_.erase(it);
}

// ---------------------------------------------------------------------------
// Event construction helpers
// ---------------------------------------------------------------------------
namespace input {

static std::map<int, FakeInputEvent::Pointer> activeTouches;
static std::mutex touchMutex;

void sendKey(int32_t keyCode, int32_t action, int32_t metaState, int32_t source, int32_t deviceId, int32_t repeat) {
    auto ev = std::make_unique<FakeInputEvent>();
    ev->type = AINPUT_EVENT_TYPE_KEY;
    ev->source = source;
    ev->deviceId = deviceId;
    ev->action = action;
    ev->keyCode = keyCode;
    ev->metaState = metaState;
    ev->repeatCount = repeat;
    ev->downTime = ev->eventTime = host::uptimeNanos();
    FakeInputQueue::instance().push(std::move(ev));
}

static std::unique_ptr<FakeInputEvent> motion(int32_t source, int32_t action) {
    auto ev = std::make_unique<FakeInputEvent>();
    ev->type = AINPUT_EVENT_TYPE_MOTION;
    ev->source = source;
    ev->action = action;
    ev->eventTime = host::uptimeNanos();
    ev->downTime = ev->eventTime;
    return ev;
}

void sendTouch(int32_t action, int32_t pointerId, float x, float y) {
    auto ev = motion(AINPUT_SOURCE_TOUCHSCREEN, action);
    ev->pointers.push_back({pointerId, x, y, AMOTION_EVENT_TOOL_TYPE_FINGER});
    FakeInputQueue::instance().push(std::move(ev));
}

void sendMouse(int32_t action, float x, float y, int32_t buttonState, float vscroll, float hscroll) {
    auto ev = motion(AINPUT_SOURCE_MOUSE, action);
    ev->buttonState = buttonState;
    ev->pointers.push_back({0, x, y, AMOTION_EVENT_TOOL_TYPE_MOUSE});
    ev->axes[AMOTION_EVENT_AXIS_VSCROLL] = vscroll;
    ev->axes[AMOTION_EVENT_AXIS_HSCROLL] = hscroll;
    FakeInputQueue::instance().push(std::move(ev));
}

void sendJoystick(int32_t deviceId, const float* axes) {
    auto ev = motion(AINPUT_SOURCE_JOYSTICK, AMOTION_EVENT_ACTION_MOVE);
    ev->deviceId = deviceId;
    ev->pointers.push_back({0, 0, 0, AMOTION_EVENT_TOOL_TYPE_UNKNOWN});
    memcpy(ev->axes, axes, sizeof(ev->axes));
    ev->pointers[0].x = axes[AMOTION_EVENT_AXIS_X];
    ev->pointers[0].y = axes[AMOTION_EVENT_AXIS_Y];
    FakeInputQueue::instance().push(std::move(ev));
}

static void sendTouchState(int32_t action, int changedId) {
    auto ev = motion(AINPUT_SOURCE_TOUCHSCREEN, action);
    int index = 0, i = 0;
    for(auto& t : activeTouches) {
        if(t.first == changedId)
            index = i;
        ev->pointers.push_back(t.second);
        i++;
    }
    if(action == AMOTION_EVENT_ACTION_POINTER_DOWN || action == AMOTION_EVENT_ACTION_POINTER_UP)
        ev->action = action | (index << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    FakeInputQueue::instance().push(std::move(ev));
}

void touchStart(int id, float x, float y) {
    std::lock_guard<std::mutex> l(touchMutex);
    activeTouches[id] = {id, x, y, AMOTION_EVENT_TOOL_TYPE_FINGER};
    sendTouchState(activeTouches.size() == 1 ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_POINTER_DOWN, id);
}

void touchMove(int id, float x, float y) {
    std::lock_guard<std::mutex> l(touchMutex);
    auto it = activeTouches.find(id);
    if(it == activeTouches.end())
        return;
    it->second.x = x;
    it->second.y = y;
    sendTouchState(AMOTION_EVENT_ACTION_MOVE, id);
}

void touchEnd(int id, float x, float y) {
    std::lock_guard<std::mutex> l(touchMutex);
    auto it = activeTouches.find(id);
    if(it == activeTouches.end())
        return;
    it->second.x = x;
    it->second.y = y;
    sendTouchState(activeTouches.size() == 1 ? AMOTION_EVENT_ACTION_UP : AMOTION_EVENT_ACTION_POINTER_UP, id);
    activeTouches.erase(id);
}

}  // namespace input

// ---------------------------------------------------------------------------
// ALooper
// ---------------------------------------------------------------------------
namespace {

struct FakeLooper {
    struct FdEntry {
        int fd, ident, events;
        ALooper_callbackFunc callback;
        void* data;
    };
    std::mutex mutex;
    std::vector<FdEntry> fds;
    int wakePipe[2] = {-1, -1};
    bool hasWindowThread = false;

    FakeLooper() {
        if(pipe2(wakePipe, O_NONBLOCK | O_CLOEXEC) != 0)
            wakePipe[0] = wakePipe[1] = -1;
    }
    ~FakeLooper() {
        if(wakePipe[0] >= 0) {
            close(wakePipe[0]);
            close(wakePipe[1]);
        }
    }

    int addFd(int fd, int ident, int events, ALooper_callbackFunc cb, void* data) {
        std::lock_guard<std::mutex> l(mutex);
        if(!cb && ident < 0)
            return -1;
        for(auto& e : fds) {
            if(e.fd == fd) {
                e = {fd, cb ? ALOOPER_POLL_CALLBACK : ident, events, cb, data};
                return 1;
            }
        }
        fds.push_back({fd, cb ? ALOOPER_POLL_CALLBACK : ident, events, cb, data});
        return 1;
    }

    int removeFd(int fd) {
        std::lock_guard<std::mutex> l(mutex);
        auto it = std::find_if(fds.begin(), fds.end(), [&](auto& e) { return e.fd == fd; });
        if(it == fds.end())
            return 0;
        fds.erase(it);
        return 1;
    }

    void wake() {
        if(wakePipe[1] >= 0) {
            char c = 1;
            (void)!write(wakePipe[1], &c, 1);
        }
    }

    // One iteration. Returns an ident, or ALOOPER_POLL_* constant.
    int pollOnceSlice(int sliceMs, int* outFd, int* outEvents, void** outData) {
        if(host::isWindowThread())
            host::pumpEvents();
        runChoreographerCallbacks();

        auto& q = FakeInputQueue::instance();
        if(q.looper == this && q.hasEvents()) {
            if(q.callback) {
                q.callback(-1, ALOOPER_EVENT_INPUT, q.data);
                return ALOOPER_POLL_CALLBACK;
            }
            if(outFd) *outFd = -1;
            if(outEvents) *outEvents = ALOOPER_EVENT_INPUT;
            if(outData) *outData = q.data;
            return q.ident;
        }

        std::vector<FdEntry> snapshot;
        {
            std::lock_guard<std::mutex> l(mutex);
            snapshot = fds;
        }
        std::vector<pollfd> pfds;
        pfds.push_back({wakePipe[0], POLLIN, 0});
        for(auto& e : snapshot) {
            short ev = 0;
            if(e.events & ALOOPER_EVENT_INPUT) ev |= POLLIN;
            if(e.events & ALOOPER_EVENT_OUTPUT) ev |= POLLOUT;
            pfds.push_back({e.fd, ev, 0});
        }
        int r = poll(pfds.data(), pfds.size(), sliceMs);
        if(r <= 0)
            return ALOOPER_POLL_TIMEOUT;
        if(pfds[0].revents) {
            char buf[64];
            while(read(wakePipe[0], buf, sizeof(buf)) > 0) {}
            return ALOOPER_POLL_WAKE;
        }
        bool calledBack = false;
        for(size_t i = 1; i < pfds.size(); i++) {
            if(!pfds[i].revents)
                continue;
            auto& e = snapshot[i - 1];
            int events = 0;
            if(pfds[i].revents & POLLIN) events |= ALOOPER_EVENT_INPUT;
            if(pfds[i].revents & POLLOUT) events |= ALOOPER_EVENT_OUTPUT;
            if(pfds[i].revents & POLLERR) events |= ALOOPER_EVENT_ERROR;
            if(pfds[i].revents & POLLHUP) events |= ALOOPER_EVENT_HANGUP;
            if(pfds[i].revents & POLLNVAL) events |= ALOOPER_EVENT_INVALID;
            if(e.callback) {
                if(e.callback(e.fd, events, e.data) == 0)
                    removeFd(e.fd);
                calledBack = true;
                continue;
            }
            if(outFd) *outFd = e.fd;
            if(outEvents) *outEvents = events;
            if(outData) *outData = e.data;
            return e.ident;
        }
        return calledBack ? ALOOPER_POLL_CALLBACK : ALOOPER_POLL_TIMEOUT;
    }

    // timeoutMillis: 0 = return immediately, <0 = wait forever
    int pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::milliseconds(timeoutMillis < 0 ? 0 : timeoutMillis);
        while(true) {
            int slice = 0;
            if(timeoutMillis != 0) {
                // wake up regularly to pump window events / input queue
                slice = 8;
                if(timeoutMillis > 0) {
                    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
                    slice = (int)std::max<long long>(0, std::min<long long>(slice, left));
                }
            }
            int r = pollOnceSlice(slice, outFd, outEvents, outData);
            if(r != ALOOPER_POLL_TIMEOUT)
                return r;
            if(timeoutMillis == 0 || (timeoutMillis > 0 && clock::now() >= deadline))
                return ALOOPER_POLL_TIMEOUT;
            if(host::exitRequested() && timeoutMillis < 0)
                return ALOOPER_POLL_TIMEOUT;
        }
    }

    int pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
        using clock = std::chrono::steady_clock;
        auto deadline = clock::now() + std::chrono::milliseconds(timeoutMillis < 0 ? 0 : timeoutMillis);
        while(true) {
            int left = timeoutMillis;
            if(timeoutMillis > 0)
                left = (int)std::max<long long>(0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count());
            int r = pollOnce(left, outFd, outEvents, outData);
            if(r != ALOOPER_POLL_CALLBACK)
                return r;
        }
    }
};

thread_local std::unique_ptr<FakeLooper> threadLooper;

FakeLooper* looperForThread(bool create) {
    if(!threadLooper && create)
        threadLooper = std::make_unique<FakeLooper>();
    return threadLooper.get();
}

// ---------------------------------------------------------------------------
// AChoreographer
// ---------------------------------------------------------------------------
struct FrameCallback {
    void* cb;
    int kind;  // 0 = long, 1 = int64, 2 = vsync
    void* data;
    int64_t due;
};
thread_local std::vector<FrameCallback> frameCallbacks;
struct FakeChoreographer {};
thread_local FakeChoreographer choreographer;

}  // namespace

void runChoreographerCallbacks() {
    if(frameCallbacks.empty())
        return;
    int64_t now = host::uptimeNanos();
    std::vector<FrameCallback> run;
    for(auto it = frameCallbacks.begin(); it != frameCallbacks.end();) {
        if(it->due <= now) {
            run.push_back(*it);
            it = frameCallbacks.erase(it);
        } else {
            ++it;
        }
    }
    for(auto& f : run) {
        if(f.kind == 0)
            ((void (*)(long, void*))f.cb)((long)now, f.data);
        else if(f.kind == 1)
            ((void (*)(int64_t, void*))f.cb)(now, f.data);
        else
            ((void (*)(void*, void*))f.cb)(nullptr, f.data);
    }
}

void looper::prepareCurrentThread() {
    looperForThread(true);
}

void looper::loopUntilExit() {
    auto l = looperForThread(true);
    while(!host::exitRequested()) {
        int fd, events;
        void* data;
        l->pollAll(100, &fd, &events, &data);
    }
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
#define EV(e) ((FakeInputEvent*)(void*)(e))

static ANDROID_PCS float motionX(const AInputEvent* e, size_t i) { return i < EV(e)->pointers.size() ? EV(e)->pointers[i].x : 0.f; }
static ANDROID_PCS float motionY(const AInputEvent* e, size_t i) { return i < EV(e)->pointers.size() ? EV(e)->pointers[i].y : 0.f; }
static ANDROID_PCS float motionAxis(const AInputEvent* e, int32_t axis, size_t i) {
    if(axis == AMOTION_EVENT_AXIS_X) return motionX(e, i);
    if(axis == AMOTION_EVENT_AXIS_Y) return motionY(e, i);
    if(axis == AMOTION_EVENT_AXIS_PRESSURE || axis == AMOTION_EVENT_AXIS_SIZE) return 1.f;
    return axis >= 0 && axis < 48 ? EV(e)->axes[axis] : 0.f;
}
static ANDROID_PCS float motionOne(const AInputEvent*, size_t) { return 1.f; }
static ANDROID_PCS float motionZero(const AInputEvent*, size_t) { return 0.f; }
static ANDROID_PCS float motionZeroE(const AInputEvent*) { return 0.f; }
static ANDROID_PCS float motionOneE(const AInputEvent*) { return 1.f; }
static ANDROID_PCS float motionHistX(const AInputEvent* e, size_t i, size_t) { return motionX(e, i); }
static ANDROID_PCS float motionHistY(const AInputEvent* e, size_t i, size_t) { return motionY(e, i); }
static ANDROID_PCS float motionHistAxis(const AInputEvent* e, int32_t a, size_t i, size_t) { return motionAxis(e, a, i); }

void fakelibs::installLooperAndInput(std::unordered_map<std::string, void*>& s) {
    // ALooper
    s["ALooper_forThread"] = (void*)+[]() -> ALooper* { return (ALooper*)looperForThread(false); };
    s["ALooper_prepare"] = (void*)+[](int) -> ALooper* { return (ALooper*)looperForThread(true); };
    s["ALooper_acquire"] = (void*)+[](ALooper*) {};
    s["ALooper_release"] = (void*)+[](ALooper*) {};
    s["ALooper_wake"] = (void*)+[](ALooper* l) { ((FakeLooper*)l)->wake(); };
    s["ALooper_addFd"] = (void*)+[](ALooper* l, int fd, int ident, int events, ALooper_callbackFunc cb, void* data) -> int {
        return ((FakeLooper*)l)->addFd(fd, ident, events, cb, data);
    };
    s["ALooper_removeFd"] = (void*)+[](ALooper* l, int fd) -> int { return ((FakeLooper*)l)->removeFd(fd); };
    s["ALooper_pollOnce"] = (void*)+[](int t, int* fd, int* ev, void** data) -> int {
        return looperForThread(true)->pollOnce(t, fd, ev, data);
    };
    s["ALooper_pollAll"] = (void*)+[](int t, int* fd, int* ev, void** data) -> int {
        return looperForThread(true)->pollAll(t, fd, ev, data);
    };

    // AInputQueue
    s["AInputQueue_attachLooper"] = (void*)+[](AInputQueue*, ALooper* l, int ident, ALooper_callbackFunc cb, void* data) {
        auto& q = FakeInputQueue::instance();
        q.looper = l;
        q.ident = ident;
        q.callback = cb;
        q.data = data;
    };
    s["AInputQueue_detachLooper"] = (void*)+[](AInputQueue*) {
        FakeInputQueue::instance().looper = nullptr;
    };
    s["AInputQueue_hasEvents"] = (void*)+[](AInputQueue*) -> int32_t { return FakeInputQueue::instance().hasEvents() ? 1 : 0; };
    s["AInputQueue_getEvent"] = (void*)+[](AInputQueue*, AInputEvent** out) -> int32_t {
        return FakeInputQueue::instance().getEvent((FakeInputEvent**)out);
    };
    s["AInputQueue_preDispatchEvent"] = (void*)+[](AInputQueue*, AInputEvent*) -> int32_t { return 0; };
    s["AInputQueue_finishEvent"] = (void*)+[](AInputQueue*, AInputEvent* e, int) { FakeInputQueue::instance().finishEvent(EV(e)); };

    // AInputEvent
    s["AInputEvent_getType"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->type; };
    s["AInputEvent_getDeviceId"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->deviceId; };
    s["AInputEvent_getSource"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->source; };
    s["AInputEvent_release"] = (void*)+[](const AInputEvent*) {};

    // AKeyEvent
    s["AKeyEvent_getAction"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->action; };
    s["AKeyEvent_getFlags"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->flags; };
    s["AKeyEvent_getKeyCode"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->keyCode; };
    s["AKeyEvent_getScanCode"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->scanCode; };
    s["AKeyEvent_getMetaState"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->metaState; };
    s["AKeyEvent_getRepeatCount"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->repeatCount; };
    s["AKeyEvent_getDownTime"] = (void*)+[](const AInputEvent* e) -> int64_t { return EV(e)->downTime; };
    s["AKeyEvent_getEventTime"] = (void*)+[](const AInputEvent* e) -> int64_t { return EV(e)->eventTime; };

    // AMotionEvent
    s["AMotionEvent_getAction"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->action; };
    s["AMotionEvent_getFlags"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->flags; };
    s["AMotionEvent_getMetaState"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->metaState; };
    s["AMotionEvent_getButtonState"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->buttonState; };
    s["AMotionEvent_getActionButton"] = (void*)+[](const AInputEvent* e) -> int32_t { return EV(e)->buttonState; };
    s["AMotionEvent_getEdgeFlags"] = (void*)+[](const AInputEvent*) -> int32_t { return 0; };
    s["AMotionEvent_getClassification"] = (void*)+[](const AInputEvent*) -> int32_t { return 0; };
    s["AMotionEvent_getDownTime"] = (void*)+[](const AInputEvent* e) -> int64_t { return EV(e)->downTime; };
    s["AMotionEvent_getEventTime"] = (void*)+[](const AInputEvent* e) -> int64_t { return EV(e)->eventTime; };
    s["AMotionEvent_getXOffset"] = (void*)motionZeroE;
    s["AMotionEvent_getYOffset"] = (void*)motionZeroE;
    s["AMotionEvent_getXPrecision"] = (void*)motionOneE;
    s["AMotionEvent_getYPrecision"] = (void*)motionOneE;
    s["AMotionEvent_getPointerCount"] = (void*)+[](const AInputEvent* e) -> size_t { return EV(e)->pointers.size(); };
    s["AMotionEvent_getPointerId"] = (void*)+[](const AInputEvent* e, size_t i) -> int32_t {
        return i < EV(e)->pointers.size() ? EV(e)->pointers[i].id : 0;
    };
    s["AMotionEvent_getToolType"] = (void*)+[](const AInputEvent* e, size_t i) -> int32_t {
        return i < EV(e)->pointers.size() ? EV(e)->pointers[i].toolType : 0;
    };
    s["AMotionEvent_getRawX"] = (void*)motionX;
    s["AMotionEvent_getRawY"] = (void*)motionY;
    s["AMotionEvent_getX"] = (void*)motionX;
    s["AMotionEvent_getY"] = (void*)motionY;
    s["AMotionEvent_getPressure"] = (void*)motionOne;
    s["AMotionEvent_getSize"] = (void*)motionOne;
    s["AMotionEvent_getTouchMajor"] = (void*)motionOne;
    s["AMotionEvent_getTouchMinor"] = (void*)motionOne;
    s["AMotionEvent_getToolMajor"] = (void*)motionOne;
    s["AMotionEvent_getToolMinor"] = (void*)motionOne;
    s["AMotionEvent_getOrientation"] = (void*)motionZero;
    s["AMotionEvent_getAxisValue"] = (void*)motionAxis;
    s["AMotionEvent_getHistorySize"] = (void*)+[](const AInputEvent*) -> size_t { return 0; };
    s["AMotionEvent_getHistoricalEventTime"] = (void*)+[](const AInputEvent* e, size_t) -> int64_t { return EV(e)->eventTime; };
    s["AMotionEvent_getHistoricalX"] = (void*)motionHistX;
    s["AMotionEvent_getHistoricalY"] = (void*)motionHistY;
    s["AMotionEvent_getHistoricalRawX"] = (void*)motionHistX;
    s["AMotionEvent_getHistoricalRawY"] = (void*)motionHistY;
    s["AMotionEvent_getHistoricalAxisValue"] = (void*)motionHistAxis;

    // AChoreographer
    s["AChoreographer_getInstance"] = (void*)+[]() -> void* { return &choreographer; };
    s["AChoreographer_postFrameCallback"] = (void*)+[](void*, void* cb, void* data) {
        frameCallbacks.push_back({cb, 0, data, host::uptimeNanos() + 16000000});
    };
    s["AChoreographer_postFrameCallbackDelayed"] = (void*)+[](void*, void* cb, void* data, long delayMs) {
        frameCallbacks.push_back({cb, 0, data, host::uptimeNanos() + 16000000 + (int64_t)delayMs * 1000000});
    };
    s["AChoreographer_postFrameCallback64"] = (void*)+[](void*, void* cb, void* data) {
        frameCallbacks.push_back({cb, 1, data, host::uptimeNanos() + 16000000});
    };
    s["AChoreographer_postFrameCallbackDelayed64"] = (void*)+[](void*, void* cb, void* data, uint32_t delayMs) {
        frameCallbacks.push_back({cb, 1, data, host::uptimeNanos() + 16000000 + (int64_t)delayMs * 1000000});
    };
    s["AChoreographer_registerRefreshRateCallback"] = (void*)+[](void*, void*, void*) {};
    s["AChoreographer_unregisterRefreshRateCallback"] = (void*)+[](void*, void*, void*) {};
}
