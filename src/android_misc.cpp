// AConfiguration, ASensorManager, ANativeActivity_* helpers and other small
// libandroid.so APIs.

#include "host.h"

#include <log.h>
#include <cstring>
#include <cstdlib>
#include <string>

namespace {

struct FakeConfig {
    char lang[2] = {'e', 'n'};
    char country[2] = {'U', 'S'};
    int32_t density = 160, orientation = 2, touchscreen = 3, keyboard = 2, navigation = 1;
    int32_t keysHidden = 1, navHidden = 1, screenSize = 3, screenLong = 1, uiModeType = 1, uiModeNight = 1;
    int32_t sdk = 30, widthDp = 0, heightDp = 0, smallestWidthDp = 0, layoutDirection = 0, screenRound = 1;
    int32_t mcc = 0, mnc = 0;
};

void fillConfig(FakeConfig* c) {
    *c = FakeConfig{};
    const char* lang = getenv("LANG");
    if(lang && strlen(lang) >= 2 && lang[0] != 'C') {
        c->lang[0] = lang[0];
        c->lang[1] = lang[1];
        if(strlen(lang) >= 5 && lang[2] == '_') {
            c->country[0] = lang[3];
            c->country[1] = lang[4];
        }
    }
    int w, h;
    host::getContentSize(w, h);
    c->density = host::opts.densityDpi;
    c->sdk = host::opts.sdkVersion;
    c->orientation = w >= h ? 2 : 1;
    float scale = c->density / 160.f;
    c->widthDp = (int32_t)(w / scale);
    c->heightDp = (int32_t)(h / scale);
    c->smallestWidthDp = c->widthDp < c->heightDp ? c->widthDp : c->heightDp;
}

struct FakeSensorManager {};
struct FakeSensorQueue {};
FakeSensorManager sensorManager;

}  // namespace

void fakelibs::installMisc(std::unordered_map<std::string, void*>& s) {
#define C(x) ((FakeConfig*)(x))
    s["AConfiguration_new"] = (void*)+[]() -> void* { auto c = new FakeConfig; fillConfig(c); return c; };
    s["AConfiguration_delete"] = (void*)+[](void* c) { delete C(c); };
    s["AConfiguration_fromAssetManager"] = (void*)+[](void* c, void*) { fillConfig(C(c)); };
    s["AConfiguration_copy"] = (void*)+[](void* d, void* src) { *C(d) = *C(src); };
    s["AConfiguration_getLanguage"] = (void*)+[](void* c, char* out) { memcpy(out, C(c)->lang, 2); };
    s["AConfiguration_getCountry"] = (void*)+[](void* c, char* out) { memcpy(out, C(c)->country, 2); };
    s["AConfiguration_setLanguage"] = (void*)+[](void* c, const char* in) { if(in) memcpy(C(c)->lang, in, 2); };
    s["AConfiguration_setCountry"] = (void*)+[](void* c, const char* in) { if(in) memcpy(C(c)->country, in, 2); };
#define G(name, field) s["AConfiguration_get" name] = (void*)+[](void* c) -> int32_t { return C(c)->field; }
    G("Density", density); G("Orientation", orientation); G("Touchscreen", touchscreen); G("Keyboard", keyboard);
    G("Navigation", navigation); G("KeysHidden", keysHidden); G("NavHidden", navHidden); G("ScreenSize", screenSize);
    G("ScreenLong", screenLong); G("ScreenRound", screenRound); G("UiModeType", uiModeType); G("UiModeNight", uiModeNight);
    G("SdkVersion", sdk); G("ScreenWidthDp", widthDp); G("ScreenHeightDp", heightDp);
    G("SmallestScreenWidthDp", smallestWidthDp); G("LayoutDirection", layoutDirection); G("Mcc", mcc); G("Mnc", mnc);
#undef G
    s["AConfiguration_diff"] = (void*)+[](void*, void*) -> int32_t { return 0; };
    s["AConfiguration_match"] = (void*)+[](void*, void*) -> int32_t { return 1; };
    s["AConfiguration_isBetterThan"] = (void*)+[](void*, void*, void*) -> int32_t { return 0; };
#undef C

    // Sensors: there are none, but the manager and queues are valid objects
    s["ASensorManager_getInstance"] = (void*)+[]() -> void* { return &sensorManager; };
    s["ASensorManager_getInstanceForPackage"] = (void*)+[](const char*) -> void* { return &sensorManager; };
    s["ASensorManager_getDefaultSensor"] = (void*)+[](void*, int) -> void* { return nullptr; };
    s["ASensorManager_getDefaultSensorEx"] = (void*)+[](void*, int, bool) -> void* { return nullptr; };
    s["ASensorManager_getSensorList"] = (void*)+[](void*, void** list) -> int { if(list) *list = nullptr; return 0; };
    s["ASensorManager_createEventQueue"] = (void*)+[](void*, void*, int, void*, void*) -> void* { return new FakeSensorQueue; };
    s["ASensorManager_destroyEventQueue"] = (void*)+[](void*, void* q) -> int { delete (FakeSensorQueue*)q; return 0; };
    s["ASensorEventQueue_enableSensor"] = (void*)+[](void*, void*) -> int { return -1; };
    s["ASensorEventQueue_disableSensor"] = (void*)+[](void*, void*) -> int { return 0; };
    s["ASensorEventQueue_setEventRate"] = (void*)+[](void*, void*, int32_t) -> int { return 0; };
    s["ASensorEventQueue_registerSensor"] = (void*)+[](void*, void*, int32_t, int64_t) -> int { return -1; };
    s["ASensorEventQueue_hasEvents"] = (void*)+[](void*) -> int { return 0; };
    s["ASensorEventQueue_getEvents"] = (void*)+[](void*, void*, size_t) -> ssize_t { return 0; };

    // NativeActivity helpers
    s["ANativeActivity_finish"] = (void*)+[](ANativeActivity*) { host::requestExit(); };
    s["ANativeActivity_setWindowFormat"] = (void*)+[](ANativeActivity*, int32_t) {};
    s["ANativeActivity_setWindowFlags"] = (void*)+[](ANativeActivity*, uint32_t add, uint32_t) {
        if(add & 0x400 /* AWINDOW_FLAG_FULLSCREEN */) {
            // Respect the user's choice; only log
            Log::trace("Activity", "App requested fullscreen window");
        }
    };
    s["ANativeActivity_showSoftInput"] = (void*)+[](ANativeActivity*, uint32_t) { host::setSoftKeyboard(true); };
    s["ANativeActivity_hideSoftInput"] = (void*)+[](ANativeActivity*, uint32_t) { host::setSoftKeyboard(false); };

    // Tracing / performance hints: no-ops
    s["ATrace_isEnabled"] = (void*)+[]() -> bool { return false; };
    s["ATrace_beginSection"] = (void*)+[](const char*) {};
    s["ATrace_endSection"] = (void*)+[]() {};
    s["ATrace_beginAsyncSection"] = (void*)+[](const char*, int32_t) {};
    s["ATrace_endAsyncSection"] = (void*)+[](const char*, int32_t) {};
    s["ATrace_setCounter"] = (void*)+[](const char*, int64_t) {};
    s["APerformanceHint_getManager"] = (void*)+[]() -> void* { return nullptr; };
    s["AThermal_acquireManager"] = (void*)+[]() -> void* { return nullptr; };
}
