#pragma once

#include <string>

// Native controller reading for apps running under ARM translation (qemu)
namespace gamepadbridge {
int serve(const std::string& socketPath);  // `apk-launcher --gamepad-bridge <socket>` (native side)
void connectIfConfigured();                 // APKL_GAMEPAD_BRIDGE=<socket> (emulated side)
}
