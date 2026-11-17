#pragma once

#include <baron/baron.h>
#include <jni.h>
#include <memory>

namespace jni {
// Registers the fake Android framework classes (Context, NativeActivity, ...)
void registerClasses(Baron::Jvm& vm);
// Creates the NativeActivity java object handed to the app (activity->clazz)
std::shared_ptr<FakeJni::JObject> createActivity();
}
