#include "ue4_support.h"
#include "host.h"

#include <mcpelauncher/linker.h>
#include <log.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <FileUtil.h>

static void applyUiScale(const std::string& project);

namespace {
bool isUE4 = false;
void* lib = nullptr;
ApkManifest manifest;
JNIEnv* uiEnv = nullptr;
jobject activityObj = nullptr;

template <class Fn>
Fn native(const char* name) {
    std::string full = std::string("Java_com_epicgames_ue4_GameActivity_") + name;
    auto p = (Fn)linker::dlsym(lib, full.c_str());
    if(!p)
        Log::warn("UE4", "native %s not found", full.c_str());
    return p;
}

const ApkManifest::MetaValue* find(const std::string& key) {
    auto it = manifest.appMeta.find(key);
    return it == manifest.appMeta.end() ? nullptr : &it->second;
}
}  // namespace

bool ue4::detect(void* handle) {
    lib = handle;
    isUE4 = linker::dlsym(handle, "Java_com_epicgames_ue4_GameActivity_nativeSetGlobalActivity") != nullptr;
    if(isUE4)
        Log::info("UE4", "Unreal Engine 4 game detected, emulating com.epicgames.ue4.GameActivity");
    return isUE4;
}

bool ue4::active() {
    return isUE4;
}

void ue4::setManifest(const ApkManifest& m) {
    manifest = m;
}

bool ue4::hasMeta(const std::string& key) {
    return find(key) != nullptr;
}

std::string ue4::metaString(const std::string& key) {
    auto v = find(key);
    return v ? v->str : std::string();
}

int ue4::metaInt(const std::string& key) {
    auto v = find(key);
    if(!v)
        return 0;
    if(v->type >= 0x10 && v->type <= 0x1f)
        return (int)v->data;
    return atoi(v->str.c_str());
}

bool ue4::metaBool(const std::string& key) {
    auto v = find(key);
    if(!v)
        return false;
    if(v->type == 0x12)
        return v->data != 0;
    return v->str == "true" || v->str == "1";
}

float ue4::metaFloat(const std::string& key) {
    auto v = find(key);
    if(!v)
        return 0.f;
    if(v->type == 0x04) {
        float f;
        memcpy(&f, &v->data, sizeof(f));
        return f;
    }
    return (float)atof(v->str.c_str());
}

void ue4::afterCreate(JNIEnv* env, jobject activity, void* handle) {
    if(!isUE4)
        return;
    lib = handle;
    uiEnv = env;
    activityObj = activity;
    auto& o = host::opts;
    const std::string pfx = "com.epicgames.ue4.GameActivity.";
    auto str = [&](const std::string& s) { return env->NewStringUTF(s.c_str()); };

    // Values GameActivity.onCreate reads from the manifest meta-data
    bool useExternalFilesDir = metaBool(pfx + "bUseExternalFilesDir");
    bool publicLogFiles = metaBool(pfx + "bPublicLogFiles");
    bool obbInApk = metaBool(pfx + "bPackageDataInsideApk");
    int depthPreference = metaInt(pfx + "DepthBufferPreference");
    std::string project = metaString(pfx + "ProjectName");
    if(project.empty())
        project = o.packageName.substr(o.packageName.rfind('.') + 1);
    std::string appType = metaString(pfx + "AppType");
    Log::info("UE4", "project %s, data inside APK: %s", project.c_str(), obbInApk ? "yes" : "no");
    applyUiScale(project);

    if(auto f = native<void (*)(JNIEnv*, jobject, jboolean, jboolean, jstring, jstring, jboolean, jstring)>("nativeSetGlobalActivity"))
        f(env, activity, useExternalFilesDir, publicLogFiles, str(o.filesDir), str(o.externalDir), obbInApk, str(o.apkPath));
    if(auto f = native<void (*)(JNIEnv*, jobject, jboolean, jint)>("nativeSetWindowInfo"))
        f(env, activity, false /* landscape */, depthPreference);
    if(auto f = native<void (*)(JNIEnv*, jobject, jboolean)>("nativeSetAndroidStartupState"))
        f(env, activity, false);
    if(auto f = native<void (*)(JNIEnv*, jobject, jstring, jint, jstring, jstring, jstring, jstring)>("nativeSetAndroidVersionInformation"))
        f(env, activity, str("11"), o.sdkVersion, str("Linux"), str("APK Launcher"), str("apk-launcher"), str("en_US"));
    if(auto f = native<void (*)(JNIEnv*, jobject, jstring, jstring, jint, jint, jstring)>("nativeSetObbInfo"))
        f(env, activity, str(project), str(o.packageName), manifest.versionCode, 0, str(appType));
    if(auto f = native<void (*)(JNIEnv*, jobject, jobjectArray)>("nativeSetConfigRulesVariables"))
        f(env, activity, env->NewObjectArray(0, env->FindClass("java/lang/String"), nullptr));
    int w, h;
    host::getContentSize(w, h);
    onSurfaceSize(w, h);
    if(auto f = native<void (*)(JNIEnv*, jobject, jboolean, jfloat, jfloat, jfloat, jfloat)>("nativeSetSafezoneInfo"))
        f(env, activity, false, 0.f, 0.f, 0.f, 0.f);
    Log::info("UE4", "Resuming main init");
    if(auto f = native<void (*)(JNIEnv*, jobject)>("nativeResumeMainInit"))
        f(env, activity);
}

