#include "haptics_backend.h"
#include "logging.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace bd_haptics {
namespace {
using Clock = std::chrono::steady_clock;
// Bound a legacy one-shot both for SDL 2.0.10's effect duration and bad guest
// values. Normal Unity Handheld.Vibrate requests are much shorter (500 ms).
constexpr int64_t max_duration_ms = 60000;
// A coin motor needs a moment to spin up, so a 100 ms hit is close to
// invisible; every motor pulse gets at least this long.
constexpr int64_t motor_min_pulse_ms = 250;
constexpr const char* motor_level_on = "3500000";
constexpr const char* motor_level_off = "500000";
struct Device {
    SDL_Joystick* joystick;
    SDL_JoystickID id;
    bool capable;
};
// TrimUI handhelds expose no force-feedback device on the pad, so SDL rejects
// every rumble with "That operation is not supported" and the motor never
// moves. Their firmware drives it from userspace instead: gpio227 switches the
// motor on and /sys/class/motor/voltage sets the strength (500000 = off,
// 2000000..3500000 = level); MainUI's own rumble setting writes the same two
// files. Prefer that path where it exists and keep SDL for pads that really do
// carry an FF_RUMBLE device.
struct Motor {
    int value = -1; // /sys/class/gpio/gpio227/value; -1 = unusable
    int level = -1; // /sys/class/motor/voltage
    bool probed = false;
    bool on = false;
};
struct State {
    std::mutex mutex;
    std::condition_variable stopped;
    std::vector<Device> devices;
    Motor motor;
    SDL_Joystick* playing = nullptr;
    Clock::time_point until{};
    bool dirty = false;
    bool suspended = false;
    bool exiting = false;
};
State& state()
{
    // InputBackend can be constructed before the first haptics call (e.g.
    // controller input disabled). Keep state alive through its static dtor;
    // shutdown() explicitly releases every SDL reference before SDL_Quit().
    static State* const s = new State;
    return *s;
}

bool capable_locked(const State& s)
{
    return std::any_of(s.devices.begin(), s.devices.end(),
                       [](const Device& d) { return d.capable; });
}

// SDL_JoystickHasRumble was added in 2.0.18; the build SDK is 2.0.10.
// On older Linux SDL, query evdev capabilities without uploading/playing an
// effect. In particular a zero-strength SDL rumble call is NOT a capability
// test: SDL can short-circuit it and return success on unsupported devices.
bool can_rumble(SDL_Joystick* joystick)
{
    using HasRumble = SDL_bool (*)(SDL_Joystick*);
    static auto has_rumble = reinterpret_cast<HasRumble>(
        dlsym(RTLD_DEFAULT, "SDL_JoystickHasRumble"));
    if (has_rumble)
        return has_rumble(joystick) == SDL_TRUE;
#ifdef __linux__
    const char* name = SDL_JoystickName(joystick);
    if (!name) return false;
    DIR* dir = opendir("/dev/input");
    if (!dir) return false;
    bool found = false;
    while (auto* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + entry->d_name;
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) continue;
        char ev_name[256]{};
        input_id id{};
        constexpr size_t word_bits = sizeof(unsigned long) * 8;
        unsigned long bits[(FF_MAX / word_bits) + 1]{};
        found = ioctl(fd, EVIOCGNAME(sizeof(ev_name)), ev_name) >= 0 &&
                std::strcmp(name, ev_name) == 0 &&
                ioctl(fd, EVIOCGID, &id) >= 0 &&
                id.vendor == SDL_JoystickGetVendor(joystick) &&
                id.product == SDL_JoystickGetProduct(joystick) &&
                ioctl(fd, EVIOCGBIT(EV_FF, sizeof(bits)), bits) >= 0 &&
                ((bits[FF_RUMBLE / word_bits] >> (FF_RUMBLE % word_bits)) & 1);
        close(fd);
        if (found) break;
    }
    closedir(dir);
    return found;
#else
    return false;
#endif
}

#ifdef __linux__
void motor_write(int fd, const char* text)
{
    if (fd < 0) return;
    // sysfs attributes ignore the offset, but seeking keeps a repeated write
    // predictable. A refused write must never take the game down with it.
    const off_t seeked = lseek(fd, 0, SEEK_SET);
    const ssize_t written = write(fd, text, std::strlen(text));
    (void)seeked;
    (void)written;
}

void motor_off(State& s)
{
    if (s.motor.value < 0) return;
    motor_write(s.motor.level, motor_level_off);
    motor_write(s.motor.value, "0");
    s.motor.on = false;
}

void motor_on(State& s)
{
    // Park at rest, then switch on and raise the level: the order MainUI uses,
    // so every pulse starts from a known state.
    motor_write(s.motor.level, motor_level_off);
    motor_write(s.motor.value, "1");
    motor_write(s.motor.level, motor_level_on);
    s.motor.on = true;
}

bool motor_ready(State& s)
{
    Motor& m = s.motor;
    if (m.probed) return m.value >= 0;
    m.probed = true;
    const int value = open("/sys/class/gpio/gpio227/value", O_WRONLY | O_CLOEXEC);
    const int level = open("/sys/class/motor/voltage", O_WRONLY | O_CLOEXEC);
    if (value < 0 || level < 0) {
        if (value >= 0) close(value);
        if (level >= 0) close(level);
        return false;
    }
    m.value = value;
    m.level = level;
    // A previous run killed mid-pulse leaves the switch on and nobody to clear
    // it, so every start begins from the resting state.
    motor_off(s);
    const int direction =
        open("/sys/class/gpio/gpio227/direction", O_WRONLY | O_CLOEXEC);
    if (direction >= 0) {
        motor_write(direction, "out");
        close(direction);
    }
    BD_LOG("HAPTICS", "TrimUI motor (gpio227 + motor/voltage)");
    return true;
}

