#include "host.h"
#include "keymap.h"
#include "looper_input.h"
#include "ue4_support.h"

#include <game_window_manager.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_gamepad.h>
#include <log.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <array>
#include <atomic>
#include <map>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <time.h>
#include <unistd.h>
#ifdef USE_ARMHF_SUPPORT
#include "armhf_support.h"
#endif

LaunchOptions host::opts;
ANativeActivity host::activity;
ANativeActivityCallbacks host::callbacks;

namespace {
std::shared_ptr<GameWindow> gameWindow;
std::thread::id windowThread;
std::atomic<bool> exiting{false};
std::atomic<bool> lifecycleStarted{false};
bool leftDown = false;
int32_t mouseButtons = 0;
std::mutex gamepadMutex;
std::map<int, host::GamepadInfo> gamepads;
std::map<int, std::array<float, 48>> gamepadAxes;
std::unordered_map<std::string, void*> glOverrides;

#define CB(name, ...) \
    do { if(host::callbacks.name) host::callbacks.name(&host::activity, ##__VA_ARGS__); } while(0)

int reportedW = -1, reportedH = -1;

void notifyResize(int width, int height) {
    if(width == reportedW && height == reportedH)
        return;
    reportedW = width;
    reportedH = height;
    // like Android's SurfaceView: the new surface size is known (UE4:
    // nativeSetSurfaceViewInfo) before the native window resize event
    ue4::onSurfaceSize(width, height);
    CB(onNativeWindowResized, (ANativeWindow*)gameWindow.get());
    CB(onContentRectChanged, nullptr);
    CB(onConfigurationChanged);
}

void registerCallbacks(GameWindow& w) {
    // Sizes changing before the activity is running (e.g. the desktop
    // maximizing the new window) are delivered once it starts.
    w.setWindowSizeCallback([](int width, int height) {
        if(lifecycleStarted)
            notifyResize(width, height);
    });
    w.setCloseCallback([]() { host::requestExit(); });

    w.setMouseButtonCallback([](double x, double y, int button, MouseButtonAction action) {
        bool down = action == MouseButtonAction::PRESS;
        if(button == 1) {
            // Left mouse button = finger on the touch screen
            leftDown = down;
            input::sendTouch(down ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP, 0, (float)x, (float)y);
            return;
        }
        int32_t bit = button == 2 ? AMOTION_EVENT_BUTTON_SECONDARY : button == 3 ? AMOTION_EVENT_BUTTON_TERTIARY
                    : button == 8 ? AMOTION_EVENT_BUTTON_BACK : button == 9 ? AMOTION_EVENT_BUTTON_FORWARD : 0;
        if(down) mouseButtons |= bit; else mouseButtons &= ~bit;
        input::sendMouse(down ? AMOTION_EVENT_ACTION_BUTTON_PRESS : AMOTION_EVENT_ACTION_BUTTON_RELEASE, (float)x, (float)y, mouseButtons);
        if(button == 8 && down) {
            // Mouse "back" side button = Android back key
            input::sendKey(AKEYCODE_BACK, AKEY_EVENT_ACTION_DOWN, 0);
            input::sendKey(AKEYCODE_BACK, AKEY_EVENT_ACTION_UP, 0);
        }
    });
    w.setMousePositionCallback([](double x, double y) {
        if(leftDown)
            input::sendTouch(AMOTION_EVENT_ACTION_MOVE, 0, (float)x, (float)y);
        else
            input::sendMouse(AMOTION_EVENT_ACTION_HOVER_MOVE, (float)x, (float)y, mouseButtons);
    });
    w.setMouseScrollCallback([](double x, double y, double dx, double dy) {
        input::sendMouse(AMOTION_EVENT_ACTION_SCROLL, (float)x, (float)y, mouseButtons, (float)dy, (float)dx);
    });
    w.setTouchStartCallback([](int id, double x, double y) { input::touchStart(id, (float)x, (float)y); });
    w.setTouchUpdateCallback([](int id, double x, double y) { input::touchMove(id, (float)x, (float)y); });
    w.setTouchEndCallback([](int id, double x, double y) { input::touchEnd(id, (float)x, (float)y); });

    w.setKeyboardCallback([](KeyCode key, KeyAction action, int mods) {
        if(key == KeyCode::FN11 && action == KeyAction::PRESS) {
            gameWindow->setFullscreen(!gameWindow->getFullscreen());
            return;
        }
        int32_t code = keymap::toAndroid(key);
        if(code == AKEYCODE_UNKNOWN)
            return;
        int32_t a = action == KeyAction::RELEASE ? AKEY_EVENT_ACTION_UP : AKEY_EVENT_ACTION_DOWN;
        input::sendKey(code, a, keymap::toAndroidMeta(mods), AINPUT_SOURCE_KEYBOARD, 1, action == KeyAction::REPEAT ? 1 : 0);
    });

    w.setGamepadStateCallback([](int id, bool connected) {
        const char* name = SDL_GetGamepadNameForID((SDL_JoystickID)id);
        host::gamepadConnected(id, connected, name ? name : "Gamepad", SDL_GetGamepadVendorForID((SDL_JoystickID)id),
                               SDL_GetGamepadProductForID((SDL_JoystickID)id));
    });
    w.setGamepadButtonCallback([](int id, GamepadButtonId btn, bool pressed) { host::gamepadButton(id, btn, pressed); });
    w.setGamepadAxisCallback([](int id, GamepadAxisId axis, float value) { host::gamepadAxis(id, axis, value); });
}

}  // namespace

void host::createWindow() {
    if(gameWindow)
        return;
    // Deliver real touch events, don't synthesize mouse <-> touch
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_APP_NAME, opts.title.c_str());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, opts.title.c_str());
    auto manager = GameWindowManager::getManager();
    // Android surfaces are opaque and games often leave garbage in the alpha
    // channel: without this the desktop compositor blends the window with
    // whatever is behind it (see-through, washed out picture).
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    gameWindow = manager->createWindow(opts.title, opts.width, opts.height, GraphicsApi::OPENGL_ES2);
    windowThread = std::this_thread::get_id();
    registerCallbacks(*gameWindow);
    if(opts.fullscreen)
        gameWindow->setFullscreen(true);
    gameWindow->show();
#ifdef USE_ARMHF_SUPPORT
    ArmhfSupport::install(glOverrides);
#endif
}

