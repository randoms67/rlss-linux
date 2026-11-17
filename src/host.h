#pragma once

// Global state of the emulated Android "device": the single window, the
// NativeActivity and its callbacks, launch options and helpers shared by all
// the fake NDK libraries (libandroid, libEGL, libGLESv2, libaaudio, ...).

#include <android/native_activity.h>
#include <game_window.h>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef __arm__
// Android armeabi-v7a passes floats in core registers (softfp), the armhf
// host in VFP registers. Every function exported to Android code that takes or
// returns a float must use the soft-float calling convention.
#define ANDROID_PCS __attribute__((pcs("aapcs")))
#else
#define ANDROID_PCS
#endif

struct LaunchOptions {
    std::string apkPath;
    std::string packageName;
    std::string title;
    std::string appDir;        // extracted APK root
    std::string nativeLibDir;  // extracted lib/<abi>
    std::string assetsDir;     // extracted assets/
    std::string dataDir;       // per app private data (/data/data/<pkg>)
    std::string filesDir, cacheDir, externalDir, obbDir;
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    int sdkVersion = 30;
    int densityDpi = 160;
};

namespace host {

extern LaunchOptions opts;
extern ANativeActivity activity;
extern ANativeActivityCallbacks callbacks;

// Window ------------------------------------------------------------------
void createWindow();            // creates + shows the SDL window (calling thread owns it)
GameWindow* window();           // nullptr until created
bool isWindowThread();
void pumpEvents();              // only does something on the window thread
void getContentSize(int& w, int& h);
void* glProc(const char* name); // host GL/GLES symbol lookup

// Lifecycle ---------------------------------------------------------------
void requestExit();             // runs onPause/onStop/onDestroy and quits
bool exitRequested();
void setSoftKeyboard(bool visible);

// Gamepads (from SDL in-process, or from the native gamepad bridge when the
// app runs under ARM translation). Android device id = 100 + id.
struct GamepadInfo {
    std::string name;
    int vendor = 0, product = 0;
};
void gamepadConnected(int id, bool connected, const std::string& name, int vendor, int product);
void gamepadButton(int id, GamepadButtonId btn, bool pressed);
void gamepadAxis(int id, GamepadAxisId axis, float value);
bool gamepadInfo(int id, GamepadInfo& out);
int gamepadCount();

// ANativeActivity callback helpers (safe to call when unset)
void dispatchLifecycleStart();  // onStart/onResume/window+input created/focus

// Time in nanoseconds (CLOCK_MONOTONIC), Android style
int64_t uptimeNanos();

}  // namespace host

// Installs all fake libraries into the Android linker.
namespace fakelibs {
void installLooperAndInput(std::unordered_map<std::string, void*>& libandroid);
void installNativeWindow(std::unordered_map<std::string, void*>& libandroid);
void installAssets(std::unordered_map<std::string, void*>& libandroid, const std::string& assetsDir);
void installMisc(std::unordered_map<std::string, void*>& libandroid);
void installEGL();
void installAudio(std::unordered_map<std::string, void*>& libaaudio);
void* assetManager();                           // the AAssetManager* handed to the app
void inputQueueCreatedForWindow();              // internal
}
