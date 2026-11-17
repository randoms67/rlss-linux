// apk-launcher: runs the native code of an Android APK directly on Linux,
// the same way mcpelauncher runs Minecraft: the APK's .so files are loaded by
// a port of the Android (bionic) linker, Android system libraries are
// replaced by fake implementations backed by SDL3/OpenGL ES, and the Java
// side is emulated with a fake JVM (libjnivm).

#include "apk_info.h"
#include "elf_scan.h"
#include "host.h"
#include "jni_classes.h"
#include "looper_input.h"
#include "stubs.h"
#include "thread_mover.h"
#include "ue4_support.h"
#include "stackdump.h"
#include "control.h"
#include "uri_handler.h"
#include "gamepad_bridge.h"
#include "zip_reader.h"

#include <mcpelauncher/linker.h>
#include <mcpelauncher/minecraft_utils.h>
#include <mcpelauncher/crash_handler.h>
#include <libc_shim.h>
#include <FileUtil.h>
#include <log.h>

#include <dirent.h>
#include <sched.h>
#include <signal.h>
#include <dlfcn.h>
#include <elf.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

#if defined(__x86_64__)
static const std::vector<std::string> kHostAbis = {"x86_64"};
static const int kHostMachine = EM_X86_64;
#elif defined(__i386__)
static const std::vector<std::string> kHostAbis = {"x86"};
static const int kHostMachine = EM_386;
#elif defined(__aarch64__)
static const std::vector<std::string> kHostAbis = {"arm64-v8a"};
static const int kHostMachine = EM_AARCH64;
#elif defined(__arm__)
static const std::vector<std::string> kHostAbis = {"armeabi-v7a", "armeabi"};
static const int kHostMachine = EM_ARM;
#else
#error "Unsupported architecture"
#endif

#if defined(__aarch64__)
extern "C" thread_local uintptr_t apkl_bionic_tls_slots[6];
extern "C" void apkl_sigsetjmp();
extern "C" void apkl_siglongjmp();
#endif

// bionic libc functions that libc-shim does not provide (used by Unreal Engine)
namespace bionic_extra {
ssize_t pread64_chk(int fd, void* buf, size_t count, off64_t offset, size_t buflen) {
    if(count > buflen)
        abort();  // what bionic's fortify does on overflow
    return pread64(fd, buf, count, offset);
}
ssize_t pwrite64_chk(int fd, const void* buf, size_t count, off64_t offset, size_t buflen) {
    if(count > buflen)
        abort();
    return pwrite64(fd, buf, count, offset);
}
int sched_cpucount(size_t setsize, const cpu_set_t* set) {
    return CPU_COUNT_S(setsize, set);
}
int isnanf_(float f) {
    return __builtin_isnan(f);
}
int sigrtmin() {
    return SIGRTMIN;
}
}  // namespace bionic_extra

