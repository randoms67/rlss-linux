#pragma once

#include <cstdint>
#include <game_window.h>

namespace keymap {
int32_t toAndroid(KeyCode key);
int32_t toAndroidMeta(int mods);
int32_t gamepadButton(GamepadButtonId b);
int32_t gamepadAxis(GamepadAxisId a);  // AMOTION_EVENT_AXIS_* or -1
}