void ue4::onSurfaceSize(int width, int height) {
    if(!isUE4 || !activityObj)
        return;
    // may be called from the window thread: use that thread's JNIEnv
    JNIEnv* env = nullptr;
    JavaVM* vm = host::activity.vm;
    if(!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)
        return;
    if(auto f = native<void (*)(JNIEnv*, jobject, jint, jint)>("nativeSetSurfaceViewInfo"))
        f(env, activityObj, width, height);
}

void ue4::callVoidNative(const char* name) {
    if(!isUE4 || !activityObj)
        return;
    JNIEnv* env = nullptr;
    JavaVM* vm = host::activity.vm;
    if(!vm || vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)
        return;
    if(auto f = native<void (*)(JNIEnv*, jobject)>(name))
        f(env, activityObj);
}

static float gUiScale = 0.75f;  // desktop default: phone UIs are huge on a monitor

void ue4::prepareCommandLine(const std::string& assetsDir, float uiScale) {
    if(uiScale > 0)
        gUiScale = uiScale;
    // restore the APK's own command line if an older launcher version changed it
    std::string file = assetsDir + "/UE4CommandLine.txt", orig = file + ".orig", base;
    if(FileUtil::readFile(orig, base))
        std::ofstream(file, std::ios::binary | std::ios::trunc) << base;
}

// The engine reads <files>/UE4Game/<Project>/<Project>/Saved/Config/Android/Engine.ini
// on top of the packaged config: set the UMG/Slate application scale there.
static void applyUiScale(const std::string& project) {
    std::string dir = host::opts.filesDir + "/UE4Game/" + project + "/" + project + "/Saved/Config/Android";
    std::string path = dir + "/Engine.ini", text, out;
    FileUtil::readFile(path, text);
    const std::string section = "[/Script/Engine.UserInterfaceSettings]";
    bool skip = false;
    std::istringstream in(text);
    for(std::string line; std::getline(in, line);) {
        if(!line.empty() && line[0] == '[')
            skip = line.rfind(section, 0) == 0;
        if(!skip)
            out += line + "\n";
    }
    while(out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    if(gUiScale != 1.0f) {
        char buf[160];
        snprintf(buf, sizeof(buf), "\n%s\nApplicationScale=%.2f\n", section.c_str(), gUiScale);
        out += buf;
    }
    FileUtil::mkdirRecursive(dir);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << out;
    Log::info("UE4", "UI scale %.2f (use --ui-scale to change)", gUiScale);
}
