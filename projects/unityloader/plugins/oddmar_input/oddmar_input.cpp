#include "plugin_api.h"
#include "oddmar_input_state.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <atomic>
#include <SDL2/SDL.h>
#include "logging.h"

namespace oddmar_input {

static BogoPluginApi g_api_storage = {};
static const BogoPluginApi* g_api = nullptr;
static bool g_enabled = false;
static bool g_sdl_buttons = false;

namespace gate {
static constexpr uint32_t menu_mask =
    (1u << SDL_CONTROLLER_BUTTON_START) | (1u << SDL_CONTROLLER_BUTTON_GUIDE);
static std::mutex state_mutex;
static OddmarInputState state{menu_mask};
static std::atomic<bool> pad_active{false};
static std::atomic<uint32_t> triggers{0};
static uint32_t frame_triggers = 0;

void configure(bool value)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    state = OddmarInputState{menu_mask};
    frame_triggers = 0;
    triggers.store(0, std::memory_order_release);
    pad_active.store(false, std::memory_order_release);
    g_enabled = value;
}

void note_button(int button, bool down)
{
    if (!g_enabled || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
        return;
    pad_active.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(state_mutex);
    const bool rising = state.note_button(button, down, SDL_GetTicks());
    if (rising && (button == SDL_CONTROLLER_BUTTON_START ||
                   button == SDL_CONTROLLER_BUTTON_GUIDE)) {
        g_api->log("INPUT", "Oddmar menu pulse armed button=%d", button);
    }
}

void note_axis(int axis, int value)
{
    if (!g_enabled) return;
    const uint32_t bit = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 1u
        : axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT ? 2u : 0u;
    if (!bit) return;
    if (value > 19660) triggers.fetch_or(bit, std::memory_order_acq_rel);
    else if (value < 9830) triggers.fetch_and(~bit, std::memory_order_acq_rel);
}

void begin_frame()
{
    if (!g_enabled) return;
    std::lock_guard<std::mutex> lock(state_mutex);
    state.begin_frame(SDL_GetTicks());
    frame_triggers = triggers.load(std::memory_order_acquire);
}

uint32_t buttons()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return state.buttons();
}
bool active() { return pad_active.load(std::memory_order_acquire); }
bool pressed(int button)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return state.pressed(button);
}
uint64_t serial()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return state.frame_serial();
}
bool allow_pause(bool playing)
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return state.allow_pause_transition(playing);
}
bool ui_available()
{
    std::lock_guard<std::mutex> lock(state_mutex);
    return state.ui_command_available();
}
bool raw_button(int index)
{
    static constexpr int map[] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        -1, -1, SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK
    };
    if (index < 0 || index >= 12) return false;
    std::lock_guard<std::mutex> lock(state_mutex);
    if (index == 6 || index == 7)
        return (frame_triggers & (1u << (index - 6))) != 0;
    return (state.buttons() & (1u << map[index])) != 0;
}
bool consume_menu()
{
    if (!g_enabled) return false;
    std::lock_guard<std::mutex> lock(state_mutex);
    return state.consume_menu();
}
} // namespace gate

static uintptr_t g_pause_orig = 0;
static uintptr_t g_quit_orig = 0;
static uintptr_t g_jump_orig = 0;
static uintptr_t g_raw_orig = 0;
static uintptr_t g_attack_orig = 0;
static uintptr_t g_attack2_orig = 0;
static uintptr_t g_walk_orig = 0;
static uintptr_t g_ui_orig[5] = {};

static bool raw_button_hook(void* self, int index, void* method)
{
    using Fn = bool (*)(void*, int, void*);
    const bool native = reinterpret_cast<Fn>(g_raw_orig)(self, index, method);
    if (!self || *reinterpret_cast<const int*>(
            static_cast<const unsigned char*>(self) + 0x1a8) != 1)
        return native;
    const bool physical = gate::raw_button(index);
    static unsigned char observations[20] = {};
    static unsigned int logs = 0;
    if (index >= 0 && index < 20) {
        const unsigned char observation = 4u | (native ? 1u : 0u) |
            (physical ? 2u : 0u);
        if (observations[index] != observation && logs < 160) {
            observations[index] = observation;
            ++logs;
            g_api->log("INPUT", "Oddmar raw button index=%d native=%d SDL=%d mask=0x%x",
                       index, native, physical, gate::buttons());
        }
    }
    return physical;
}

