// Login redirects (e.g. Epic: eos.<client id>://epic/auth?code=...).
//
// On Android the browser hands such a link back to the app through an intent.
// Here the launcher registers itself as the desktop's handler for the link
// scheme; the browser then runs `apk-launcher --deliver-uri <link>`, which
// passes the link through a FIFO to the running app instance.

#include "uri_handler.h"
#include "host.h"

#include <mcpelauncher/linker.h>
#include <FileUtil.h>
#include <log.h>
#include <dirent.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <strings.h>
#include <fstream>
#include <set>
#include <string>
#include <thread>

namespace {

std::string runtimeDir() {
    const char* r = getenv("XDG_RUNTIME_DIR");
    std::string d = (r && *r) ? r : "/tmp";
    return d + "/apk-launcher";
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for(size_t i = 0; i < s.size(); i++) {
        if(s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}

void run(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if(pid == 0) {
        unsetenv("LD_LIBRARY_PATH");
        unsetenv("LD_PRELOAD");
        std::vector<char*> a;
        for(auto& s : argv)
            a.push_back((char*)s.c_str());
        a.push_back(nullptr);
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 1);
        dup2(devnull, 2);
        execvp(a[0], a.data());
        _exit(127);
    }
    if(pid > 0)
        waitpid(pid, nullptr, 0);
}

// Command line the desktop should run for links
std::string handlerCommand() {
    std::string exe;
    if(const char* a = getenv("APPIMAGE"))
        exe = a;  // inside the AppImage
    else if(const char* h = getenv("APKL_URI_HANDLER"))
        exe = h;  // development / ARM translation runs
    else {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if(n > 0)
            exe.assign(buf, n);
    }
    // .desktop Exec quoting: escape " ` $ and backslash inside double quotes
    std::string q;
    for(char c : exe) {
        if(c == '"' || c == '`' || c == '$' || c == '\\')
            q += '\\';
        q += c;
    }
    return "\"" + q + "\" --deliver-uri %u";
}

std::set<std::string> registeredSchemes;

void registerScheme(const std::string& scheme) {
    auto& done = registeredSchemes;
    if(scheme.empty() || done.count(scheme))
        return;
    done.insert(scheme);
    for(char c : scheme)
        if(!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '+')
            return;
    // Browsers lowercase the scheme of a link before asking the desktop who
    // handles it, so the MIME type must be lowercase too, otherwise GNOME
    // finds no handler ("which app should open this?" with nothing to pick).
    // deliverToApp() restores the original spelling for the app.
    std::string lower = scheme;
    for(auto& c : lower)
        c = (char)tolower((unsigned char)c);
    const char* home = getenv("HOME");
    const char* xdgData = getenv("XDG_DATA_HOME");
    std::string apps = (xdgData && *xdgData) ? std::string(xdgData) + "/applications" : std::string(home ? home : "/tmp") + "/.local/share/applications";
    FileUtil::mkdirRecursive(apps);
    std::string name = "apk-launcher-" + lower + ".desktop";
    std::ofstream(apps + "/" + name) << "[Desktop Entry]\n"
                                        "Type=Application\n"
                                        "Name=APK Launcher (" << host::opts.title << " login)\n"
                                        "Exec=" << handlerCommand() << "\n"
                                        "NoDisplay=true\n"
                                        "Terminal=false\n"
                                        "MimeType=x-scheme-handler/" << lower << ";\n";
    run({"update-desktop-database", apps});
    run({"xdg-mime", "default", name, "x-scheme-handler/" + lower});
    run({"gio", "mime", "x-scheme-handler/" + lower, name});
    Log::info("Login", "Registered %s:// links to come back to this app", scheme.c_str());
}

void deliverToApp(std::string uri) {
    // Browsers lowercase the scheme; the app may compare it exactly, so put
    // back the spelling it asked for (eos.<Client Id>://)
    auto c = uri.find("://");
    if(c != std::string::npos) {
        for(auto& s : registeredSchemes) {
            if(s.size() == c && strncasecmp(s.c_str(), uri.c_str(), c) == 0) {
                uri = s + uri.substr(c);
                break;
            }
        }
    }
    // Epic Online Services (com.epicgames.mobile.eossdk.EOSAuthHandlerActivity)
    void* eos = linker::dlopen("libEOSSDK.so", 0);
    using AuthFn = void (*)(JNIEnv*, jclass, jstring);
    AuthFn auth = nullptr;
    if(eos) {
        auth = (AuthFn)linker::dlsym(eos, "Java_com_epicgames_mobile_eossdk_EOSSDK_auth");
        if(!auth)  // exported under the overloaded JNI name
            auth = (AuthFn)linker::dlsym(eos, "Java_com_epicgames_mobile_eossdk_EOSSDK_auth__Ljava_lang_String_2");
    }
    if(!auth) {
        Log::warn("Login", "Received %s but the app has no handler for it", uri.c_str());
        return;
    }
    JNIEnv* env = nullptr;
    if(!host::activity.vm || host::activity.vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env)
        return;
    Log::info("Login", "Passing the login redirect back to Epic Online Services");
    auth(env, env->FindClass("com/epicgames/mobile/eossdk/EOSSDK"), env->NewStringUTF(uri.c_str()));
}

}  // namespace

void urihandler::onOpenUrl(const std::string& url) {
    // OAuth style flows tell us where the browser will send the user back to
    auto p = url.find("redirect_uri=");
    if(p == std::string::npos)
        return;
    std::string r = urlDecode(url.substr(p + 13, url.find('&', p) - (p + 13)));
    auto c = r.find("://");
    if(c == std::string::npos)
        return;
    std::string scheme = r.substr(0, c);
    if(scheme == "http" || scheme == "https")
        return;
    registerScheme(scheme);
}

void urihandler::startListener() {
    std::string dir = runtimeDir();
    FileUtil::mkdirRecursive(dir);
    chmod(dir.c_str(), 0700);
    std::string fifo = dir + "/" + host::opts.packageName + ".uri";
    unlink(fifo.c_str());
    if(mkfifo(fifo.c_str(), 0600) != 0) {
        Log::warn("Login", "cannot create %s", fifo.c_str());
        return;
    }
    std::thread([fifo]() {
        while(true) {
            std::ifstream in(fifo);
            std::string line;
            while(std::getline(in, line))
                if(!line.empty())
                    deliverToApp(line);
        }
    }).detach();
}

int urihandler::deliver(const std::string& uri) {
    // called as `apk-launcher --deliver-uri <link>` by the desktop
    int sent = 0;
    std::string dir = runtimeDir();
    if(DIR* d = opendir(dir.c_str())) {
        while(dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if(n.size() < 5 || n.compare(n.size() - 4, 4, ".uri") != 0)
                continue;
            int fd = open((dir + "/" + n).c_str(), O_WRONLY | O_NONBLOCK);
            if(fd < 0)
                continue;  // no app instance listening
            std::string line = uri + "\n";
            if(write(fd, line.data(), line.size()) == (ssize_t)line.size())
                sent++;
            close(fd);
        }
        closedir(d);
    }
    if(!sent)
        fprintf(stderr, "apk-launcher: no running app is waiting for %s\n", uri.c_str());
    return sent ? 0 : 1;
}
