// Test harness: APKL_CONTROL=<fifo path> lets a script drive the app.
// Commands (one per line):
//   tap X Y            touch down + up at window pixel X,Y
//   drag X1 Y1 X2 Y2   swipe
//   key CODE           Android key code down + up (4 = BACK, 66 = ENTER)
//   fullscreen         toggle fullscreen (like F11)
//   shot FILE.png      save the next frame the app presents

#include "control.h"
#include "host.h"
#include "looper_input.h"

#include <log.h>
#include <zlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex shotMutex;
std::string pendingShot;

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void handle(const std::string& line) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if(cmd == "tap") {
        float x, y;
        in >> x >> y;
        input::sendTouch(AMOTION_EVENT_ACTION_DOWN, 0, x, y);
        sleepMs(90);
        input::sendTouch(AMOTION_EVENT_ACTION_UP, 0, x, y);
    } else if(cmd == "drag") {
        float x1, y1, x2, y2;
        in >> x1 >> y1 >> x2 >> y2;
        input::sendTouch(AMOTION_EVENT_ACTION_DOWN, 0, x1, y1);
        for(int i = 1; i <= 10; i++) {
            sleepMs(30);
            input::sendTouch(AMOTION_EVENT_ACTION_MOVE, 0, x1 + (x2 - x1) * i / 10, y1 + (y2 - y1) * i / 10);
        }
        input::sendTouch(AMOTION_EVENT_ACTION_UP, 0, x2, y2);
    } else if(cmd == "key") {
        int code;
        in >> code;
        input::sendKey(code, AKEY_EVENT_ACTION_DOWN, 0);
        sleepMs(60);
        input::sendKey(code, AKEY_EVENT_ACTION_UP, 0);
    } else if(cmd == "fullscreen") {
        if(auto w = host::window())
            w->setFullscreen(!w->getFullscreen());
    } else if(cmd == "shot") {
        std::string file;
        in >> file;
        std::lock_guard<std::mutex> l(shotMutex);
        pendingShot = file;
    } else if(!cmd.empty()) {
        Log::warn("Control", "unknown command: %s", line.c_str());
    }
}

void put32(std::string& s, uint32_t v) {
    char b[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    s.append(b, 4);
}

void chunk(std::string& out, const char* type, const std::string& data) {
    put32(out, (uint32_t)data.size());
    std::string body = std::string(type, 4) + data;
    out += body;
    put32(out, (uint32_t)crc32(0, (const Bytef*)body.data(), (uInt)body.size()));
}
}  // namespace

void control::install() {
    const char* path = getenv("APKL_CONTROL");
    if(!path)
        return;
    std::string p = path;
    mkfifo(p.c_str(), 0600);
    Log::info("Control", "listening for commands on %s", p.c_str());
    std::thread([p]() {
        while(true) {
            std::ifstream in(p);
            std::string line;
            while(std::getline(in, line))
                handle(line);
        }
    }).detach();
}

std::string control::takeScreenshotRequest() {
    std::lock_guard<std::mutex> l(shotMutex);
    std::string r;
    r.swap(pendingShot);
    return r;
}

bool control::writePng(const std::string& file, int w, int h, const unsigned char* rgbaBottomUp) {
    // rows bottom-up (GL) -> PNG top-down, filter byte 0 per row
    std::string raw;
    raw.reserve((size_t)(w * 4 + 1) * h);
    for(int y = h - 1; y >= 0; y--) {
        raw.push_back(0);
        raw.append((const char*)rgbaBottomUp + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf len = compressBound(raw.size());
    std::string z(len, '\0');
    if(compress2((Bytef*)&z[0], &len, (const Bytef*)raw.data(), raw.size(), 6) != Z_OK)
        return false;
    z.resize(len);
    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    put32(ihdr, w);
    put32(ihdr, h);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8 bit RGBA
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", "");
    std::ofstream out(file, std::ios::binary);
    out << png;
    return (bool)out;
}
