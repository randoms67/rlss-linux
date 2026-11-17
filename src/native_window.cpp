// ANativeWindow implementation. The ANativeWindow* handed to the app is the
// GameWindow*. Besides the size getters used by EGL apps, this implements the
// software rendering path (ANativeWindow_lock / unlockAndPost) by uploading the
// CPU buffer into a GLES texture and drawing it full screen.

#include "host.h"

#include <android/native_window.h>
#include <log.h>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;
typedef char GLchar;

struct GL {
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLenum);
    void (*GenTextures)(GLsizei, GLuint*);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
    void (*PixelStorei)(GLenum, GLint);
    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*CompileShader)(GLuint);
    GLuint (*CreateProgram)();
    void (*AttachShader)(GLuint, GLuint);
    void (*BindAttribLocation)(GLuint, GLuint, const GLchar*);
    void (*LinkProgram)(GLuint);
    void (*UseProgram)(GLuint);
    GLint (*GetUniformLocation)(GLuint, const GLchar*);
    void (*Uniform1i)(GLint, GLint);
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
    void (*EnableVertexAttribArray)(GLuint);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*Disable)(GLenum);
    void (*BindBuffer)(GLenum, GLuint);
    bool loaded = false;

    void load() {
        if(loaded)
            return;
#define L(n) n = (decltype(n))host::glProc("gl" #n)
        L(Viewport); L(ClearColor); L(Clear); L(GenTextures); L(BindTexture); L(TexParameteri);
        L(TexImage2D); L(PixelStorei); L(CreateShader); L(ShaderSource); L(CompileShader);
        L(CreateProgram); L(AttachShader); L(BindAttribLocation); L(LinkProgram); L(UseProgram);
        L(GetUniformLocation); L(Uniform1i); L(VertexAttribPointer); L(EnableVertexAttribArray);
        L(DrawArrays); L(Disable); L(BindBuffer);
#undef L
        loaded = true;
    }
};

struct SoftwareSurface {
    std::mutex mutex;
    int32_t reqWidth = 0, reqHeight = 0, format = WINDOW_FORMAT_RGBA_8888;
    std::vector<uint8_t> pixels;
    int32_t width = 0, height = 0;
    GL gl;
    GLuint program = 0, texture = 0;
};
SoftwareSurface surface;

int bytesPerPixel(int32_t format) {
    return format == WINDOW_FORMAT_RGB_565 ? 2 : 4;
}

void setupProgram() {
    auto& gl = surface.gl;
    if(surface.program)
        return;
    static const char* vs =
        "attribute vec2 pos;\n"
        "varying vec2 uv;\n"
        "void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5); gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char* fs =
        "precision mediump float;\n"
        "varying vec2 uv;\n"
        "uniform sampler2D tex;\n"
        "void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); }\n";
    GLuint v = gl.CreateShader(0x8B31 /* GL_VERTEX_SHADER */);
    gl.ShaderSource(v, 1, &vs, nullptr);
    gl.CompileShader(v);
    GLuint f = gl.CreateShader(0x8B30 /* GL_FRAGMENT_SHADER */);
    gl.ShaderSource(f, 1, &fs, nullptr);
    gl.CompileShader(f);
    surface.program = gl.CreateProgram();
    gl.AttachShader(surface.program, v);
    gl.AttachShader(surface.program, f);
    gl.BindAttribLocation(surface.program, 0, "pos");
    gl.LinkProgram(surface.program);
    gl.GenTextures(1, &surface.texture);
    gl.BindTexture(0x0DE1, surface.texture);
    gl.TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2601 /* LINEAR */);
    gl.TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2601);
    gl.TexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */);
    gl.TexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
}

int32_t getWidth(ANativeWindow*) {
    int w, h;
    host::getContentSize(w, h);
    return w;
}

int32_t getHeight(ANativeWindow*) {
    int w, h;
    host::getContentSize(w, h);
    return h;
}

