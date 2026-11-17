// Gamepad bridge for the ARM translation layer.
//
// Under qemu-user the emulated launcher cannot open controllers (the evdev /
// hidraw ioctls are not translated). The native x86_64 launcher runs as
// `apk-launcher --gamepad-bridge <socket>`, reads controllers with SDL and
// streams their events to the emulated launcher (APKL_GAMEPAD_BRIDGE=<socket>).
//
// Protocol, one event per line:
//   A <id> <vendor> <product> <name...>   controller added
//   R <id>                                controller removed
//   B <id> <GamepadButtonId> <0|1>        button
//   X <id> <GamepadAxisId> <value>        axis, -1..1 (triggers 0..1)

#include "gamepad_bridge.h"
#include "host.h"

#include <SDL3/SDL.h>
#include <log.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

namespace {

GamepadButtonId mapButton(Uint8 b) {
    switch(b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return GamepadButtonId::A;
    case SDL_GAMEPAD_BUTTON_EAST: return GamepadButtonId::B;
    case SDL_GAMEPAD_BUTTON_WEST: return GamepadButtonId::X;
    case SDL_GAMEPAD_BUTTON_NORTH: return GamepadButtonId::Y;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return GamepadButtonId::LB;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GamepadButtonId::RB;
    case SDL_GAMEPAD_BUTTON_BACK: return GamepadButtonId::BACK;
    case SDL_GAMEPAD_BUTTON_START: return GamepadButtonId::START;
    case SDL_GAMEPAD_BUTTON_GUIDE: return GamepadButtonId::GUIDE;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return GamepadButtonId::LEFT_STICK;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return GamepadButtonId::RIGHT_STICK;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return GamepadButtonId::DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return GamepadButtonId::DPAD_RIGHT;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return GamepadButtonId::DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return GamepadButtonId::DPAD_LEFT;
    default: return GamepadButtonId::UNKNOWN;
    }
}

GamepadAxisId mapAxis(Uint8 a) {
    switch(a) {
    case SDL_GAMEPAD_AXIS_LEFTX: return GamepadAxisId::LEFT_X;
    case SDL_GAMEPAD_AXIS_LEFTY: return GamepadAxisId::LEFT_Y;
    case SDL_GAMEPAD_AXIS_RIGHTX: return GamepadAxisId::RIGHT_X;
    case SDL_GAMEPAD_AXIS_RIGHTY: return GamepadAxisId::RIGHT_Y;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return GamepadAxisId::LEFT_TRIGGER;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return GamepadAxisId::RIGHT_TRIGGER;
    default: return GamepadAxisId::UNKNOWN;
    }
}

bool sendLine(int fd, const std::string& line) {
    std::string l = line + "\n";
    return send(fd, l.data(), l.size(), MSG_NOSIGNAL) == (ssize_t)l.size();
}

std::string addLine(SDL_JoystickID id) {
    const char* name = SDL_GetGamepadNameForID(id);
    char buf[64];
    snprintf(buf, sizeof(buf), "A %u %u %u ", (unsigned)id, (unsigned)SDL_GetGamepadVendorForID(id),
             (unsigned)SDL_GetGamepadProductForID(id));
    std::string n = name ? name : "Gamepad";
    for(auto& c : n)
        if(c == '\n')
            c = ' ';
    return buf + n;
}

}  // namespace

int gamepadbridge::serve(const std::string& path) {
    // stop when the launcher that started us goes away
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    unlink(path.c_str());
    if(bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(srv, 1) != 0) {
        perror("gamepad bridge: bind");
        return 1;
    }
    int fd = accept(srv, nullptr, nullptr);
    close(srv);
    unlink(path.c_str());
    if(fd < 0)
        return 1;
    // controllers must keep working while the (other process') window has focus
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if(!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        fprintf(stderr, "gamepad bridge: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Event ev;
    while(true) {
        if(!SDL_WaitEventTimeout(&ev, 500)) {
            // detect a closed connection even without events
            if(!sendLine(fd, ""))
                break;
            continue;
        }
        std::string line;
        switch(ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            SDL_OpenGamepad(ev.gdevice.which);
            line = addLine(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            line = "R " + std::to_string(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            line = "B " + std::to_string(ev.gbutton.which) + " " + std::to_string((int)mapButton(ev.gbutton.button)) + " " +
                   (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "1" : "0");
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            line = "X " + std::to_string(ev.gaxis.which) + " " + std::to_string((int)mapAxis(ev.gaxis.axis)) + " " +
                   std::to_string(ev.gaxis.value / 32767.0f);
            break;
        default:
            continue;
        }
        if(!sendLine(fd, line))
            break;
    }
    close(fd);
    SDL_Quit();
    return 0;
}

void gamepadbridge::connectIfConfigured() {
    const char* p = getenv("APKL_GAMEPAD_BRIDGE");
    if(!p || !*p)
        return;
    std::string path = p;
    std::thread([path]() {
        int fd = -1;
        for(int i = 0; i < 100 && fd < 0; i++) {
            fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
            if(connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
                close(fd);
                fd = -1;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if(fd < 0) {
            Log::warn("Gamepad", "cannot reach the gamepad bridge at %s", path.c_str());
            return;
        }
        Log::info("Gamepad", "using the native gamepad bridge");
        std::string buf;
        char chunk[4096];
        ssize_t n;
        while((n = read(fd, chunk, sizeof(chunk))) > 0) {
            buf.append(chunk, n);
            size_t nl;
            while((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::istringstream in(line);
                char t = 0;
                int id = 0;
                in >> t >> id;
                if(t == 'A') {
                    int vendor = 0, product = 0;
                    in >> vendor >> product;
                    std::string name;
                    std::getline(in, name);
                    if(!name.empty() && name[0] == ' ')
                        name.erase(0, 1);
                    host::gamepadConnected(id, true, name, vendor, product);
                } else if(t == 'R') {
                    host::gamepadConnected(id, false, "", 0, 0);
                } else if(t == 'B') {
                    int b = -1, state = 0;
                    in >> b >> state;
                    if(b >= 0)
                        host::gamepadButton(id, (GamepadButtonId)b, state != 0);
                } else if(t == 'X') {
                    int a = -1;
                    float v = 0;
                    in >> a >> v;
                    if(a >= 0)
                        host::gamepadAxis(id, (GamepadAxisId)a, v);
                }
            }
        }
        close(fd);
        Log::warn("Gamepad", "gamepad bridge closed");
    }).detach();
}
