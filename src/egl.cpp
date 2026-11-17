// Fake libEGL.so: Android apps create their GLES context through EGL; we map
// that onto the single SDL window/context created by the launcher. Also
// provides libGLESv2.so / libGLESv3.so backed by the host GLES implementation.

#include "host.h"
#include "control.h"

#define __ANDROID__
#include <EGL/egl.h>
#undef __ANDROID__
#include <android/native_window.h>
#include <log.h>
#include <mcpelauncher/linker.h>
#include <minecraft/imported/glesv2_symbols.h>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_egl.h>

#ifndef EGL_OPENGL_ES3_BIT
#define EGL_OPENGL_ES3_BIT 0x00000040
#endif
#ifndef EGL_CONTEXT_CLIENT_VERSION
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#endif

namespace fegl {

constexpr uintptr_t kDisplay = 1, kConfig = 1, kPbuffer = 2;

// Every EGL context is a real SDL/OpenGL context sharing objects with the
// window's main context, so engines that keep several contexts current on
// different threads (Unreal: render thread + loading thread) work.
struct FakeContext {
    SDL_GLContext gl = nullptr;
};
std::mutex contextMutex;
SDL_GLContext rootContext = nullptr;  // the window's own context (share root)

bool eglDebug() {
    static bool d = getenv("APKL_EGL_DEBUG") != nullptr;
    return d;
}
unsigned long tid() { return (unsigned long)gettid(); }

// Direct access to the host EGL that SDL uses (SDL_GLContext is an EGLContext there)
struct HostEGL {
    EGLBoolean (*makeCurrent)(void*, void*, void*, void*) = nullptr;
    void* (*createPbuffer)(void*, void*, const EGLint*) = nullptr;
    void* display = nullptr;
    void* config = nullptr;
    bool loaded = false;
    void load() {
        if(loaded)
            return;
        loaded = true;
        makeCurrent = (decltype(makeCurrent))SDL_EGL_GetProcAddress("eglMakeCurrent");
        createPbuffer = (decltype(createPbuffer))SDL_EGL_GetProcAddress("eglCreatePbufferSurface");
        display = SDL_EGL_GetCurrentDisplay();
        config = SDL_EGL_GetCurrentConfig();
    }
};
HostEGL hostEgl;
thread_local void* threadPbuffer = nullptr;

// Bind ctx on this thread without the window surface (surfaceless, else 1x1 pbuffer)
bool makeCurrentOffscreen(SDL_GLContext gl) {
    if(!hostEgl.makeCurrent || !hostEgl.display)
        return false;
    // let SDL forget this thread's binding so a later window bind is not skipped
    SDL_GL_MakeCurrent(nullptr, nullptr);
    if(hostEgl.makeCurrent(hostEgl.display, nullptr, nullptr, gl))
        return true;
    if(!threadPbuffer && hostEgl.createPbuffer && hostEgl.config) {
        const EGLint attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        threadPbuffer = hostEgl.createPbuffer(hostEgl.display, hostEgl.config, attribs);
    }
    return threadPbuffer && hostEgl.makeCurrent(hostEgl.display, threadPbuffer, threadPbuffer, gl);
}

SDL_Window* sdlWindow() {
    int n = 0;
    SDL_Window** ws = SDL_GetWindows(&n);
    SDL_Window* w = n > 0 ? ws[0] : nullptr;
    SDL_free(ws);
    return w;
}
thread_local EGLSurface currentDraw = EGL_NO_SURFACE;
thread_local EGLContext currentContext = EGL_NO_CONTEXT;
thread_local EGLint lastError = EGL_SUCCESS;
EGLenum boundApi = EGL_OPENGL_ES_API;

bool isWindowSurface(EGLSurface s) {
    return s != EGL_NO_SURFACE && (uintptr_t)s != kPbuffer;
}

EGLBoolean configAttrib(EGLint attribute, EGLint* value) {
    switch(attribute) {
    case EGL_RED_SIZE: case EGL_GREEN_SIZE: case EGL_BLUE_SIZE: case EGL_ALPHA_SIZE: *value = 8; break;
    case EGL_BUFFER_SIZE: *value = 32; break;
    case EGL_DEPTH_SIZE: *value = 24; break;
    case EGL_STENCIL_SIZE: *value = 8; break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_NATIVE_VISUAL_ID: *value = WINDOW_FORMAT_RGBA_8888; break;
    case EGL_NATIVE_VISUAL_TYPE: *value = 0; break;
    case EGL_SAMPLES: case EGL_SAMPLE_BUFFERS: *value = 0; break;
    case EGL_RENDERABLE_TYPE: case EGL_CONFORMANT: *value = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT; break;
    case EGL_SURFACE_TYPE: *value = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; break;
    case EGL_COLOR_BUFFER_TYPE: *value = EGL_RGB_BUFFER; break;
    case EGL_CONFIG_CAVEAT: *value = EGL_NONE; break;
    case EGL_LEVEL: *value = 0; break;
    case EGL_MAX_PBUFFER_WIDTH: case EGL_MAX_PBUFFER_HEIGHT: *value = 8192; break;
    case EGL_MIN_SWAP_INTERVAL: *value = 0; break;
    case EGL_MAX_SWAP_INTERVAL: *value = 1; break;
    case EGL_NATIVE_RENDERABLE: *value = EGL_TRUE; break;
    case EGL_TRANSPARENT_TYPE: *value = EGL_NONE; break;
    default: *value = 0; break;
    }
    return EGL_TRUE;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType) { return (EGLDisplay)kDisplay; }
EGLDisplay eglGetPlatformDisplay(EGLenum, void*, const void*) { return (EGLDisplay)kDisplay; }
EGLBoolean eglInitialize(EGLDisplay, EGLint* major, EGLint* minor) {
    if(major) *major = 1;
    if(minor) *minor = 4;
    return EGL_TRUE;
}
EGLBoolean eglTerminate(EGLDisplay) { return EGL_TRUE; }
EGLint eglGetError() {
    EGLint e = lastError;
    lastError = EGL_SUCCESS;
    return e;
}
const char* eglQueryString(EGLDisplay, EGLint name) {
    switch(name) {
    case EGL_VENDOR: return "apk-launcher";
    case EGL_VERSION: return "1.4 apk-launcher";
    case EGL_CLIENT_APIS: return "OpenGL_ES";
    case EGL_EXTENSIONS: return "EGL_KHR_create_context EGL_KHR_surfaceless_context EGL_ANDROID_presentation_time";
    }
    return "";
}
EGLBoolean eglGetConfigs(EGLDisplay, EGLConfig* configs, EGLint size, EGLint* num) {
    if(configs && size > 0)
        configs[0] = (EGLConfig)kConfig;
    if(num)
        *num = 1;
    return EGL_TRUE;
}
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint*, EGLConfig* configs, EGLint size, EGLint* num) {
    return eglGetConfigs(d, configs, size, num);
}
EGLBoolean eglGetConfigAttrib(EGLDisplay, EGLConfig, EGLint attribute, EGLint* value) {
    return value ? configAttrib(attribute, value) : EGL_FALSE;
}
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, EGLNativeWindowType win, const EGLint*) {
    return win ? (EGLSurface)win : (EGLSurface)host::window();
}
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig c, void* win, const void*) {
    return fegl::eglCreateWindowSurface(d, c, (EGLNativeWindowType)win, nullptr);
}
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*) { return (EGLSurface)kPbuffer; }
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface) { return EGL_TRUE; }
EGLBoolean eglSurfaceAttrib(EGLDisplay, EGLSurface, EGLint, EGLint) { return EGL_TRUE; }
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext share, const EGLint*) {
    std::lock_guard<std::mutex> l(contextMutex);
    SDL_Window* win = sdlWindow();
    if(!win || !host::window()) {
        lastError = EGL_BAD_NATIVE_WINDOW;
        return EGL_NO_CONTEXT;
    }
    // SDL shares with the context current on this thread: temporarily make
    // the share root (or the requested share context) current.
    SDL_GLContext prev = SDL_GL_GetCurrentContext();
    if(!rootContext) {
        host::window()->makeCurrent(true);
        rootContext = SDL_GL_GetCurrentContext();
        hostEgl.load();  // display/config of the window's context
    }
    SDL_GLContext shareWith = share ? ((FakeContext*)share)->gl : rootContext;
    if(prev != shareWith && !SDL_GL_MakeCurrent(win, shareWith)) {
        Log::warn("EGL", "cannot make share context current on this thread: %s", SDL_GetError());
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(win, prev);
    if(!gl) {
        Log::error("EGL", "SDL_GL_CreateContext failed: %s", SDL_GetError());
        lastError = EGL_BAD_ALLOC;
        return EGL_NO_CONTEXT;
    }
    auto c = new FakeContext;
    c->gl = gl;
    if(eglDebug())
        Log::info("EGL", "[%lu] eglCreateContext share=%p -> %p (gl %p)", tid(), share, c, gl);
    return (EGLContext)c;
}
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext ctx) {
    if(ctx) {
        SDL_GL_DestroyContext(((FakeContext*)ctx)->gl);
        delete (FakeContext*)ctx;
    }
    return EGL_TRUE;
}
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface draw, EGLSurface, EGLContext ctx) {
    SDL_Window* win = sdlWindow();
    if(!win) {
        lastError = EGL_BAD_NATIVE_WINDOW;
        return EGL_FALSE;
    }
    SDL_GLContext gl = ctx ? ((FakeContext*)ctx)->gl : nullptr;
    // A window surface may only be current in one thread at a time, so
    // pbuffer (offscreen) contexts are made current without a surface; apps
    // only render into FBOs with them.
    bool ok;
    if(gl && !isWindowSurface(draw)) {
        ok = makeCurrentOffscreen(gl);
    } else {
        ok = SDL_GL_MakeCurrent(win, gl);
    }
    if(eglDebug())
        Log::info("EGL", "[%lu] eglMakeCurrent draw=%p ctx=%p -> %s (%s)", tid(), draw, ctx, ok ? "ok" : "FAILED", ok ? "" : SDL_GetError());
    if(!ok) {
        Log::warn("EGL", "SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        lastError = EGL_BAD_ACCESS;
        return EGL_FALSE;
    }
    currentDraw = draw;
    currentContext = ctx;
    return EGL_TRUE;
}
EGLContext eglGetCurrentContext() { return currentContext; }
EGLSurface eglGetCurrentSurface(EGLint) { return currentDraw; }
EGLDisplay eglGetCurrentDisplay() { return currentContext ? (EGLDisplay)kDisplay : EGL_NO_DISPLAY; }
EGLBoolean eglQueryContext(EGLDisplay, EGLContext, EGLint attribute, EGLint* value) {
    if(!value)
        return EGL_FALSE;
    if(attribute == EGL_CONTEXT_CLIENT_VERSION) *value = 3;
    else if(attribute == EGL_CONFIG_ID) *value = 1;
    else if(attribute == EGL_CONTEXT_CLIENT_TYPE) *value = EGL_OPENGL_ES_API;
    else if(attribute == EGL_RENDER_BUFFER) *value = EGL_BACK_BUFFER;
    else *value = 0;
    return EGL_TRUE;
}
EGLBoolean eglQuerySurface(EGLDisplay, EGLSurface surface, EGLint attribute, EGLint* value) {
    if(!value)
        return EGL_FALSE;
    int w = 1, h = 1;
    if(isWindowSurface(surface))
        host::getContentSize(w, h);
    switch(attribute) {
    case EGL_WIDTH: *value = w; break;
    case EGL_HEIGHT: *value = h; break;
    case EGL_CONFIG_ID: *value = 1; break;
    case EGL_RENDER_BUFFER: *value = EGL_BACK_BUFFER; break;
    case EGL_SWAP_BEHAVIOR: *value = EGL_BUFFER_DESTROYED; break;
    case EGL_HORIZONTAL_RESOLUTION: case EGL_VERTICAL_RESOLUTION: *value = host::opts.densityDpi * EGL_DISPLAY_SCALING / 254 * 100; break;
    default: *value = 0;
    }
    return EGL_TRUE;
}
void countFrame() {
    // APKL_FPS=1: every 5 seconds log the frame rate and the worst frame time
    static const bool enabled = getenv("APKL_FPS") != nullptr;
    if(!enabled)
        return;
    static int frames = 0, slow = 0;
    static int64_t start = host::uptimeNanos(), last = start, worst = 0;
    int64_t now = host::uptimeNanos();
    int64_t dt = now - last;
    last = now;
    worst = std::max(worst, dt);
    if(dt > 50000000)
        slow++;  // frames over 50 ms are visible hitches
    frames++;
    if(now - start >= 5000000000LL) {
        Log::info("FPS", "%.1f fps, worst frame %.0f ms, %d frames over 50 ms", frames * 1e9 / (double)(now - start), worst / 1e6, slow);
        frames = slow = 0;
        worst = 0;
        start = now;
    }
}
void maybeScreenshot() {
    std::string file = control::takeScreenshotRequest();
    if(file.empty())
        return;
    auto getInt = (void (*)(unsigned, int*))host::glProc("glGetIntegerv");
    auto bindFb = (void (*)(unsigned, unsigned))host::glProc("glBindFramebuffer");
    auto read = (void (*)(int, int, int, int, unsigned, unsigned, void*))host::glProc("glReadPixels");
    auto pixelStore = (void (*)(unsigned, int))host::glProc("glPixelStorei");
    int w, h, prevRead = 0;
    host::getContentSize(w, h);
    getInt(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &prevRead);
    bindFb(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    pixelStore(0x0D05 /* GL_PACK_ALIGNMENT */, 1);
    std::vector<unsigned char> px((size_t)w * h * 4);
    read(0, 0, w, h, 0x1908 /* RGBA */, 0x1401 /* UNSIGNED_BYTE */, px.data());
    bindFb(0x8CA8, prevRead);
    // keep the real alpha values: a non-opaque window shows the desktop through
    bool ok = control::writePng(file, w, h, px.data());
    Log::info("Control", "screenshot %s: %s", file.c_str(), ok ? "saved" : "failed");
}

// If the window framebuffer still has an alpha channel, make it fully opaque
// before presenting (the app's GL state is saved and restored).
void forceOpaqueAlpha() {
    struct Fns {
        void (*getInt)(unsigned, int*);
        void (*getBool)(unsigned, unsigned char*);
        void (*getFloat)(unsigned, float*);
        unsigned char (*isEnabled)(unsigned);
        void (*enable)(unsigned);
        void (*disable)(unsigned);
        void (*bindFb)(unsigned, unsigned);
        void (*colorMask)(unsigned char, unsigned char, unsigned char, unsigned char);
        void (*clearColor)(float, float, float, float);
        void (*clear)(unsigned);
    };
    static thread_local int alphaBits = -1;
    static Fns f;
    static bool loaded = false;
    if(!loaded) {
        f.getInt = (decltype(f.getInt))host::glProc("glGetIntegerv");
        f.getBool = (decltype(f.getBool))host::glProc("glGetBooleanv");
        f.getFloat = (decltype(f.getFloat))host::glProc("glGetFloatv");
        f.isEnabled = (decltype(f.isEnabled))host::glProc("glIsEnabled");
        f.enable = (decltype(f.enable))host::glProc("glEnable");
        f.disable = (decltype(f.disable))host::glProc("glDisable");
        f.bindFb = (decltype(f.bindFb))host::glProc("glBindFramebuffer");
        f.colorMask = (decltype(f.colorMask))host::glProc("glColorMask");
        f.clearColor = (decltype(f.clearColor))host::glProc("glClearColor");
        f.clear = (decltype(f.clear))host::glProc("glClear");
        loaded = true;
    }
    int prevDraw = 0;
    f.getInt(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &prevDraw);
    f.bindFb(0x8CA9 /* GL_DRAW_FRAMEBUFFER */, 0);
    if(alphaBits < 0) {
        alphaBits = 0;
        f.getInt(0x0D55 /* GL_ALPHA_BITS */, &alphaBits);
        Log::info("EGL", "window framebuffer alpha bits: %d%s", alphaBits, alphaBits ? " (forcing opaque)" : "");
    }
    if(alphaBits > 0) {
        unsigned char mask[4];
        float color[4];
        f.getBool(0x0C23 /* GL_COLOR_WRITEMASK */, mask);
        f.getFloat(0x0C22 /* GL_COLOR_CLEAR_VALUE */, color);
        unsigned char scissor = f.isEnabled(0x0C11 /* GL_SCISSOR_TEST */);
        if(scissor)
            f.disable(0x0C11);
        f.colorMask(0, 0, 0, 1);
        f.clearColor(0, 0, 0, 1);
        f.clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
        f.colorMask(mask[0], mask[1], mask[2], mask[3]);
        f.clearColor(color[0], color[1], color[2], color[3]);
        if(scissor)
            f.enable(0x0C11);
    }
    f.bindFb(0x8CA9, prevDraw);
}
EGLBoolean eglSwapBuffers(EGLDisplay, EGLSurface surface) {
    if(isWindowSurface(surface) && host::window()) {
        forceOpaqueAlpha();
        maybeScreenshot();
        host::window()->swapBuffers();
        countFrame();
    }
    return EGL_TRUE;
}
EGLBoolean eglSwapBuffersWithDamage(EGLDisplay d, EGLSurface s, const EGLint*, EGLint) { return eglSwapBuffers(d, s); }
EGLBoolean eglSwapInterval(EGLDisplay, EGLint interval) {
    if(host::window())
        host::window()->setSwapInterval(interval);
    return EGL_TRUE;
}
EGLBoolean eglBindAPI(EGLenum api) {
    boundApi = api;
    return api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE;
}
EGLenum eglQueryAPI() { return boundApi; }
EGLBoolean eglTrue() { return EGL_TRUE; }
EGLBoolean eglPresentationTimeANDROID(EGLDisplay, EGLSurface, int64_t) { return EGL_TRUE; }
void* eglGetProcAddress(const char* name) {
    if(strncmp(name, "egl", 3) == 0) {
        void* lib = linker::dlopen("libEGL.so", 0);
        void* r = lib ? linker::dlsym(lib, name) : nullptr;
        if(r)
            return r;
    }
    return host::glProc(name);
}

}  // namespace fegl

void fakelibs::installEGL() {
    std::unordered_map<std::string, void*> s;
#define E(n) s[#n] = (void*)fegl::n
    E(eglGetDisplay); E(eglGetPlatformDisplay); E(eglInitialize); E(eglTerminate); E(eglGetError);
    E(eglQueryString); E(eglGetConfigs); E(eglChooseConfig); E(eglGetConfigAttrib);
    E(eglCreateWindowSurface); E(eglCreatePlatformWindowSurface); E(eglCreatePbufferSurface);
    E(eglDestroySurface); E(eglSurfaceAttrib); E(eglCreateContext); E(eglDestroyContext);
    E(eglMakeCurrent); E(eglGetCurrentContext); E(eglGetCurrentSurface); E(eglGetCurrentDisplay);
    E(eglQueryContext); E(eglQuerySurface); E(eglSwapBuffers); E(eglSwapInterval); E(eglBindAPI);
    E(eglQueryAPI); E(eglPresentationTimeANDROID); E(eglGetProcAddress);
#undef E
    s["eglGetPlatformDisplayEXT"] = (void*)fegl::eglGetPlatformDisplay;
    s["eglCreatePlatformWindowSurfaceEXT"] = (void*)fegl::eglCreatePlatformWindowSurface;
    s["eglSwapBuffersWithDamageKHR"] = (void*)fegl::eglSwapBuffersWithDamage;
    s["eglSwapBuffersWithDamageEXT"] = (void*)fegl::eglSwapBuffersWithDamage;
    for(auto n : {"eglWaitClient", "eglWaitGL", "eglWaitNative", "eglReleaseThread"})
        s[n] = (void*)fegl::eglTrue;
    linker::load_library("libEGL.so", s);

    // GLES 2 / 3 entry points from the host driver
    std::unordered_map<std::string, void*> gles;
    for(int i = 0; glesv2_symbols[i]; i++) {
        if(void* p = host::glProc(glesv2_symbols[i]))
            gles[glesv2_symbols[i]] = p;
    }
    linker::load_library("libGLESv2.so", gles);
    linker::load_library("libGLESv3.so", gles);
}
