// Link the production backend against a simulated SDL driver. No motor needed.
#ifdef NDEBUG
#error "Haptics tests require assertions enabled"
#endif
#include "haptics_backend.h"
#include <SDL2/SDL.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>

struct _SDL_Joystick {
    int id;
    bool attached, capable, fail;
    int references = 0;
};
static _SDL_Joystick pads[] = {{11, true, false, false}, {22, true, true, false}};
struct Output { int id; Uint16 low, high; Uint32 duration; };
static std::vector<Output> output;
static bool legacy;
static int scan;
static std::thread::id owner;

static SDL_Joystick* pad_at(int i)
{
    for (auto& pad : pads) if (pad.attached && i-- == 0) return &pad;
    assert(false); return nullptr;
}

extern "C" {
int SDL_NumJoysticks() { return static_cast<int>(pads[0].attached) + static_cast<int>(pads[1].attached); }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int i) { return pad_at(i)->id; }
SDL_Joystick* SDL_JoystickOpen(int i) { auto* j = pad_at(i); ++j->references; return j; }
void SDL_JoystickClose(SDL_Joystick* j) { assert(j->references > 0); --j->references; }
SDL_bool SDL_JoystickGetAttached(SDL_Joystick* j) { return j->attached ? SDL_TRUE : SDL_FALSE; }
const char* SDL_JoystickName(SDL_Joystick* j) { return j->id == 11 ? "no motor" : "test motor"; }
Uint16 SDL_JoystickGetVendor(SDL_Joystick*) { return 1; }
Uint16 SDL_JoystickGetProduct(SDL_Joystick*) { return 2; }
const char* SDL_GetError() { return "simulated output failure"; }
SDL_bool test_has_rumble(SDL_Joystick* j) { return j->capable ? SDL_TRUE : SDL_FALSE; }
int SDL_JoystickRumble(SDL_Joystick* j, Uint16 low, Uint16 high, Uint32 duration)
{
    assert(std::this_thread::get_id() == owner);
    output.push_back({j->id, low, high, duration});
    return j->fail ? -1 : 0;
}

void* __wrap_dlsym(void*, const char* name)
{
    assert(std::strcmp(name, "SDL_JoystickHasRumble") == 0);
    return legacy ? nullptr : reinterpret_cast<void*>(&test_has_rumble);
}
DIR* __wrap_opendir(const char* path)
{
    assert(std::strcmp(path, "/dev/input") == 0);
    scan = 0;
    return reinterpret_cast<DIR*>(1);
}
dirent* __wrap_readdir(DIR*)
{
    static dirent entry{};
    if (scan++ != 0) return nullptr;
    std::strcpy(entry.d_name, "event8");
    return &entry;
}
int __wrap_closedir(DIR*) { return 0; }
int __wrap_open(const char* path, int flags, ...)
{
    // The TrimUI motor probe runs before any pad is consulted and opens both
    // attributes before it gives up; this machine has no motor, so the SDL path
    // below must still be exercised.
    if (std::strncmp(path, "/sys/class/", 11) == 0) {
        assert(std::strcmp(path, "/sys/class/gpio/gpio227/value") == 0 ||
               std::strcmp(path, "/sys/class/motor/voltage") == 0);
        assert((flags & O_ACCMODE) == O_WRONLY);
        return -1;
    }
    assert(std::strcmp(path, "/dev/input/event8") == 0);
    assert((flags & O_ACCMODE) == O_RDONLY); // Capability probe never writes.
    return 123;
}
int __wrap_close(int fd) { assert(fd == 123); return 0; }
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 123);
    va_list args; va_start(args, request);
    void* data = va_arg(args, void*); va_end(args);
    if (_IOC_NR(request) == _IOC_NR(EVIOCGNAME(256))) {
        std::strcpy(static_cast<char*>(data), "test motor");
    } else if (request == EVIOCGID) {
        *static_cast<input_id*>(data) = {0x19, 1, 2, 1};
    } else {
        assert(_IOC_NR(request) == _IOC_NR(EVIOCGBIT(EV_FF, 16)));
        constexpr unsigned bits = 8 * sizeof(unsigned long);
        static_cast<unsigned long*>(data)[FF_RUMBLE / bits] |= 1UL << (FF_RUMBLE % bits);
    }
    return 0;
}
}

int main(int argc, char**)
{
    legacy = argc > 1;
    owner = std::this_thread::get_id();
    using namespace bd_haptics;
    assert(!has_vibrator());
    vibrate(500); // No device: do not replay when a device later appears.
    refresh_devices();
    assert(has_vibrator() && output.empty());
    update(); assert(output.empty());
    refresh_devices(); assert(pads[0].references == 1 && pads[1].references == 1);

    std::thread requester([] { vibrate(500); vibrate(50); });
    requester.join();
    assert(output.empty()); // JNI/worker thread did not call SDL.
    update();
    assert(output.size() == 1 && output.back().id == 22);
    assert(output.back().low == 0xffff && output.back().high == 0xffff);
    assert(output.back().duration > 0 && output.back().duration <= 50);
    update(); assert(output.size() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    update(); assert(output.back().duration == 0 && output.back().low == 0);

    vibrate(500); cancel(); update();
    const auto canceled = output.size();
    assert(canceled == 2); // Cancel before dispatch produces no pulse.
    vibrate(INT64_MAX); update();
    assert(output.back().duration <= 60000 && output.back().duration > 59000);
    const auto active = output.size();
    vibrate(0); vibrate(-1); update(); assert(output.size() == active);
    cancel(); update(); assert(output.back().duration == 0);

    vibrate(1); std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto expired = output.size();
    update(); assert(output.size() == expired); // No delayed stale pulse.
    vibrate(500); update(); set_suspended(true);
    assert(output.back().duration == 0 && has_vibrator());
    const auto suspended = output.size();
    vibrate(500); update(); set_suspended(false); update();
    assert(output.size() == suspended);

    vibrate(500); update();
    pads[1].attached = false;
    refresh_devices();
    assert(output.back().duration == 0 && !has_vibrator());
    assert(pads[1].references == 0);
    pads[1].attached = true;
    refresh_devices(); update();
    assert(has_vibrator() && output.back().duration == 0);

    pads[1].fail = true;
    vibrate(500); update(); assert(!has_vibrator());
    const auto failed = output.size();
    vibrate(500); update(); assert(output.size() == failed);
    pads[1].attached = false; refresh_devices();
    pads[1].attached = true; pads[1].fail = false; refresh_devices();
    assert(has_vibrator());

    vibrate(500); update();
    std::thread exiting([] { prepare_exit(); });
    while (has_vibrator()) std::this_thread::yield();
    update(); exiting.join();
    assert(output.back().duration == 0);
    const auto stopped = output.size();
    vibrate(500); update(); assert(output.size() == stopped);
    shutdown(); shutdown();
    assert(pads[0].references == 0 && pads[1].references == 0);
    std::printf("PASS: %s capabilities, worker requests, replacement, expiry, cancel, standby, detach, failure, exit\n",
                legacy ? "SDL 2.0.10/2.0.12 evdev" : "SDL 2.0.18+ API");
}
