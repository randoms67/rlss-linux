#pragma once

#include <string>
#include <unordered_map>

// Moves the first thread the app spawns from ANativeActivity_onCreate (the
// android_native_app_glue "android_main" thread) onto the process main thread.
// Window systems (and SDL) want all window/GL work on the main thread.
namespace ThreadMover {
void hookLibC(std::unordered_map<std::string, void*>& libcSymbols);
void markStartThread();
void onCreateFinished();  // call after ANativeActivity_onCreate returned
// Returns false if the app did not create a thread during onCreate.
bool runMovedThread();
}