void motor_close(State& s)
{
    motor_off(s);
    if (s.motor.value >= 0) close(s.motor.value);
    if (s.motor.level >= 0) close(s.motor.level);
    s.motor.value = s.motor.level = -1;
}
#else
bool motor_ready(State&) { return false; }
void motor_on(State&) {}
void motor_off(State&) {}
void motor_close(State&) {}
#endif

void stop_locked(State& s)
{
    motor_off(s); // Unlike an SDL effect, nothing stops the motor on its own.
    if (s.playing) {
        if (SDL_JoystickRumble(s.playing, 0, 0, 0) != 0)
            BD_LOG("HAPTICS", "stop failed: %s", SDL_GetError());
        s.playing = nullptr;
    }
}

void clear_locked(State& s)
{
    s.until = {};
    s.dirty = false;
    stop_locked(s);
    s.stopped.notify_all();
}
}

bool has_vibrator()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return !s.exiting && (motor_ready(s) || capable_locked(s));
}

void vibrate(int64_t milliseconds)
{
    // Invalid Android durations do not replace an existing valid request.
    if (milliseconds <= 0) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.exiting || s.suspended) return;
    if (!motor_ready(s) && !capable_locked(s)) return;
    s.until = Clock::now() + std::chrono::milliseconds(
        std::min(milliseconds, max_duration_ms));
    s.dirty = true; // Latest request replaces the previous one, never queues up.
}

void cancel()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.until = {};
    s.dirty = true;
}

void refresh_devices()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.exiting) return;
    motor_ready(s); // Log the output once per run, not once per pulse.
    for (auto it = s.devices.begin(); it != s.devices.end();) {
        if (!SDL_JoystickGetAttached(it->joystick)) {
            if (s.playing == it->joystick || it->capable) clear_locked(s);
            SDL_JoystickClose(it->joystick);
            it = s.devices.erase(it);
        } else {
            ++it;
        }
    }
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        const auto id = SDL_JoystickGetDeviceInstanceID(i);
        if (std::any_of(s.devices.begin(), s.devices.end(),
                        [id](const Device& d) { return d.id == id; })) continue;
        auto* joystick = SDL_JoystickOpen(i);
        if (!joystick) continue;
        const bool capable = can_rumble(joystick);
        // Own one SDL reference, independent of GameController mappings.
        s.devices.push_back({joystick, id, capable});
        BD_LOG("HAPTICS", "device='%s' instance=%d rumble=%d",
               SDL_JoystickName(joystick), static_cast<int>(id), capable);
    }
}

void update()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const auto now = Clock::now();
    if (s.exiting || s.suspended || s.until <= now) {
        if (s.dirty || s.playing || s.motor.on) clear_locked(s);
        return;
    }
    if (!s.dirty) return;
    s.dirty = false;
    // Round up so a short valid request does not turn into SDL's zero-duration
    // stop command. A separate deadline also stops it if SDL's timer is late.
    auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        s.until - now).count();
    if (motor_ready(s)) {
        // TrimUI: no FF device, so the motor is switched on here and switched
        // off by the deadline check above. Short pulses are stretched, and the
        // deadline with them, or a coin motor would barely twitch.
        if (remaining < motor_min_pulse_ms) {
            remaining = motor_min_pulse_ms;
            s.until = now + std::chrono::milliseconds(remaining);
        }
        motor_on(s);
        BD_DEBUG("HAPTICS", "motor on duration=%lld ms",
                 static_cast<long long>(remaining));
        return;
    }
    for (auto& d : s.devices) {
        if (!d.capable) continue;
        if (SDL_JoystickRumble(d.joystick, 0xffff, 0xffff,
                               static_cast<Uint32>(remaining)) == 0) {
            s.playing = d.joystick;
            BD_DEBUG("HAPTICS", "rumble instance=%d duration=%lld ms",
                     static_cast<int>(d.id), static_cast<long long>(remaining));
            return;
        }
        BD_LOG("HAPTICS", "rumble failed instance=%d: %s; disabling until reconnect",
               static_cast<int>(d.id), SDL_GetError());
        stop_locked(s);
        d.capable = false;
    }
    s.until = {};
}

void set_suspended(bool suspended)
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.suspended = suspended;
    clear_locked(s); // Wake never replays a request made before standby.
}

void shutdown()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.exiting = true;
    clear_locked(s);
    motor_close(s);
    for (auto& d : s.devices) SDL_JoystickClose(d.joystick);
    s.devices.clear();
}

void prepare_exit()
{
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.exiting = true;
    s.until = {};
    // Nothing reclaims a sysfs-driven motor when the process dies, unlike an
    // evdev effect, so it is stopped here rather than left to the input thread.
    motor_off(s);
    if (!s.playing) {
        s.dirty = false;
        return;
    }
    s.dirty = true;
    s.stopped.wait_for(lock, std::chrono::milliseconds(250), [&s] {
        return !s.playing && !s.dirty;
    });
    // If the event loop has already died, _exit closes the evdev descriptors
    // and the kernel releases their force-feedback effects.
}
}
