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
struct Device {
    SDL_Joystick* joystick;
    SDL_JoystickID id;
    bool capable;
};
struct State {
    std::mutex mutex;
    std::condition_variable stopped;
    std::vector<Device> devices;
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

void stop_locked(State& s)
{
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
    return !s.exiting && capable_locked(s);
}

void vibrate(int64_t milliseconds)
{
    // Invalid Android durations do not replace an existing valid request.
    if (milliseconds <= 0) return;
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.exiting || s.suspended || !capable_locked(s)) return;
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
        if (s.dirty || s.playing) clear_locked(s);
        return;
    }
    if (!s.dirty) return;
    s.dirty = false;
    // Round up so a short valid request does not turn into SDL's zero-duration
    // stop command. A separate deadline also stops it if SDL's timer is late.
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        s.until - now).count();
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
    for (auto& d : s.devices) SDL_JoystickClose(d.joystick);
    s.devices.clear();
}

void prepare_exit()
{
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    s.exiting = true;
    s.until = {};
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
