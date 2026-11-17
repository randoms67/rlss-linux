#include "apk_info.h"

#include <axml/axml_file.h>
#include <axml/axml_parser.h>
#include <log.h>

namespace {

struct ActivityInfo {
    std::string name;
    std::string libName, funcName;
    bool launcher = false;
    bool main = false;
};

std::string qualify(const std::string& pkg, std::string cls) {
    if(!cls.empty() && cls[0] == '.')
        return pkg + cls;
    if(cls.find('.') == std::string::npos && !pkg.empty())
        return pkg + "." + cls;
    return cls;
}

}  // namespace

ApkManifest ApkManifest::parse(const std::string& xml) {
    ApkManifest m;
    try {
        std::string copy = xml;
        axml::AXMLFile file((void*)copy.data(), copy.size());
        axml::AXMLParser parser(file);
        std::vector<std::string> path;
        std::vector<ActivityInfo> activities;
        ActivityInfo* cur = nullptr;

        auto attr = [&](const char* name, std::string& out) -> bool {
            for(size_t i = 0; i < parser.getElementAttributeCount(); i++) {
                if(parser.getElementAttributeName(i) == name) {
                    try {
                        out = parser.getElementAttributeRawValue(i);
                    } catch(...) {
                        out.clear();
                    }
                    if(out.empty()) {
                        auto& tv = parser.getElementAttributeTypedValue(i);
                        if(tv.dataType == 0x01)  // reference
                            out = "@0x" + [&] { char b[16]; snprintf(b, sizeof(b), "%08x", (unsigned)tv.data); return std::string(b); }();
                        else
                            out = std::to_string((int)tv.data);
                    }
                    return true;
                }
            }
            return false;
        };
        auto attrInt = [&](const char* name, int def) -> int {
            for(size_t i = 0; i < parser.getElementAttributeCount(); i++)
                if(parser.getElementAttributeName(i) == name)
                    return (int)parser.getElementAttributeTypedValue(i).data;
            return def;
        };

        while(parser.next()) {
            if(parser.eventType() == axml::AXMLParser::START_ELEMENT) {
                std::string el = parser.getElementName();
                path.push_back(el);
                if(el == "manifest") {
                    attr("package", m.package);
                    attr("versionName", m.versionName);
                    m.versionCode = attrInt("versionCode", 0);
                } else if(el == "uses-sdk") {
                    m.minSdk = attrInt("minSdkVersion", 0);
                    m.targetSdk = attrInt("targetSdkVersion", 0);
                } else if(el == "application") {
                    attr("label", m.label);
                    m.hasCode = attrInt("hasCode", -1) != 0;
                } else if(el == "activity" || el == "activity-alias") {
                    activities.emplace_back();
                    cur = &activities.back();
                    std::string target;
                    attr("name", cur->name);
                    if(el == "activity-alias" && attr("targetActivity", target))
                        cur->name = target;
                } else if(cur && el == "action") {
                    std::string n;
                    if(attr("name", n) && n == "android.intent.action.MAIN")
                        cur->main = true;
                } else if(cur && el == "category") {
                    std::string n;
                    if(attr("name", n) && (n == "android.intent.category.LAUNCHER" || n == "android.intent.category.LEANBACK_LAUNCHER"))
                        cur->launcher = true;
                } else if(!cur && el == "meta-data" && path.size() == 3 && path[1] == "application") {
                    std::string n;
                    attr("name", n);
                    for(size_t i = 0; i < parser.getElementAttributeCount(); i++) {
                        if(parser.getElementAttributeName(i) != "value")
                            continue;
                        MetaValue v;
                        auto& tv = parser.getElementAttributeTypedValue(i);
                        v.type = (uint8_t)tv.dataType;
                        v.data = (uint32_t)tv.data;
                        try {
                            v.str = parser.getElementAttributeRawValue(i);
                        } catch(...) {
                        }
                        m.appMeta[n] = v;
                    }
                } else if(cur && el == "meta-data") {
                    std::string n, v;
                    attr("name", n);
                    attr("value", v);
                    if(n == "android.app.lib_name")
                        cur->libName = v;
                    else if(n == "android.app.func_name")
                        cur->funcName = v;
                }
            } else if(parser.eventType() == axml::AXMLParser::END_ELEMENT) {
                if(!path.empty()) {
                    if(path.back() == "activity" || path.back() == "activity-alias")
                        cur = nullptr;
                    path.pop_back();
                }
            }
        }
        const ActivityInfo* best = nullptr;
        for(auto& a : activities)
            if(a.main && a.launcher) { best = &a; break; }
        if(!best)
            for(auto& a : activities)
                if(!a.libName.empty()) { best = &a; break; }
        if(!best && !activities.empty())
            best = &activities.front();
        if(best) {
            m.mainActivity = qualify(m.package, best->name);
            m.libName = best->libName;
            m.funcName = best->funcName;
        }
        m.usesNativeActivity = m.mainActivity == "android.app.NativeActivity" || !m.libName.empty();
        m.usesGameActivity = m.mainActivity.find("GameActivity") != std::string::npos;
        m.parsed = true;
    } catch(std::exception& e) {
        Log::warn("ApkManifest", "Failed to parse AndroidManifest.xml: %s", e.what());
    }
    return m;
}