void host::gamepadConnected(int id, bool connected, const std::string& name, int vendor, int product) {
    {
        std::lock_guard<std::mutex> l(gamepadMutex);
        if(connected)
            gamepads[id] = {name, vendor, product};
        else
            gamepads.erase(id);
    }
    Log::info("Gamepad", "%s: %s (id %d, vendor %04x product %04x)", connected ? "connected" : "disconnected",
              name.c_str(), id, vendor, product);
}

void host::gamepadButton(int id, GamepadButtonId btn, bool pressed) {
    static const bool debug = getenv("APKL_INPUT_DEBUG") != nullptr;
    if(debug)
        Log::info("Gamepad", "id %d button %d %s", id, (int)btn, pressed ? "down" : "up");
    int32_t code = keymap::gamepadButton(btn);
    if(code != AKEYCODE_UNKNOWN)
        input::sendKey(code, pressed ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP, 0, AINPUT_SOURCE_GAMEPAD, 100 + id);
}

void host::gamepadAxis(int id, GamepadAxisId axis, float value) {
    int a = keymap::gamepadAxis(axis);
    if(a < 0)
        return;
    std::array<float, 48> axes;
    {
        std::lock_guard<std::mutex> l(gamepadMutex);
        auto& st = gamepadAxes[id];
        st[a] = value;
        axes = st;
    }
    input::sendJoystick(100 + id, axes.data());
}

bool host::gamepadInfo(int id, GamepadInfo& out) {
    std::lock_guard<std::mutex> l(gamepadMutex);
    auto it = gamepads.find(id);
    if(it == gamepads.end())
        return false;
    out = it->second;
    return true;
}

int host::gamepadCount() {
    std::lock_guard<std::mutex> l(gamepadMutex);
    return (int)gamepads.size();
}

GameWindow* host::window() {
    return gameWindow.get();
}

bool host::isWindowThread() {
    return gameWindow && std::this_thread::get_id() == windowThread;
}

void host::pumpEvents() {
    if(isWindowThread())
        gameWindow->pollEvents();
}

void host::getContentSize(int& w, int& h) {
    if(gameWindow) {
        gameWindow->getWindowSize(w, h);
    } else {
        w = opts.width;
        h = opts.height;
    }
}

void* host::glProc(const char* name) {
    auto it = glOverrides.find(name);
    if(it != glOverrides.end())
        return it->second;
    return (void*)GameWindowManager::getManager()->getProcAddrFunc()(name);
}

bool host::exitRequested() {
    return exiting;
}

void host::requestExit() {
    bool expected = false;
    if(!exiting.compare_exchange_strong(expected, true))
        return;
    Log::info("Launcher", "Exit requested, shutting the activity down");
    // Watchdog: some apps never return from android_main
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Log::warn("Launcher", "Activity did not stop in time, terminating");
        _Exit(0);
    }).detach();
    std::thread([]() {
        CB(onWindowFocusChanged, 0);
        CB(onPause);
        CB(onNativeWindowDestroyed, (ANativeWindow*)gameWindow.get());
        CB(onInputQueueDestroyed, (AInputQueue*)&FakeInputQueue::instance());
        CB(onStop);
        CB(onDestroy);
        Log::info("Launcher", "Activity destroyed");
        _Exit(0);
    }).detach();
}

void host::setSoftKeyboard(bool visible) {
    if(!gameWindow)
        return;
    if(visible)
        gameWindow->startTextInput();
    else
        gameWindow->stopTextInput();
}

void host::dispatchLifecycleStart() {
    CB(onStart);
    CB(onResume);
    CB(onNativeWindowCreated, (ANativeWindow*)gameWindow.get());
    CB(onInputQueueCreated, (AInputQueue*)&FakeInputQueue::instance());
    lifecycleStarted = true;
    CB(onWindowFocusChanged, 1);
    // catch up with resizes that happened while starting
    int w, h;
    getContentSize(w, h);
    if(w != opts.width || h != opts.height)
        notifyResize(w, h);
    else {
        reportedW = w;
        reportedH = h;
    }
}

int64_t host::uptimeNanos() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
