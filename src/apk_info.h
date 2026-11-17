#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Information extracted from the binary AndroidManifest.xml of an APK.
struct ApkManifest {
    bool parsed = false;
    std::string package;
    std::string versionName;
    int versionCode = 0;
    int minSdk = 0;
    int targetSdk = 0;
    bool hasCode = true;
    std::string label;          // raw label (may be a resource reference)
    std::string mainActivity;   // class name of the launcher activity
    std::string libName;        // <meta-data android:name="android.app.lib_name">
    std::string funcName;       // <meta-data android:name="android.app.func_name">
    bool usesNativeActivity = false;
    bool usesGameActivity = false;

    // <application><meta-data android:name=... android:value=...>
    struct MetaValue {
        uint8_t type = 0;   // Res_value data type (0x03 string, 0x04 float, 0x10/0x11 int, 0x12 bool)
        uint32_t data = 0;
        std::string str;
    };
    std::map<std::string, MetaValue> appMeta;

    static ApkManifest parse(const std::string& binaryXml);
};