namespace {

struct Args {
    std::string command = "run";
    std::string apk;
    std::string abi;
    std::string dataRoot;
    int width = 1280, height = 720;
    bool fullscreen = true;  // start fullscreen by default (F11 toggles)
    bool reextract = false;
    bool force = false;
    int dpi = 0;
    float uiScale = 0;
};

void usage() {
    std::cout <<
        "apk-launcher " APK_LAUNCHER_VERSION " - run native Android apps (NativeActivity) on Linux\n\n"
        "Usage:\n"
        "  apk-launcher [run] <app.apk> [options]   launch the app\n"
        "  apk-launcher info <app.apk>              show what the APK contains / if it can run\n"
        "  apk-launcher abis <app.apk>              print the native ABIs inside the APK\n"
        "  apk-launcher extract <app.apk>           only extract the APK\n\n"
        "Options:\n"
        "  -w, --width <px>       window width (default 1280)\n"
        "  -h, --height <px>      window height (default 720)\n"
        "  -f, --fullscreen       start in fullscreen (default, F11 toggles)\n"
        "  --windowed             start in a window instead of fullscreen\n"
        "  --abi <abi>            force an ABI directory (e.g. x86_64)\n"
        "  --data-dir <dir>       where apps are extracted and store data\n"
        "                         (default $XDG_DATA_HOME/apk-launcher)\n"
        "  --reextract            extract the APK again even if cached\n"
        "  --force                try to start apps whose main activity is Java\n"
        "  --dpi <n>              screen density reported to the app (default 160;\n"
        "                         higher = smaller menus and text)\n"
        "  --ui-scale <f>         Unreal Engine games: UI size (default 0.75, 1 = original)\n"
        "  --version              print the version\n\n"
        "Environment: MCPELAUNCHER_LINKER_VERBOSITY=<n> for Android linker debug output.\n";
}

bool parseArgs(int argc, char** argv, Args& a) {
    std::vector<std::string> pos;
    for(int i = 1; i < argc; i++) {
        std::string s = argv[i];
        auto next = [&](std::string& out) {
            if(i + 1 >= argc)
                throw std::runtime_error("missing value for " + s);
            out = argv[++i];
        };
        std::string v;
        if(s == "--help") { usage(); exit(0); }
        else if(s == "--version") { std::cout << APK_LAUNCHER_VERSION << "\n"; exit(0); }
        else if(s == "-w" || s == "--width") { next(v); a.width = std::stoi(v); }
        else if(s == "-h" || s == "--height") { next(v); a.height = std::stoi(v); }
        else if(s == "-f" || s == "--fullscreen") a.fullscreen = true;
        else if(s == "--windowed") a.fullscreen = false;
        else if(s == "--abi") next(a.abi);
        else if(s == "--data-dir") next(a.dataRoot);
        else if(s == "--reextract") a.reextract = true;
        else if(s == "--force") a.force = true;
        else if(s == "--dpi") { next(v); a.dpi = std::stoi(v); }
        else if(s == "--ui-scale") { next(v); a.uiScale = std::stof(v); }
        else if(!s.empty() && s[0] == '-') throw std::runtime_error("unknown option " + s);
        else pos.push_back(s);
    }
    if(!pos.empty() && (pos[0] == "run" || pos[0] == "info" || pos[0] == "abis" || pos[0] == "extract")) {
        a.command = pos[0];
        pos.erase(pos.begin());
    }
    if(pos.size() != 1)
        return false;
    a.apk = pos[0];
    return true;
}

std::string absPath(const std::string& p) {
    char buf[PATH_MAX];
    return realpath(p.c_str(), buf) ? std::string(buf) : p;
}

std::string defaultDataRoot() {
    const char* x = getenv("XDG_DATA_HOME");
    std::string base = x && *x ? x : std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.local/share";
    return base + "/apk-launcher";
}

std::string sanitize(std::string s) {
    for(auto& c : s)
        if(!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
            c = '_';
    return s.empty() ? "app" : s;
}

std::set<std::string> listAbis(const ZipReader& zip) {
    std::set<std::string> abis;
    for(auto& e : zip.entries()) {
        if(e.name.rfind("lib/", 0) != 0 || e.name.size() < 4 || e.name.compare(e.name.size() - 3, 3, ".so") != 0)
            continue;
        auto slash = e.name.find('/', 4);
        if(slash != std::string::npos)
            abis.insert(e.name.substr(4, slash - 4));
    }
    return abis;
}

std::string pickAbi(const std::set<std::string>& abis, const std::string& forced) {
    if(!forced.empty())
        return abis.count(forced) ? forced : "";
    for(auto& a : kHostAbis)
        if(abis.count(a))
            return a;
    return "";
}

std::string selfDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(n <= 0)
        return ".";
    buf[n] = 0;
    std::string p = buf;
    return p.substr(0, p.rfind('/'));
}

// Finds helper libraries shipped with the launcher (armhf bionic libc, ...)
std::string findSupportLib(const std::string& abi, const std::string& name) {
    std::vector<std::string> dirs;
    if(const char* appdir = getenv("APPDIR"))
        dirs.push_back(std::string(appdir) + "/usr/share/apk-launcher/lib/");
    dirs.push_back(selfDir() + "/../share/apk-launcher/lib/");
    dirs.push_back(std::string(MCPELAUNCHER_BIN_DIR_DEV) + "/lib/");
    for(auto& d : dirs)
        if(FileUtil::exists(d + abi + "/" + name))
            return d + abi + "/" + name;
    return "";
}

// Extracts what the launcher needs (native libs of one ABI, assets, manifest)
bool extractApk(ZipReader& zip, const std::string& apk, const std::string& abi, const std::string& dest, bool force) {
    struct stat st{};
    stat(apk.c_str(), &st);
    std::ostringstream stampContent;
    // not the path: an APK inside an AppImage is mounted somewhere new every run
    stampContent << st.st_size << "\n" << st.st_mtime << "\n" << abi << "\n";
    std::string stampPath = dest + "/.apk-launcher-stamp";
    std::string old;
    if(!force && FileUtil::readFile(stampPath, old) && old == stampContent.str())
        return true;
    Log::info("Launcher", "Extracting %s to %s", apk.c_str(), dest.c_str());
    FileUtil::mkdirRecursive(dest);
    std::string libPrefix = "lib/" + abi + "/";
    size_t count = 0;
    for(auto& e : zip.entries()) {
        bool want = e.name.rfind(libPrefix, 0) == 0 || e.name.rfind("assets/", 0) == 0 ||
                    e.name == "AndroidManifest.xml" || e.name.rfind("res/raw/", 0) == 0;
        if(!want || e.name.back() == '/' || e.name.find("..") != std::string::npos)
            continue;
        if(!zip.extractTo(e, dest + "/" + e.name)) {
            Log::error("Launcher", "Failed to extract %s", e.name.c_str());
            return false;
        }
        count++;
    }
    FileUtil::mkdirRecursive(dest + "/assets");
    std::ofstream(stampPath) << stampContent.str();
    Log::info("Launcher", "Extracted %zu files", count);
    return true;
}

// Builds an Android style CA directory (/system/etc/security/cacerts: one
// PEM per regular file) from the host's certificate bundle, so TLS in apps
// (HTTPS, websockets) trusts the same roots as the desktop.
std::string prepareCaStore(const std::string& root) {
    std::string bundlePath;
    for(const char* b : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                         "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"}) {
        if(FileUtil::exists(b)) {
            bundlePath = b;
            break;
        }
    }
    if(bundlePath.empty()) {
        Log::warn("Launcher", "No system CA bundle found, secure connections may fail");
        return "";
    }
    std::string dir = root + "/cacerts";
    struct stat st{};
    stat(bundlePath.c_str(), &st);
    std::string stamp = bundlePath + " " + std::to_string(st.st_size) + " " + std::to_string(st.st_mtime);
    std::string old;
    if(FileUtil::readFile(dir + "/.stamp", old) && old == stamp)
        return dir;
    std::string bundle;
    if(!FileUtil::readFile(bundlePath, bundle))
        return "";
    FileUtil::mkdirRecursive(dir);
    if(DIR* d = opendir(dir.c_str())) {
        while(dirent* e = readdir(d))
            if(e->d_name[0] != '.')
                unlink((dir + "/" + e->d_name).c_str());
        closedir(d);
    }
    const std::string begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    size_t pos = 0;
    int n = 0;
    while((pos = bundle.find(begin, pos)) != std::string::npos) {
        size_t e = bundle.find(end, pos);
        if(e == std::string::npos)
            break;
        e += end.size();
        char name[32];
        snprintf(name, sizeof(name), "/ca%04d.0", n++);
        std::ofstream(dir + name) << bundle.substr(pos, e - pos) << "\n";
        pos = e;
    }
    std::ofstream(dir + "/.stamp") << stamp;
    Log::info("Launcher", "Prepared %d trusted CA certificates from %s", n, bundlePath.c_str());
    return dir;
}