static void pause_hook(void* self, void* method)
{
    const uint32_t state = self
        ? *reinterpret_cast<const uint32_t*>(
              reinterpret_cast<const unsigned char*>(self) + 0x13c) : 1u;
    const auto event_serial = gate::serial();
    if (!gate::allow_pause(state == 1u)) {
        static uint64_t logged_serial = UINT64_MAX;
        if (event_serial != logged_serial) {
            logged_serial = event_serial;
            g_api->log("INPUT", "Oddmar pauseToggle blocked state=%u serial=%llu",
                       state, (unsigned long long)event_serial);
        }
        return;
    }
    g_api->log("INPUT", "Oddmar pauseToggle allowed state=%u serial=%llu",
               state, (unsigned long long)event_serial);
    reinterpret_cast<void (*)(void*, void*)>(g_pause_orig)(self, method);
}

static void quit_hook(void* self, void* method)
{
    if (!gate::consume_menu()) {
        g_api->log("INPUT", "Oddmar tryQuit blocked without Start/Guide pulse");
        return;
    }
    reinterpret_cast<void (*)(void*, void*)>(g_quit_orig)(self, method);
}

static bool jump_hook(void* method)
{
    const bool game_jump = reinterpret_cast<bool (*)(void*)>(g_jump_orig)(method);
    if (!game_jump || !g_sdl_buttons || !gate::active()) return game_jump;
    const bool physical = gate::pressed(SDL_CONTROLLER_BUTTON_A);
    static unsigned int blocked_logs = 0;
    if (!physical && blocked_logs++ < 24)
        g_api->log("INPUT", "Oddmar Jump suppressed without A mask=0x%x",
                   gate::buttons());
    return physical;
}

static bool attack_hook(void* method)
{
    const bool native = reinterpret_cast<bool (*)(void*)>(g_attack_orig)(method);
    if (!gate::active()) return native;
    const bool pressed = gate::pressed(SDL_CONTROLLER_BUTTON_X);
    const auto event_serial = gate::serial();
    static uint64_t logged_serial = UINT64_MAX;
    static unsigned int logs = 0;
    if ((pressed || native) && event_serial != logged_serial && logs < 160) {
        logged_serial = event_serial;
        ++logs;
        g_api->log("INPUT", "Oddmar attack edge native=%d SDL=%d serial=%llu mask=0x%x",
                   native, pressed, (unsigned long long)event_serial, gate::buttons());
    }
    return pressed;
}

static bool attack2_hook(void* method)
{
    const bool native = reinterpret_cast<bool (*)(void*)>(g_attack2_orig)(method);
    if (!g_sdl_buttons || !gate::active()) return native;
    const bool pressed = gate::pressed(SDL_CONTROLLER_BUTTON_B);
    const auto event_serial = gate::serial();
    static uint64_t logged_serial = UINT64_MAX;
    static unsigned int logs = 0;
    if ((pressed || native) && event_serial != logged_serial && logs < 160) {
        logged_serial = event_serial;
        ++logs;
        g_api->log("INPUT", "Oddmar attack2 edge native=%d SDL=%d serial=%llu mask=0x%x",
                   native, pressed, (unsigned long long)event_serial, gate::buttons());
    }
    return pressed;
}

static int walk_hook(void* self, const unsigned char* args, void* method)
{
    const int type = args ? *reinterpret_cast<const int*>(args + 0x20) : -1;
    const int result = reinterpret_cast<int (*)(void*, const unsigned char*, void*)>(
        g_walk_orig)(self, args, method);
    static uint64_t logged_serial[9] = {};
    static unsigned int logs = 0;
    const auto event_serial = gate::serial();
    if (type >= 4 && type <= 8 && logs < 200 &&
        logged_serial[type] != event_serial) {
        logged_serial[type] = event_serial;
        const int air_jumps = self ? *reinterpret_cast<const int*>(
            static_cast<const unsigned char*>(self) + 0xac) : -1;
        g_api->log("INPUT", "Oddmar character command type=%d result=%d airJumps=%d serial=%llu",
                   type, result, air_jumps, (unsigned long long)event_serial);
        ++logs;
    }
    return result;
}

static bool ui_query(bool native, unsigned int slot)
{
    if (!native || !gate::active() || gate::ui_available()) return native;
    static uint64_t logged_serial[5] = {};
    static unsigned int logs = 0;
    const auto event_serial = gate::serial();
    if (logs < 80 && logged_serial[slot] != event_serial) {
        logged_serial[slot] = event_serial;
        ++logs;
        g_api->log("INPUT", "Oddmar UI boundary blocked query=%u serial=%llu",
                   slot, (unsigned long long)event_serial);
    }
    return false;
}

template<unsigned int Slot>
static bool static_ui_hook(void* method)
{
    return ui_query(reinterpret_cast<bool (*)(void*)>(g_ui_orig[Slot])(method), Slot);
}