int32_t lock(ANativeWindow* window, ANativeWindow_Buffer* out, ARect* dirty) {
    std::lock_guard<std::mutex> l(surface.mutex);
    // With setBuffersGeometry(w, h) the buffer has a fixed size and is scaled
    surface.width = surface.reqWidth ? surface.reqWidth : getWidth(window);
    surface.height = surface.reqHeight ? surface.reqHeight : getHeight(window);
    size_t size = (size_t)surface.width * surface.height * bytesPerPixel(surface.format);
    if(surface.pixels.size() != size)
        surface.pixels.assign(size, 0);
    out->width = surface.width;
    out->height = surface.height;
    out->stride = surface.width;
    out->format = surface.format;
    out->bits = surface.pixels.data();
    if(dirty) {
        dirty->left = dirty->top = 0;
        dirty->right = surface.width;
        dirty->bottom = surface.height;
    }
    return 0;
}

int32_t unlockAndPost(ANativeWindow* window) {
    std::lock_guard<std::mutex> l(surface.mutex);
    auto w = host::window();
    if(!w)
        return -1;
    auto& gl = surface.gl;
    w->makeCurrent(true);
    gl.load();
    setupProgram();
    int ww, wh;
    w->getWindowSize(ww, wh);
    gl.Viewport(0, 0, ww, wh);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(0x4000);
    gl.Disable(0x0B71 /* DEPTH_TEST */);
    gl.Disable(0x0BE2 /* BLEND */);
    gl.UseProgram(surface.program);
    gl.BindTexture(0x0DE1, surface.texture);
    gl.PixelStorei(0x0CF5 /* UNPACK_ALIGNMENT */, 1);
    if(surface.format == WINDOW_FORMAT_RGB_565)
        gl.TexImage2D(0x0DE1, 0, 0x1907 /* RGB */, surface.width, surface.height, 0, 0x1907, 0x8363 /* UNSIGNED_SHORT_5_6_5 */, surface.pixels.data());
    else
        gl.TexImage2D(0x0DE1, 0, 0x1908 /* RGBA */, surface.width, surface.height, 0, 0x1908, 0x1401 /* UNSIGNED_BYTE */, surface.pixels.data());
    gl.Uniform1i(gl.GetUniformLocation(surface.program, "tex"), 0);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    gl.BindBuffer(0x8892 /* ARRAY_BUFFER */, 0);
    gl.VertexAttribPointer(0, 2, 0x1406 /* FLOAT */, 0, 0, quad);
    gl.EnableVertexAttribArray(0);
    gl.DrawArrays(0x0005 /* TRIANGLE_STRIP */, 0, 4);
    w->swapBuffers();
    return 0;
}

}  // namespace

void fakelibs::installNativeWindow(std::unordered_map<std::string, void*>& s) {
    s["ANativeWindow_getWidth"] = (void*)getWidth;
    s["ANativeWindow_getHeight"] = (void*)getHeight;
    s["ANativeWindow_getFormat"] = (void*)+[](ANativeWindow*) -> int32_t { return surface.format; };
    s["ANativeWindow_setBuffersGeometry"] = (void*)+[](ANativeWindow*, int32_t w, int32_t h, int32_t format) -> int32_t {
        surface.reqWidth = w;
        surface.reqHeight = h;
        if(format)
            surface.format = format;
        return 0;
    };
    s["ANativeWindow_setBuffersTransform"] = (void*)+[](ANativeWindow*, int32_t) -> int32_t { return 0; };
    s["ANativeWindow_setBuffersDataSpace"] = (void*)+[](ANativeWindow*, int32_t) -> int32_t { return 0; };
    s["ANativeWindow_getBuffersDataSpace"] = (void*)+[](ANativeWindow*) -> int32_t { return 0; };
    s["ANativeWindow_setFrameRate"] = (void*)+[](ANativeWindow*, float, int8_t) -> int32_t { return 0; };
    s["ANativeWindow_setFrameRateWithChangeStrategy"] = (void*)+[](ANativeWindow*, float, int8_t, int8_t) -> int32_t { return 0; };
    s["ANativeWindow_acquire"] = (void*)+[](ANativeWindow*) {};
    s["ANativeWindow_release"] = (void*)+[](ANativeWindow*) {};
    s["ANativeWindow_fromSurface"] = (void*)+[](void*, void*) -> ANativeWindow* { return (ANativeWindow*)host::window(); };
    s["ANativeWindow_toSurface"] = (void*)+[](void*, ANativeWindow*) -> void* { return nullptr; };
    s["ANativeWindow_lock"] = (void*)lock;
    s["ANativeWindow_unlockAndPost"] = (void*)unlockAndPost;
}