std::vector<std::string> listSharedLibs(const std::string& dir) {
    std::vector<std::string> out;
    if(DIR* d = opendir(dir.c_str())) {
        while(dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if(n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0)
                out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

const char* machineName(int m) {
    switch(m) {
    case EM_X86_64: return "x86_64";
    case EM_386: return "x86";
    case EM_AARCH64: return "arm64";
    case EM_ARM: return "arm";
    default: return "unknown";
    }
}

int printInfo(ZipReader& zip, const ApkManifest& m, const std::set<std::string>& abis) {
    std::cout << "Package:        " << (m.package.empty() ? "?" : m.package) << "\n";
    std::cout << "Version:        " << m.versionName << " (" << m.versionCode << ")\n";
    std::cout << "SDK:            min " << m.minSdk << ", target " << m.targetSdk << "\n";
    std::cout << "Main activity:  " << (m.mainActivity.empty() ? "?" : m.mainActivity) << "\n";
    if(!m.libName.empty())
        std::cout << "Native lib:     lib" << m.libName << ".so\n";
    std::cout << "Native ABIs:    ";
    for(auto& a : abis)
        std::cout << a << " ";
    std::cout << (abis.empty() ? "(none)" : "") << "\n";
    bool hasDex = zip.find("classes.dex") != nullptr;
    std::cout << "Java code:      " << (hasDex ? "yes (classes.dex)" : "no") << "\n";
    std::string verdict;
    if(abis.empty())
        verdict = "NO - the app has no native code (pure Java/Kotlin app)";
    else if(zip.find("assets/UE4CommandLine.txt") || m.mainActivity.rfind("com.epicgames.ue4.", 0) == 0)
        verdict = "YES - Unreal Engine 4 game (its Java side is emulated)";
    else if(m.usesGameActivity)
        verdict = "UNLIKELY - uses GameActivity (Java based), try --force";
    else if(m.usesNativeActivity)
        verdict = "YES - NativeActivity app";
    else
        verdict = "UNLIKELY - main activity is a Java class, the native libs are only helpers (try --force)";
    std::cout << "Can run:        " << verdict << "\n";
    std::cout << "This launcher:  " << kHostAbis[0] << (pickAbi(abis, "").empty() && !abis.empty() ? "  (ABI mismatch!)" : "") << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if(argc == 3 && std::string(argv[1]) == "--deliver-uri")
        return urihandler::deliver(argv[2]);
    if(argc == 3 && std::string(argv[1]) == "--gamepad-bridge")
        return gamepadbridge::serve(argv[2]);
    Args args;
    try {
        if(!parseArgs(argc, argv, args)) {
            usage();
            return 1;
        }
    } catch(std::exception& e) {
        std::cerr << "apk-launcher: " << e.what() << "\n";
        return 1;
    }
    args.apk = absPath(args.apk);

    std::unique_ptr<ZipReader> zip;
    try {
        zip = std::make_unique<ZipReader>(args.apk);
    } catch(std::exception& e) {
        std::cerr << "apk-launcher: " << e.what() << "\n";
        return 1;
    }
    auto abis = listAbis(*zip);
    if(args.command == "abis") {
        for(auto& a : abis)
            std::cout << a << "\n";
        return 0;
    }

    ApkManifest manifest;
    if(auto e = zip->find("AndroidManifest.xml")) {
        std::string xml;
        if(zip->read(*e, xml))
            manifest = ApkManifest::parse(xml);
    }
    if(args.command == "info")
        return printInfo(*zip, manifest, abis);

    std::string abi = pickAbi(abis, args.abi);
    if(abi.empty()) {
        if(abis.empty()) {
            std::cerr << "apk-launcher: this APK contains no native libraries. It is a Java/Kotlin app,\n"
                         "which needs a full Android runtime (e.g. Waydroid); apk-launcher only runs native apps.\n";
            return 2;
        }
        std::cerr << "apk-launcher: the APK only has native code for:";
        for(auto& a : abis)
            std::cerr << " " << a;
        std::cerr << "\nbut this launcher binary is " << kHostAbis[0] << ".\n";
        return 3;
    }

    // --- Paths -----------------------------------------------------------------
    std::string root = args.dataRoot.empty() ? defaultDataRoot() : args.dataRoot;
    std::string base = args.apk.substr(args.apk.rfind('/') + 1);
    std::string pkg = manifest.package.empty() ? sanitize(base.substr(0, base.rfind('.'))) : sanitize(manifest.package);
    auto& o = host::opts;
    o.apkPath = args.apk;
    o.packageName = manifest.package.empty() ? pkg : manifest.package;
    o.title = o.packageName;
    o.appDir = root + "/apps/" + pkg;
    o.nativeLibDir = o.appDir + "/lib/" + abi;
    o.assetsDir = o.appDir + "/assets";
    o.dataDir = root + "/data/" + pkg;
    o.filesDir = o.dataDir + "/files";
    o.cacheDir = o.dataDir + "/cache";
    o.externalDir = root + "/sdcard/Android/data/" + pkg + "/files";
    o.obbDir = root + "/sdcard/Android/obb/" + pkg;
    o.width = args.width;
    o.height = args.height;
    o.fullscreen = args.fullscreen;
    o.sdkVersion = 30;
    if(args.dpi > 0)
        o.densityDpi = args.dpi;
    else if(const char* d = getenv("APKL_DPI"))
        o.densityDpi = atoi(d) > 0 ? atoi(d) : o.densityDpi;
    for(auto d : {o.filesDir, o.cacheDir, o.externalDir, o.obbDir})
        FileUtil::mkdirRecursive(d);

    if(!extractApk(*zip, args.apk, abi, o.appDir, args.reextract))
        return 1;
    if(args.command == "extract") {
        std::cout << o.appDir << "\n";
        return 0;
    }
    zip.reset();

    CrashHandler::registerCrashHandler();
    stackdump::install();
    stackdump::installFaultReport();
    control::install();
#if defined(__aarch64__)
    {
        // Android's per-thread stack guard lives at TPIDR_EL0 + 0x28 (see tls_guard.cpp)
        uintptr_t tp;
        asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
        if((uintptr_t)&apkl_bionic_tls_slots[3] != tp + 0x28)
            Log::warn("Launcher", "TLS guard slot is misplaced (%p vs tp+0x28 %p): stack protector checks in arm64 apps may fail",
                      (void*)&apkl_bionic_tls_slots[3], (void*)(tp + 0x28));
    }
#endif
    Log::info("Launcher", "apk-launcher %s, package %s, ABI %s", APK_LAUNCHER_VERSION, o.packageName.c_str(), abi.c_str());

    // --- Inspect the native libraries ---------------------------------------
    auto libs = listSharedLibs(o.nativeLibDir);
    std::unordered_map<std::string, ElfInfo> elf;
    std::set<std::string> appDefined;
    for(auto& l : libs) {
        auto info = ElfInfo::read(o.nativeLibDir + "/" + l);
        if(!info.valid) {
            Log::warn("Launcher", "%s is not a valid ELF library", l.c_str());
            continue;
        }
        if(info.machine != kHostMachine) {
            std::cerr << "apk-launcher: " << l << " is built for " << machineName(info.machine)
                      << " but this launcher is " << machineName(kHostMachine) << "\n";
            return 3;
        }
        appDefined.insert(info.defined.begin(), info.defined.end());
        elf[l] = std::move(info);
    }

    std::string entry = manifest.funcName.empty() ? "ANativeActivity_onCreate" : manifest.funcName;
    std::string mainLib;
    if(!manifest.libName.empty() && elf.count("lib" + manifest.libName + ".so"))
        mainLib = "lib" + manifest.libName + ".so";
    else if(elf.count("libmain.so") && elf["libmain.so"].defined.count(entry))
        mainLib = "libmain.so";
    if(mainLib.empty())
        for(auto& kv : elf)
            if(kv.second.defined.count(entry)) {
                mainLib = kv.first;
                break;
            }
    if(mainLib.empty()) {
        std::cerr << "apk-launcher: no library in lib/" << abi << " exports " << entry << ".\n";
        if(manifest.usesGameActivity)
            std::cerr << "This app uses GameActivity (Java), which is not supported yet.\n";
        else
            std::cerr << "The app's main activity (" << (manifest.mainActivity.empty() ? "?" : manifest.mainActivity)
                      << ") is written in Java/Kotlin; its native libs can't run on their own.\n";
        return 4;
    }
    // Unreal Engine 4 games start from a Java activity, but the launcher
    // emulates its GameActivity (ue4_support.cpp)
    bool isUE4 = false;
    for(auto& kv : elf)
        if(kv.second.defined.count("Java_com_epicgames_ue4_GameActivity_nativeSetGlobalActivity"))
            isUE4 = true;
    if(manifest.parsed && !manifest.usesNativeActivity && !isUE4 && !args.force) {
        std::cerr << "apk-launcher: the main activity " << manifest.mainActivity
                  << " is a Java class; starting " << mainLib << " anyway may not work.\n"
                  << "Use --force to try.\n";
        return 4;
    }
    Log::info("Launcher", "Main library: %s, entry point: %s", mainLib.c_str(), entry.c_str());

    // --- Filesystem redirection (Android paths -> our data dir) --------------
    shim::rewrite_filesystem_access = {
        {"/data/data/" + o.packageName, o.dataDir},
        {"/data/user/0/" + o.packageName, o.dataDir},
        {"/data/app/" + o.packageName + "/lib/" + abi, o.nativeLibDir},
        {"/sdcard", root + "/sdcard"},
        {"/storage/emulated/0", root + "/sdcard"},
        {"/storage/self/primary", root + "/sdcard"},
    };
    // Android's trusted CA store: one PEM certificate per regular file
    {
        std::string ca = prepareCaStore(root);
        if(!ca.empty())
            shim::rewrite_filesystem_access.push_back({"/system/etc/security/cacerts", ca});
    }
    FileUtil::mkdirRecursive(root + "/sdcard");

    // --- Android linker + system libraries ----------------------------------
    linker::init();
    auto libcSyms = MinecraftUtils::getLibCSymbols();
    // By default the process main thread is a dedicated window/event loop (like
    // Android's UI thread) and the app's own threads run normally. Some apps
    // need their android_main thread on the main thread instead (window
    // systems that only allow GL on the main thread): APKL_MOVE_MAIN_THREAD=1
    const bool moveMainThread = getenv("APKL_MOVE_MAIN_THREAD") != nullptr;
    if(moveMainThread)
        ThreadMover::hookLibC(libcSyms);
    libcSyms["__pread64_chk"] = (void*)bionic_extra::pread64_chk;
    libcSyms["__pwrite64_chk"] = (void*)bionic_extra::pwrite64_chk;
    libcSyms["__sched_cpucount"] = (void*)bionic_extra::sched_cpucount;
    libcSyms["__isnanf"] = (void*)bionic_extra::isnanf_;
    libcSyms["__libc_current_sigrtmin"] = (void*)bionic_extra::sigrtmin;
    // sigsetjmp/siglongjmp: missing from the shim on 64-bit; see sigsetjmp_aarch64.S
#if defined(__aarch64__)
    libcSyms["sigsetjmp"] = (void*)apkl_sigsetjmp;
    libcSyms["siglongjmp"] = (void*)apkl_siglongjmp;
#elif defined(__x86_64__)
    // glibc's x86_64 sigjmp_buf (200 bytes) fits into bionic's (256 bytes)
    libcSyms["sigsetjmp"] = (void*)__sigsetjmp;
    libcSyms["siglongjmp"] = (void*)siglongjmp;
#endif
#ifdef USE_ARMHF_SUPPORT
    {
        linker::load_library("ld-android.so", {});
        android_dlextinfo extinfo{};
        std::vector<mcpelauncher_hook_t> hooks;
        for(auto& e : libcSyms)
            hooks.push_back({e.first.c_str(), e.second});
        hooks.push_back({nullptr, nullptr});
        extinfo.flags = ANDROID_DLEXT_MCPELAUNCHER_HOOKS;
        extinfo.mcpelauncher_hooks = hooks.data();
        auto libc = findSupportLib(abi, "libc.so"), libm = findSupportLib(abi, "libm.so");
        if(libc.empty() || !linker::dlopen_ext(libc.c_str(), 0, &extinfo) || libm.empty() || !linker::dlopen(libm.c_str(), 0)) {
            Log::error("Launcher", "Failed to load armhf compat libc/libm: %s", linker::dlerror());
            return 1;
        }
    }
#else
    linker::load_library("libc.so", libcSyms);
    MinecraftUtils::loadLibM();
#endif
    MinecraftUtils::setupHybris();  // libz, liblog, libOpenSLES, libGLESv1_CM, libstdc++
    linker::update_LD_LIBRARY_PATH(o.nativeLibDir.c_str());

    host::createWindow();
    gamepadbridge::connectIfConfigured();
    fakelibs::installEGL();
    if(!moveMainThread) {
        // the app's threads bind the window themselves (EGL); a window surface
        // can only be current in one thread, so the event thread lets go of it
        host::window()->makeCurrent(false);
    }

    std::unordered_map<std::string, void*> androidSyms, windowSyms, audioSyms;
    fakelibs::installLooperAndInput(androidSyms);
    fakelibs::installNativeWindow(androidSyms);
    fakelibs::installNativeWindow(windowSyms);
    fakelibs::installAssets(androidSyms, o.assetsDir);
    fakelibs::installMisc(androidSyms);
    fakelibs::installAudio(audioSyms);
    linker::load_library("libandroid.so", androidSyms);
    linker::load_library("libnativewindow.so", windowSyms);
    linker::load_library("libaaudio.so", audioSyms);

    std::vector<std::string> systemLibs = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libEGL.so",
                                           "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so", "libandroid.so",
                                           "libnativewindow.so", "libaaudio.so", "libOpenSLES.so", "libstdc++.so"};
    // Other system libraries the app links against get an empty stand-in
    for(auto& kv : elf) {
        for(auto& n : kv.second.needed) {
            if(elf.count(n) || std::find(systemLibs.begin(), systemLibs.end(), n) != systemLibs.end())
                continue;
            Log::warn("Launcher", "%s needs %s, providing an empty stand-in", kv.first.c_str(), n.c_str());
            linker::load_library(n.c_str(), {});
            systemLibs.push_back(n);
        }
    }

    // --- Resolve every symbol the app imports from the system ----------------
    std::vector<void*> systemHandles;
    for(auto& n : systemLibs)
        if(void* h = linker::dlopen(n.c_str(), 0))
            systemHandles.push_back(h);
    std::unordered_map<std::string, void*> fallback;
    size_t stubbed = 0;
    for(auto& kv : elf) {
        for(auto& sym : kv.second.undefinedStrong) {
            if(appDefined.count(sym) || fallback.count(sym))
                continue;
            bool found = false;
            for(void* h : systemHandles)
                if(linker::dlsym(h, sym.c_str())) {
                    found = true;
                    break;
                }
            if(found)
                continue;
            void* p = nullptr;
            if(sym.rfind("gl", 0) == 0)
                p = host::glProc(sym.c_str());
            bool hostOk = sym.rfind("_Z", 0) != 0 || sym.rfind("_Znw", 0) == 0 || sym.rfind("_Zna", 0) == 0 ||
                          sym.rfind("_Zdl", 0) == 0 || sym.rfind("_Zda", 0) == 0;
            if(!p && hostOk && sym.rfind("__", 0) != 0)
                p = dlsym(RTLD_DEFAULT, sym.c_str());
            if(p) {
                Log::debug("Launcher", "%s: using host %s", kv.first.c_str(), sym.c_str());
            } else {
                Log::debug("Launcher", "%s: stubbing missing %s", kv.first.c_str(), sym.c_str());
                p = stubs::make(sym);
                stubbed++;
            }
            fallback[sym] = p;
        }
    }
    if(!fallback.empty()) {
        Log::info("Launcher", "%zu imported symbols not implemented by the launcher (%zu stubbed)", fallback.size(), stubbed);
        linker::relocate(linker::dlopen("libc.so", 0), fallback);
    }

    // --- Fake JVM ------------------------------------------------------------
    static Baron::Jvm vm;
    jni::registerClasses(vm);

    Log::info("Launcher", "Loading %s", mainLib.c_str());
    void* handle = linker::dlopen((o.nativeLibDir + "/" + mainLib).c_str(), RTLD_NOW);
    if(!handle) {
        std::cerr << "apk-launcher: failed to load " << mainLib << ": " << linker::dlerror() << "\n";
        return 5;
    }
    for(auto& kv : elf)
        vm.attachLibrary(kv.first, "", {linker::dlopen, linker::dlsym, linker::dlclose_unlocked});

    ue4::setManifest(manifest);
    ue4::detect(handle);
    {
        float uiScale = args.uiScale;
        if(uiScale <= 0 && getenv("APKL_UI_SCALE"))
            uiScale = (float)atof(getenv("APKL_UI_SCALE"));
        ue4::prepareCommandLine(o.assetsDir, uiScale);
    }

    auto onCreate = (ANativeActivity_createFunc*)linker::dlsym(handle, entry.c_str());
    if(!onCreate) {
        std::cerr << "apk-launcher: " << mainLib << " does not export " << entry << "\n";
        return 4;
    }

    urihandler::startListener();

    // --- Start the activity on an "Android UI thread" ------------------------
    std::thread uiThread([&]() {
        FakeJni::LocalFrame frame(vm);
        auto activityObj = jni::createActivity();
        auto& a = host::activity;
        a.callbacks = &host::callbacks;
        a.vm = (JavaVM*)&vm;
        a.env = (JNIEnv*)&frame.getJniEnv();
        a.clazz = vm.createGlobalReference(activityObj);
        a.internalDataPath = o.filesDir.c_str();
        a.externalDataPath = o.externalDir.c_str();
        a.obbPath = o.obbDir.c_str();
        a.sdkVersion = o.sdkVersion;
        a.assetManager = (AAssetManager*)fakelibs::assetManager();
        a.instance = nullptr;

        looper::prepareCurrentThread();
        if(auto onLoad = (jint(*)(JavaVM*, void*))linker::dlsym(handle, "JNI_OnLoad")) {
            Log::info("Launcher", "Calling JNI_OnLoad");
            onLoad(a.vm, nullptr);
        }
        Log::info("Launcher", "Calling %s", entry.c_str());
        ThreadMover::markStartThread();
        onCreate(&a, nullptr, 0);
        ThreadMover::onCreateFinished();
        ue4::afterCreate(a.env, a.clazz, handle);
        host::dispatchLifecycleStart();
        looper::loopUntilExit();
    });
    uiThread.detach();

    if(moveMainThread && ThreadMover::runMovedThread()) {
        // android_main returned
        Log::info("Launcher", "App main thread finished");
        if(!host::exitRequested())
            host::requestExit();
        while(true)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    // Window event loop: input, resize, close and compositor pings are handled
    // here, input goes to the app's AInputQueue.
    Log::info("Launcher", "Running window event loop");
    while(!host::exitRequested()) {
        host::pumpEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    while(true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
