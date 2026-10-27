// TrimUI motor output: the production backend against simulated SDL and the
// three sysfs attributes MainUI itself writes. No pad is present, because these
// handhelds expose no force-feedback device.
#ifdef NDEBUG
#error "Haptics tests require assertions enabled"
#endif
#include "haptics_backend.h"
#include <SDL2/SDL.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

extern "C" {
int SDL_NumJoysticks() { return 0; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int) { assert(false); return 0; }
SDL_Joystick* SDL_JoystickOpen(int) { assert(false); return nullptr; }
void SDL_JoystickClose(SDL_Joystick*) { assert(false); }
SDL_bool SDL_JoystickGetAttached(SDL_Joystick*) { assert(false); return SDL_FALSE; }
const char* SDL_JoystickName(SDL_Joystick*) { assert(false); return nullptr; }
Uint16 SDL_JoystickGetVendor(SDL_Joystick*) { assert(false); return 0; }
Uint16 SDL_JoystickGetProduct(SDL_Joystick*) { assert(false); return 0; }
const char* SDL_GetError() { return "no pad in this test"; }
int SDL_JoystickRumble(SDL_Joystick*, Uint16, Uint16, Uint32)
{
    assert(false); // The motor must be preferred over any SDL output.
    return -1;
}
}

namespace {
// Attribute values, last write wins, exactly like sysfs.
std::string motor_value = "0";
std::string motor_direction = "in";
std::string motor_level = "500000";
constexpr int value_fd = 201;
constexpr int direction_fd = 202;
constexpr int level_fd = 203;

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
}

extern "C" {
int __wrap_open(const char* path, int flags, ...)
{
    assert((flags & O_ACCMODE) == O_WRONLY); // Never read the attributes back.
    if (std::strcmp(path, "/sys/class/gpio/gpio227/value") == 0) return value_fd;
    if (std::strcmp(path, "/sys/class/gpio/gpio227/direction") == 0)
        return direction_fd;
    assert(std::strcmp(path, "/sys/class/motor/voltage") == 0);
    return level_fd;
}
int __wrap_close(int fd)
{
    assert(fd == value_fd || fd == direction_fd || fd == level_fd);
    return 0;
}
off_t __wrap_lseek(int, off_t offset, int whence)
{
    assert(offset == 0 && whence == SEEK_SET);
    return 0;
}
ssize_t __wrap_write(int fd, const void* buffer, size_t count)
{
    std::string& attribute = fd == value_fd       ? motor_value
                           : fd == level_fd       ? motor_level
                                                  : motor_direction;
    attribute.assign(static_cast<const char*>(buffer), count);
    return static_cast<ssize_t>(count);
}
}

int main()
{
    using namespace bd_haptics;
    motor_value = "1"; // Left on by a run that was killed mid-pulse.
    assert(has_vibrator()); // A motor counts even though no pad has FF.
    assert(motor_direction == "out"); // Probe leaves it switched as an output.
    assert(motor_value == "0"); // Probe parks it off, whoever left it on.
    assert(motor_level == "500000");

    refresh_devices(); // No joysticks at all: the motor is the only output.
    update();
    assert(motor_value == "0" && motor_level == "500000");

    vibrate(500);
    update();
    assert(motor_value == "1"); // On at full level.
    assert(motor_level == "3500000");

    sleep_ms(800);
    update();
    assert(motor_value == "0"); // The tick stops it at the deadline.
    assert(motor_level == "500000");

    // A short pulse is stretched to a quarter second, and so is its deadline.
    vibrate(120);
    update();
    assert(motor_value == "1");
    sleep_ms(180);
    update();
    assert(motor_value == "1"); // Still running past the requested length.
    sleep_ms(300);
    update();
    assert(motor_value == "0");

    vibrate(500);
    update();
    assert(motor_value == "1");
    cancel();
    update();
    assert(motor_value == "0");

    vibrate(500);
    update();
    assert(motor_value == "1");
    set_suspended(true);
    assert(motor_value == "0"); // Standby never leaves the motor running.
    vibrate(500);
    update();
    assert(motor_value == "0"); // Requested while suspended: ignored.
    set_suspended(false);
    update();
    assert(motor_value == "0"); // Wake never replays it.

    vibrate(500);
    update();
    assert(motor_value == "1");
    prepare_exit();
    assert(motor_value == "0"); // Stopped without waiting for the input loop.
    assert(!has_vibrator());

    vibrate(500);
    update();
    assert(motor_value == "0"); // Nothing runs after exit.
    shutdown();
    shutdown();
    assert(motor_value == "0" && motor_level == "500000");

    std::printf("PASS: TrimUI motor on/off, minimum pulse, cancel, standby, exit\n");
}
