// game-window key codes (Windows virtual key style) -> Android AKEYCODE_*.

#include "keymap.h"

#include <android/keycodes.h>
#include <android/input.h>

int32_t keymap::toAndroid(KeyCode key) {
    int k = (int)key;
    if(k >= (int)KeyCode::A && k <= (int)KeyCode::Z)
        return AKEYCODE_A + (k - (int)KeyCode::A);
    if(k >= (int)KeyCode::NUM_0 && k <= (int)KeyCode::NUM_9)
        return AKEYCODE_0 + (k - (int)KeyCode::NUM_0);
    if(k >= (int)KeyCode::NUMPAD_0 && k <= (int)KeyCode::NUMPAD_9)
        return AKEYCODE_NUMPAD_0 + (k - (int)KeyCode::NUMPAD_0);
    if(k >= (int)KeyCode::FN1 && k <= (int)KeyCode::FN12)
        return AKEYCODE_F1 + (k - (int)KeyCode::FN1);
    switch(key) {
    case KeyCode::BACK: return AKEYCODE_BACK;
    case KeyCode::ESCAPE: return AKEYCODE_BACK;  // Android "back" is the closest thing
    case KeyCode::BACKSPACE: return AKEYCODE_DEL;
    case KeyCode::DELETE: return AKEYCODE_FORWARD_DEL;
    case KeyCode::TAB: return AKEYCODE_TAB;
    case KeyCode::ENTER: return AKEYCODE_ENTER;
    case KeyCode::LEFT_SHIFT: return AKEYCODE_SHIFT_LEFT;
    case KeyCode::RIGHT_SHIFT: return AKEYCODE_SHIFT_RIGHT;
    case KeyCode::LEFT_CTRL: return AKEYCODE_CTRL_LEFT;
    case KeyCode::RIGHT_CTRL: return AKEYCODE_CTRL_RIGHT;
    case KeyCode::LEFT_ALT: return AKEYCODE_ALT_LEFT;
    case KeyCode::RIGHT_ALT: return AKEYCODE_ALT_RIGHT;
    case KeyCode::LEFT_SUPER: return AKEYCODE_META_LEFT;
    case KeyCode::RIGHT_SUPER: return AKEYCODE_META_RIGHT;
    case KeyCode::PAUSE: return AKEYCODE_BREAK;
    case KeyCode::CAPS_LOCK: return AKEYCODE_CAPS_LOCK;
    case KeyCode::SPACE: return AKEYCODE_SPACE;
    case KeyCode::PAGE_UP: return AKEYCODE_PAGE_UP;
    case KeyCode::PAGE_DOWN: return AKEYCODE_PAGE_DOWN;
    case KeyCode::END: return AKEYCODE_MOVE_END;
    case KeyCode::HOME: return AKEYCODE_MOVE_HOME;
    case KeyCode::LEFT: return AKEYCODE_DPAD_LEFT;
    case KeyCode::UP: return AKEYCODE_DPAD_UP;
    case KeyCode::RIGHT: return AKEYCODE_DPAD_RIGHT;
    case KeyCode::DOWN: return AKEYCODE_DPAD_DOWN;
    case KeyCode::INSERT: return AKEYCODE_INSERT;
    case KeyCode::NUMPAD_MULTIPLY: return AKEYCODE_NUMPAD_MULTIPLY;
    case KeyCode::NUMPAD_ADD: return AKEYCODE_NUMPAD_ADD;
    case KeyCode::NUMPAD_SEPERATOR: return AKEYCODE_NUMPAD_COMMA;
    case KeyCode::NUMPAD_SUBTRACT: return AKEYCODE_NUMPAD_SUBTRACT;
    case KeyCode::NUMPAD_DECIMAL: return AKEYCODE_NUMPAD_DOT;
    case KeyCode::NUMPAD_DIVIDE: return AKEYCODE_NUMPAD_DIVIDE;
    case KeyCode::NUM_LOCK: return AKEYCODE_NUM_LOCK;
    case KeyCode::SCROLL_LOCK: return AKEYCODE_SCROLL_LOCK;
    case KeyCode::SEMICOLON: return AKEYCODE_SEMICOLON;
    case KeyCode::EQUAL: return AKEYCODE_EQUALS;
    case KeyCode::COMMA: return AKEYCODE_COMMA;
    case KeyCode::MINUS: return AKEYCODE_MINUS;
    case KeyCode::PERIOD: return AKEYCODE_PERIOD;
    case KeyCode::SLASH: return AKEYCODE_SLASH;
    case KeyCode::GRAVE: return AKEYCODE_GRAVE;
    case KeyCode::LEFT_BRACKET: return AKEYCODE_LEFT_BRACKET;
    case KeyCode::BACKSLASH: return AKEYCODE_BACKSLASH;
    case KeyCode::RIGHT_BRACKET: return AKEYCODE_RIGHT_BRACKET;
    case KeyCode::APOSTROPHE: return AKEYCODE_APOSTROPHE;
    case KeyCode::MENU: return AKEYCODE_MENU;
    default: return AKEYCODE_UNKNOWN;
    }
}

