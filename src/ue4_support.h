#pragma once

// Support for Unreal Engine 4 Android games (com.epicgames.ue4.GameActivity).
// UE4 apps are NativeActivity apps, but their Java GameActivity feeds the
// engine a lot of startup information through native calls and the engine
// calls back into ~90 "AndroidThunkJava_*" Java methods. This module plays the
// part of that Java class.

#include "apk_info.h"
#include <jni.h>
#include <string>

namespace ue4 {

// true if the loaded main library is a UE4 game
bool detect(void* mainLibHandle);
bool active();

// manifest <meta-data> lookups used by the GetMetaData* thunks
void setManifest(const ApkManifest& manifest);
bool hasMeta(const std::string& key);
std::string metaString(const std::string& key);
int metaInt(const std::string& key);
bool metaBool(const std::string& key);
float metaFloat(const std::string& key);

// Adds engine command line arguments (assets/UE4CommandLine.txt), e.g. the
// UI scale. Must run before ANativeActivity_onCreate.
void prepareCommandLine(const std::string& assetsDir, float uiScale);

// Runs the startup sequence of GameActivity.onCreate after
// ANativeActivity_onCreate (on the UI thread).
void afterCreate(JNIEnv* env, jobject activity, void* mainLibHandle);

// Calls a no-argument GameActivity native (e.g. "nativeInitHMDs") on the calling thread
void callVoidNative(const char* name);

// Window size changed (GameActivity.surfaceChanged)
void onSurfaceSize(int width, int height);

}  // namespace ue4