template<unsigned int Slot>
static bool instance_ui_hook(void* self, void* method)
{
    return ui_query(reinterpret_cast<bool (*)(void*, void*)>(
        g_ui_orig[Slot])(self, method), Slot);
}

static bool enabled()
{
    if (!g_api) {
        return false;
    }
    const char* package = g_api->config_get_string("package.packageName", "");
    if (!package || std::strcmp(package, "com.mobge.Oddmar") != 0) {
        g_api->log("ODDMAR_INPUT", "skipped package=%s", package ? package : "(null)");
        return false;
    }
    const int patch_enabled = g_api->config_get_bool(
        "game_patches.oddmar_input.enabled",
        g_api->config_get_bool("oddmar_input.enabled", 0));
    if (!patch_enabled) {
        g_api->log("ODDMAR_INPUT", "skipped: game_patches.oddmar_input.enabled=false");
        return false;
    }
    return true;
}

static void on_button(int button, int down, void*)
{
    gate::note_button(button, down != 0);
}

static void on_axis(int axis, int value, void*)
{
    gate::note_axis(axis, value);
}

static void on_frame(void*)
{
    gate::begin_frame();
}

static int init(const BogoPluginApi* api)
{
    if (!enabled())
        return BOGO_PLUGIN_OK;
    if (!api->register_input_observer) {
        return -1;
    }
    const bool menu_gate = api->config_get_bool("input.oddmar_menu_gate", 0) != 0;
    g_sdl_buttons = api->config_get_bool("input.oddmar_sdl_buttons", 0) != 0;
    gate::configure(menu_gate);
    const BogoInputObserver observer = {
        &on_button, &on_axis, &on_frame, nullptr
    };
    if (!api->register_input_observer(&observer))
        return -1;
    if (!menu_gate)
        return BOGO_PLUGIN_OK;

    if (!api->so_base)
        return -1;
    const uintptr_t base = api->so_base(api->il2cpp);
    if (!base)
        return -1;
    const uintptr_t pause = 0x93047C;
    const uintptr_t quit = 0x8E53AC;
    api->hook_address_detour(api->il2cpp, base + pause,
                             (uintptr_t)&pause_hook, &g_pause_orig);
    api->hook_address_detour(api->il2cpp, base + quit,
                             (uintptr_t)&quit_hook, &g_quit_orig);
    const uintptr_t ui_rvas[] = {
        0x948BF8, 0x948E04, 0x97D5F0, 0xFD4C48, 0xFD4C68
    };
    const uintptr_t ui_hooks[] = {
        (uintptr_t)&static_ui_hook<0>, (uintptr_t)&static_ui_hook<1>,
        (uintptr_t)&instance_ui_hook<2>, (uintptr_t)&instance_ui_hook<3>,
        (uintptr_t)&instance_ui_hook<4>
    };
    for (unsigned int i = 0; i < 5; ++i)
        api->hook_address_detour(api->il2cpp,
            base + ui_rvas[i], ui_hooks[i], &g_ui_orig[i]);

    if (g_sdl_buttons) {
        const uintptr_t rvas[] = {
            0x17EB88C, 0x9483A8, 0x9485CC, 0x9487E8, 0x94DD00
        };
        const uintptr_t hooks[] = {
            (uintptr_t)&raw_button_hook, (uintptr_t)&jump_hook,
            (uintptr_t)&attack_hook, (uintptr_t)&attack2_hook,
            (uintptr_t)&walk_hook
        };
        uintptr_t* originals[] = {
            &g_raw_orig, &g_jump_orig, &g_attack_orig,
            &g_attack2_orig, &g_walk_orig
        };
        for (unsigned int i = 0; i < 5; ++i)
            api->hook_address_detour(api->il2cpp,
                base + rvas[i], hooks[i], originals[i]);
    }
    api->log("ODDMAR_INPUT", "installed menu_gate=%d sdl_buttons=%d",
             menu_gate ? 1 : 0, g_sdl_buttons ? 1 : 0);
    return BOGO_PLUGIN_OK;
}

} // namespace oddmar_input

extern "C" int bogodroid_plugin_init(const BogoPluginApi* api)
{
    if (!api || api->abi_version != BOGODROID_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(BogoPluginApi) || !api->log)
        return -1;
    oddmar_input::g_api_storage = *api;
    oddmar_input::g_api = &oddmar_input::g_api_storage;
    if (!api->il2cpp)
        return BOGO_PLUGIN_DEFERRED;
    return oddmar_input::init(api);
}