int32_t keymap::toAndroidMeta(int mods) {
    int32_t m = 0;
    if(mods & KEY_MOD_SHIFT) m |= AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON;
    if(mods & KEY_MOD_CTRL) m |= AMETA_CTRL_ON | AMETA_CTRL_LEFT_ON;
    if(mods & KEY_MOD_ALT) m |= AMETA_ALT_ON | AMETA_ALT_LEFT_ON;
    if(mods & KEY_MOD_SUPER) m |= AMETA_META_ON | AMETA_META_LEFT_ON;
    if(mods & KEY_MOD_CAPSLOCK) m |= AMETA_CAPS_LOCK_ON;
    if(mods & KEY_MOD_NUMLOCK) m |= AMETA_NUM_LOCK_ON;
    return m;
}

int32_t keymap::gamepadButton(GamepadButtonId b) {
    switch(b) {
    case GamepadButtonId::A: return AKEYCODE_BUTTON_A;
    case GamepadButtonId::B: return AKEYCODE_BUTTON_B;
    case GamepadButtonId::X: return AKEYCODE_BUTTON_X;
    case GamepadButtonId::Y: return AKEYCODE_BUTTON_Y;
    case GamepadButtonId::LB: return AKEYCODE_BUTTON_L1;
    case GamepadButtonId::RB: return AKEYCODE_BUTTON_R1;
    case GamepadButtonId::BACK: return AKEYCODE_BUTTON_SELECT;
    case GamepadButtonId::START: return AKEYCODE_BUTTON_START;
    case GamepadButtonId::GUIDE: return AKEYCODE_BUTTON_MODE;
    case GamepadButtonId::LEFT_STICK: return AKEYCODE_BUTTON_THUMBL;
    case GamepadButtonId::RIGHT_STICK: return AKEYCODE_BUTTON_THUMBR;
    case GamepadButtonId::DPAD_UP: return AKEYCODE_DPAD_UP;
    case GamepadButtonId::DPAD_RIGHT: return AKEYCODE_DPAD_RIGHT;
    case GamepadButtonId::DPAD_DOWN: return AKEYCODE_DPAD_DOWN;
    case GamepadButtonId::DPAD_LEFT: return AKEYCODE_DPAD_LEFT;
    default: return AKEYCODE_UNKNOWN;
    }
}

int32_t keymap::gamepadAxis(GamepadAxisId a) {
    switch(a) {
    case GamepadAxisId::LEFT_X: return AMOTION_EVENT_AXIS_X;
    case GamepadAxisId::LEFT_Y: return AMOTION_EVENT_AXIS_Y;
    case GamepadAxisId::RIGHT_X: return AMOTION_EVENT_AXIS_Z;
    case GamepadAxisId::RIGHT_Y: return AMOTION_EVENT_AXIS_RZ;
    case GamepadAxisId::LEFT_TRIGGER: return AMOTION_EVENT_AXIS_LTRIGGER;
    case GamepadAxisId::RIGHT_TRIGGER: return AMOTION_EVENT_AXIS_RTRIGGER;
    default: return -1;
    }
}
