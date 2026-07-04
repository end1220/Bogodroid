// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#include "input_backend.h"
#include "android.h"
#include "logging.h"
#include <map>
#include <string>
#include <cctype>
#include <unistd.h>

#include "toml++/toml.hpp"
extern toml::table config;

extern "C" void bd_flush_prefs_impl();

static bool input_enable_controller = false;
static bool input_mouse_touch_mode = false;
static bool input_mouse_accurate_mode = false;
static int buttonState;

// wsm.toml [input.remap] — SDL controller button → Android KEYCODE.
static std::map<int, int> g_button_remap;

// Some handhelds route D-pad / Select / Start through the keyboard scancode
// path instead of the joystick path; mirror the same remap there.
static std::map<int, int> g_scancode_remap;

// Physical D-pad direction -> logical D-pad direction. This is for rotated
// handheld layouts only; it never maps analog sticks to D-pad.
static std::map<int, int> g_dpad_button_remap;
static std::map<int, int> g_dpad_scancode_remap;

enum BD_AxisDir : int {
    BD_LSTICK_LEFT = 0, BD_LSTICK_RIGHT, BD_LSTICK_UP, BD_LSTICK_DOWN,
    BD_RSTICK_LEFT,     BD_RSTICK_RIGHT, BD_RSTICK_UP, BD_RSTICK_DOWN,
    BD_L2,              BD_R2,
    BD_AXIS_DIR_COUNT
};
static std::map<int, int> g_axis_remap;
static bool g_axis_pressed[BD_AXIS_DIR_COUNT] = { false };
// Hysteresis prevents auto-repeat near the threshold.
static constexpr float BD_AXIS_PRESS_THR   = 0.60f;
static constexpr float BD_AXIS_RELEASE_THR = 0.30f;

// false → no HAT axis; D-pad delivered only as KEYCODE_DPAD_*. Sidesteps
// Unity InputSystem's NavigationModel sticky-latch race on quick taps.
// true  → AOSP-standard (HAT + KEYCODE), but exposes the race.
static bool input_dpad_synthesize_hat = true;
static bool input_start_select_exit = true;
static bool g_exit_hotkey_start_down = false;
static bool g_exit_hotkey_select_down = false;

static void bd_exit_hotkey_update(bool is_start, bool is_select, bool down, const char* source)
{
    if (!input_start_select_exit || (!is_start && !is_select)) return;
    if (is_start) g_exit_hotkey_start_down = down;
    if (is_select) g_exit_hotkey_select_down = down;
    if (g_exit_hotkey_start_down && g_exit_hotkey_select_down) {
        BD_LOG("EXIT", "Start+Select exit hotkey (%s)", source ? source : "input");
        bd_flush_prefs_impl();
        _exit(0);
    }
}

// (deviceId, keyCode) -> last ACTION_DOWN eventTime, so getDownTime() reads
// the press start (Android contract: hold time = eventTime - downTime).
static std::map<std::pair<int,int>, long> g_key_down_time;
static long bd_stamp_keyevent_downtime(int deviceId, int keyCode, int action, long eventTime)
{
    auto key = std::make_pair(deviceId, keyCode);
    if (action == jnivm::android::view::KeyEvent::ACTION_DOWN) {
        g_key_down_time[key] = eventTime;
        return eventTime;
    }
    auto it = g_key_down_time.find(key);
    return (it != g_key_down_time.end()) ? it->second : eventTime;
}

static void bd_axis_synth(BD_AxisDir dir, float value, int sign,
                          const std::shared_ptr<jnivm::android::view::InputDevice>& dev,
                          std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)>& onKey);
static void bd_axis_synth_pair(BD_AxisDir negativeDir, BD_AxisDir positiveDir, float value,
                               const std::shared_ptr<jnivm::android::view::InputDevice>& dev,
                               std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)>& onKey);
static int bd_remap_dpad_button(int button);

// "E" / "TAB" / "DPAD_UP" / "KEYCODE_M" -> KeyEvent::KEYCODE_*; -1 if unknown.
static int parse_keycode_name(std::string s)
{
    if (s.empty()) return -1;
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    if (s.rfind("KEYCODE_", 0) == 0) s = s.substr(8);

    using K = jnivm::android::view::KeyEvent;

    if (s.size() == 1 && s[0] >= 'A' && s[0] <= 'Z')
        return K::KEYCODE_A + (s[0] - 'A');
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '9')
        return K::KEYCODE_0 + (s[0] - '0');

    if (s == "TAB")        return K::KEYCODE_TAB;
    if (s == "ENTER")      return K::KEYCODE_ENTER;
    if (s == "ESCAPE" || s == "ESC") return K::KEYCODE_ESCAPE;
    if (s == "SPACE")      return K::KEYCODE_SPACE;
    if (s == "DEL" || s == "BACKSPACE") return K::KEYCODE_DEL;
    if (s == "DPAD_UP")    return K::KEYCODE_DPAD_UP;
    if (s == "DPAD_DOWN")  return K::KEYCODE_DPAD_DOWN;
    if (s == "DPAD_LEFT")  return K::KEYCODE_DPAD_LEFT;
    if (s == "DPAD_RIGHT") return K::KEYCODE_DPAD_RIGHT;
    if (s == "BUTTON_A")   return K::KEYCODE_BUTTON_A;
    if (s == "BUTTON_B")   return K::KEYCODE_BUTTON_B;
    if (s == "BUTTON_X")   return K::KEYCODE_BUTTON_X;
    if (s == "BUTTON_Y")   return K::KEYCODE_BUTTON_Y;
    if (s == "BUTTON_L1")  return K::KEYCODE_BUTTON_L1;
    if (s == "BUTTON_L2")  return K::KEYCODE_BUTTON_L2;
    if (s == "BUTTON_R1")  return K::KEYCODE_BUTTON_R1;
    if (s == "BUTTON_R2")  return K::KEYCODE_BUTTON_R2;
    if (s == "BUTTON_START")  return K::KEYCODE_BUTTON_START;
    if (s == "BUTTON_SELECT") return K::KEYCODE_BUTTON_SELECT;
    if (s == "BUTTON_THUMBL") return K::KEYCODE_BUTTON_THUMBL;
    if (s == "BUTTON_THUMBR") return K::KEYCODE_BUTTON_THUMBR;
    if (s == "NONE" || s == "DISABLE" || s == "OFF") return K::KEYCODE_UNKNOWN;
    return -1;
}

static int parse_dpad_button_name(std::string s)
{
    if (s.empty()) return -1;
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    if (s.rfind("KEYCODE_", 0) == 0) s = s.substr(8);
    if (s.rfind("DPAD_", 0) == 0) s = s.substr(5);

    if (s == "UP")    return SDL_CONTROLLER_BUTTON_DPAD_UP;
    if (s == "DOWN")  return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    if (s == "LEFT")  return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    if (s == "RIGHT") return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    return -1;
}

static int keycode_for_dpad_button(int button)
{
    using K = jnivm::android::view::KeyEvent;
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    return K::KEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return K::KEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return K::KEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K::KEYCODE_DPAD_RIGHT;
    default: return K::KEYCODE_UNKNOWN;
    }
}

static bool is_dpad_button(int button)
{
    return button >= SDL_CONTROLLER_BUTTON_DPAD_UP &&
           button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
}

static int bd_remap_dpad_button(int button)
{
    if (!is_dpad_button(button)) return button;
    auto it = g_dpad_button_remap.find(button);
    return (it != g_dpad_button_remap.end()) ? it->second : button;
}

static void load_input_remap()
{
    static bool loaded = false;
    if (loaded) return;
    loaded = true;

    // Trigger default: digital-trigger handhelds (R36S etc.) expect L2/R2
    // keycodes. SDL exposes triggers as 0..32767 axes; we synthesize on cross.
    g_axis_remap[BD_L2] = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_L2;
    g_axis_remap[BD_R2] = jnivm::android::view::KeyEvent::KEYCODE_BUTTON_R2;

    auto remap = config["input"]["remap"];
    auto* tbl = remap.is_table() ? remap.as_table() : nullptr;

    auto bind = [&](const char* cfg_key, int sdl_button) {
        if (!tbl) return;
        auto v = (*tbl)[cfg_key].value<std::string>();
        if (!v) return;
        int kc = parse_keycode_name(*v);
        if (kc >= 0) {
            g_button_remap[sdl_button] = kc;
            BD_LOG("INPUT-REMAP", "%s -> %s (keycode %d)",
                    cfg_key, v->c_str(), kc);
        }
    };
    // SDL button surface; L2/R2 and stick directions are axes — bound below.
    bind("a",       SDL_CONTROLLER_BUTTON_A);
    bind("b",       SDL_CONTROLLER_BUTTON_B);
    bind("x",       SDL_CONTROLLER_BUTTON_X);
    bind("y",       SDL_CONTROLLER_BUTTON_Y);
    bind("select",  SDL_CONTROLLER_BUTTON_BACK); // SDL calls Select "BACK"
    bind("back",    SDL_CONTROLLER_BUTTON_BACK); // alias
    bind("start",   SDL_CONTROLLER_BUTTON_START);
    bind("guide",   SDL_CONTROLLER_BUTTON_GUIDE);
    bind("l1",      SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    bind("r1",      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    bind("l3",      SDL_CONTROLLER_BUTTON_LEFTSTICK);
    bind("r3",      SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    bind("dpup",    SDL_CONTROLLER_BUTTON_DPAD_UP);
    bind("dpdown",  SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bind("dpleft",  SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bind("dpright", SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

    // D-pad rotation is independent from [input.remap]. It describes the
    // physical handheld layout, while [input.remap] describes logical buttons.
    auto dpad_remap = config["input"]["dpad_remap"];
    if (dpad_remap.is_table()) {
        auto* dpad_tbl = dpad_remap.as_table();
        auto bind_dpad = [&](const char* cfg_key, int physical_button, SDL_Scancode physical_scancode) {
            auto v = (*dpad_tbl)[cfg_key].value<std::string>();
            if (!v) return;
            int logical_button = parse_dpad_button_name(*v);
            if (logical_button < 0) {
                BD_LOG("INPUT-REMAP", "ignore input.dpad_remap.%s = %s (expected up/down/left/right)",
                        cfg_key, v->c_str());
                return;
            }
            g_dpad_button_remap[physical_button] = logical_button;
            g_dpad_scancode_remap[physical_scancode] = keycode_for_dpad_button(logical_button);
            BD_LOG("INPUT-REMAP", "dpad %s -> %s", cfg_key, v->c_str());
        };
        bind_dpad("up",    SDL_CONTROLLER_BUTTON_DPAD_UP,    SDL_SCANCODE_UP);
        bind_dpad("down",  SDL_CONTROLLER_BUTTON_DPAD_DOWN,  SDL_SCANCODE_DOWN);
        bind_dpad("left",  SDL_CONTROLLER_BUTTON_DPAD_LEFT,  SDL_SCANCODE_LEFT);
        bind_dpad("right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_SCANCODE_RIGHT);
    }

    // Stick / trigger -> keycode (synthesized on threshold cross alongside MotionEvent).
    auto bind_axis = [&](const char* cfg_key, BD_AxisDir dir) {
        if (!tbl) return;
        auto v = (*tbl)[cfg_key].value<std::string>();
        if (!v) return;
        int kc = parse_keycode_name(*v);
        if (kc >= 0) {
            g_axis_remap[dir] = kc;
            BD_LOG("INPUT-REMAP", "%s -> %s (keycode %d)",
                    cfg_key, v->c_str(), kc);
        }
    };
    bind_axis("lstick_left",  BD_LSTICK_LEFT);
    bind_axis("lstick_right", BD_LSTICK_RIGHT);
    bind_axis("lstick_up",    BD_LSTICK_UP);
    bind_axis("lstick_down",  BD_LSTICK_DOWN);
    bind_axis("rstick_left",  BD_RSTICK_LEFT);
    bind_axis("rstick_right", BD_RSTICK_RIGHT);
    bind_axis("rstick_up",    BD_RSTICK_UP);
    bind_axis("rstick_down",  BD_RSTICK_DOWN);
    bind_axis("l2",           BD_L2);
    bind_axis("r2",           BD_R2);

    // Mirror remap on the scancode path (some handhelds route here, not joystick).
    auto bind_scancode = [&](const char* cfg_key, SDL_Scancode sc) {
        if (!tbl) return;
        auto v = (*tbl)[cfg_key].value<std::string>();
        if (!v) return;
        int kc = parse_keycode_name(*v);
        if (kc >= 0) {
            g_scancode_remap[sc] = kc;
            BD_LOG("INPUT-REMAP", "%s (scancode %d) -> %s (keycode %d)",
                    cfg_key, sc, v->c_str(), kc);
        }
    };
    bind_scancode("dpup",    SDL_SCANCODE_UP);
    bind_scancode("dpdown",  SDL_SCANCODE_DOWN);
    bind_scancode("dpleft",  SDL_SCANCODE_LEFT);
    bind_scancode("dpright", SDL_SCANCODE_RIGHT);
    bind_scancode("select",  SDL_SCANCODE_ESCAPE);
    bind_scancode("back",    SDL_SCANCODE_ESCAPE);
    bind_scancode("start",   SDL_SCANCODE_RETURN);
}

InputBackend& InputBackend::instance()
{
    static InputBackend backend;
    return backend;
}

InputBackend::InputBackend()
{
    input_enable_controller = config["input"]["controller"].value_or<bool>(false);
    input_mouse_touch_mode = config["input"]["touch_mode"].value_or<bool>(false);
    input_mouse_accurate_mode = config["input"]["accurate_mode"].value_or<bool>(false);
    input_dpad_synthesize_hat = config["input"]["dpad_synthesize_hat"].value_or<bool>(true);
    input_start_select_exit = config["input"]["start_select_exit"].value_or<bool>(true);
    if (!input_dpad_synthesize_hat) {
        BD_LOG("INPUT-REMAP", "dpad_synthesize_hat = false (D-pad → KeyEvent only, no HAT axis)");
    }
    load_input_remap();

    if (SDL_Init(SDL_INIT_EVENTS) < 0) {
        BD_LOG("INPUT", "SDL_Init(EVENTS) failed: %s", SDL_GetError());
        return;
    }

    // Add default devices
    addDevice(INPUT_ID_KEYBOARD, "Bogodroid Keyboard", 0x046d, 0xc316, jnivm::android::view::InputDevice::SOURCE_KEYBOARD);
    auto mouse = addDevice(INPUT_ID_MOUSE, "Bogodroid Mouse", 0x046D, 0xC077, input_mouse_touch_mode ? jnivm::android::view::InputDevice::SOURCE_TOUCHSCREEN : jnivm::android::view::InputDevice::SOURCE_MOUSE);
    mouse->addMotionRange(jnivm::android::view::MotionEvent::AXIS_X, mouse->source, 0.0f, 640.0f, 0.0f, 1.0f); // Example screen width
    mouse->addMotionRange(jnivm::android::view::MotionEvent::AXIS_Y, mouse->source, 0.0f, 480.0f, 0.0f, 1.0f); // Example screen height

    if (input_enable_controller) {
        if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
            BD_LOG("INPUT", "SDL_Init(JOYSTICK|GAMECONTROLLER) failed: %s", SDL_GetError());
            return;
        }

        // AOSP gamepad contract (AndroidGameControllerState.cs):
        //   stick L/R = AXIS_X/Y, AXIS_Z/RZ ; trigger L/R = AXIS_LTRIGGER/RTRIGGER
        //   dpad = AXIS_HAT_X/Y. Source bits intentionally omit KEYBOARD —
        //   setting it makes Unity spawn a duplicate device and split routing.
        auto xbox = addDevice(INPUT_ID_XBOX, "Xbox 360 Controller", 0x045E, 0x028E,
                              jnivm::android::view::InputDevice::SOURCE_GAMEPAD
                            | jnivm::android::view::InputDevice::SOURCE_JOYSTICK
                            | jnivm::android::view::InputDevice::SOURCE_DPAD);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_X,        xbox->source, -1.0f, 1.0f, 0.12f, 0.0f);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_Y,        xbox->source, -1.0f, 1.0f, 0.12f, 0.0f);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_Z,        xbox->source, -1.0f, 1.0f, 0.12f, 0.0f);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_RZ,       xbox->source, -1.0f, 1.0f, 0.12f, 0.0f);
        if (input_dpad_synthesize_hat) {
            // Registering HAT motionRange flips Unity to GamepadWithDpadAxes.
            xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_HAT_X, xbox->source, -1.0f, 1.0f, 0.0f, 0.0f);
            xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_HAT_Y, xbox->source, -1.0f, 1.0f, 0.0f, 0.0f);
        }
        // Triggers fan out to both LTRIGGER/RTRIGGER and BRAKE/GAS — different
        // Unity AndroidSupport layouts read different axes; cover all four.
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_LTRIGGER, xbox->source,  0.0f, 1.0f, 0.0f,  0.0f);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_RTRIGGER, xbox->source,  0.0f, 1.0f, 0.0f,  0.0f);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_BRAKE,    xbox->source,  0.0f, 1.0f, 0.0f,  0.0f);
        xbox->addMotionRange(jnivm::android::view::MotionEvent::AXIS_GAS,      xbox->source,  0.0f, 1.0f, 0.0f,  0.0f);

        // Open game controllers
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                SDL_GameControllerOpen(i);
                verbose("InputBackend", "Opened Game Controller: %s", SDL_GameControllerNameForIndex(i));
            }
        }

        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_X]        = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_Y]        = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_Z]        = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_RZ]       = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_HAT_X]    = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_HAT_Y]    = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_LTRIGGER] = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_RTRIGGER] = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_BRAKE]    = 0.0f;
        mControllerAxisState[jnivm::android::view::MotionEvent::AXIS_GAS]      = 0.0f;
    }
}

InputBackend::~InputBackend()
{
    if (input_enable_controller) {
        // Clean up controllers
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                SDL_GameController* controller = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
                if (controller) {
                    SDL_GameControllerClose(controller);
                }
            }
        }
    }
    SDL_Quit();
}

// ---- Device Management ----
std::shared_ptr<jnivm::android::view::InputDevice> InputBackend::addDevice(int id, const std::string& name, int vendor, int product, int sources)
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    auto device = std::make_shared<jnivm::android::view::InputDevice>();
    device->id = id;
    device->name = std::make_shared<FakeJni::JString>(name);
    device->vendor = vendor;
    device->product = product;
    device->source = sources;
    devices[id] = device;
    return device;
}

std::shared_ptr<jnivm::android::view::InputDevice> InputBackend::getDevice(int id)
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    auto it = devices.find(id);
    return (it != devices.end()) ? it->second : nullptr;
}

std::shared_ptr<FakeJni::JArray<FakeJni::JInt>> InputBackend::getDeviceIds()
{
    std::lock_guard<std::mutex> lock(deviceMutex);
    auto ids = std::make_shared<FakeJni::JArray<FakeJni::JInt>>(devices.size());
    int i = 0;
    for (const auto& kv : devices) {
        (*ids)[i++] = kv.first;
    }
    return ids;
}

void InputBackend::dispatchControllerAxisMotion(int sdlAxis, Sint16 rawAxisValue)
{
    if (!input_enable_controller || !onMotion) return;

    int axis  = -1;
    int axis2 = -1;
    float value = 0.0f;
    bool isTrigger = false;

    switch (sdlAxis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
        axis = jnivm::android::view::MotionEvent::AXIS_X;
        break;
    case SDL_CONTROLLER_AXIS_LEFTY:
        axis = jnivm::android::view::MotionEvent::AXIS_Y;
        break;
    case SDL_CONTROLLER_AXIS_RIGHTX:
        axis = jnivm::android::view::MotionEvent::AXIS_Z;
        break;
    case SDL_CONTROLLER_AXIS_RIGHTY:
        axis = jnivm::android::view::MotionEvent::AXIS_RZ;
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        axis  = jnivm::android::view::MotionEvent::AXIS_LTRIGGER;
        axis2 = jnivm::android::view::MotionEvent::AXIS_BRAKE;
        isTrigger = true;
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        axis  = jnivm::android::view::MotionEvent::AXIS_RTRIGGER;
        axis2 = jnivm::android::view::MotionEvent::AXIS_GAS;
        isTrigger = true;
        break;
    }

    if (axis == -1) return;

    if (isTrigger) {
        value = rawAxisValue / 32767.0f;
    } else {
        value = rawAxisValue < 0 ? rawAxisValue / 32768.0f : rawAxisValue / 32767.0f;
    }
    float rawValue = value;

    // MotionEvent should carry the normalized physical axis. The InputDevice
    // MotionRange advertises flat=0.12, and Unity is expected to apply its own
    // stick deadzone. The filtered value below is only for optional axis->KeyEvent
    // synthesis.
    if (!isTrigger) {
        constexpr float kFlat = 0.12f;
        if (value > -kFlat && value < kFlat) {
            value = 0.0f;
        } else if (value > 0.0f) {
            value = (value - kFlat) / (1.0f - kFlat);
        } else {
            value = (value + kFlat) / (1.0f - kFlat);
        }
    }
    float synthValue = value;
    float motionValue = rawValue;

    if (axis2 != -1) {
        mControllerAxisState[axis2] = motionValue;
    }

    mControllerAxisState[axis] = motionValue;

    auto dev = devices[INPUT_ID_XBOX];
    auto motionEvent = std::make_shared<jnivm::android::view::MotionEvent>(
        dev, jnivm::android::view::MotionEvent::ACTION_MOVE, 0.0f, 0.0f);
    motionEvent->axisValues = mControllerAxisState;
    onMotion(motionEvent);

    auto keyDev = devices[INPUT_ID_XBOX];
    switch (sdlAxis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
        bd_axis_synth_pair(BD_LSTICK_LEFT, BD_LSTICK_RIGHT, synthValue, keyDev, onKey);
        break;
    case SDL_CONTROLLER_AXIS_LEFTY:
        bd_axis_synth_pair(BD_LSTICK_UP, BD_LSTICK_DOWN, synthValue, keyDev, onKey);
        break;
    case SDL_CONTROLLER_AXIS_RIGHTX:
        bd_axis_synth_pair(BD_RSTICK_LEFT, BD_RSTICK_RIGHT, synthValue, keyDev, onKey);
        break;
    case SDL_CONTROLLER_AXIS_RIGHTY:
        bd_axis_synth_pair(BD_RSTICK_UP, BD_RSTICK_DOWN, synthValue, keyDev, onKey);
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        bd_axis_synth(BD_L2, synthValue, +1, keyDev, onKey);
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        bd_axis_synth(BD_R2, synthValue, +1, keyDev, onKey);
        break;
    }
}

// ---- Event Loop ----
void InputBackend::runEventLoop()
{
    running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                if (!onKey)
                    break;
                int action = (e.type == SDL_KEYDOWN) ? jnivm::android::view::KeyEvent::ACTION_DOWN : jnivm::android::view::KeyEvent::ACTION_UP;
                int keyCode = InputBackend::toAndroidKeycode(e.key.keysym.scancode);
                bd_exit_hotkey_update(e.key.keysym.scancode == SDL_SCANCODE_RETURN,
                                      e.key.keysym.scancode == SDL_SCANCODE_ESCAPE,
                                      action == jnivm::android::view::KeyEvent::ACTION_DOWN,
                                      "keyboard");
                if (e.type == SDL_KEYDOWN) {
                    BD_DEBUG("INPUT", "KEYDOWN scancode=%d -> KEYCODE=%d",
                            e.key.keysym.scancode, keyCode);
                }
                auto keyEvent = std::make_shared<jnivm::android::view::KeyEvent>(devices[INPUT_ID_KEYBOARD], action, keyCode, 0);
                keyEvent->downTime = bd_stamp_keyevent_downtime(
                    INPUT_ID_KEYBOARD, keyCode, action, keyEvent->timestamp);
                onKey(keyEvent);
                break;
            }

            case SDL_MOUSEMOTION: {
                if (!onMotion)
                    break;

                // In touch screen emulation move events are only generated when a button is pressed.
                if (input_mouse_touch_mode && !buttonState)
                    return;

                // Mouse motion is a generic motion event.
                auto motionEvent = std::make_shared<jnivm::android::view::MotionEvent>(devices[INPUT_ID_MOUSE],
                    buttonState == 0 ? jnivm::android::view::MotionEvent::ACTION_HOVER_MOVE : jnivm::android::view::MotionEvent::ACTION_MOVE,
                    (float)e.motion.x, (float)e.motion.y);
                motionEvent->buttonState = buttonState;
                onMotion(motionEvent);
                break;
            }

            case SDL_MOUSEBUTTONDOWN: {
                if (!onMotion)
                    break;

                int previousButtonState = buttonState; // Store previous state for comparison

                // Update button state based on which button was pressed
                if (e.button.button == SDL_BUTTON_LEFT)
                    buttonState |= jnivm::android::view::MotionEvent::BUTTON_PRIMARY;
                if (e.button.button == SDL_BUTTON_RIGHT)
                    buttonState |= jnivm::android::view::MotionEvent::BUTTON_SECONDARY;
                if (e.button.button == SDL_BUTTON_MIDDLE)
                    buttonState |= jnivm::android::view::MotionEvent::BUTTON_TERTIARY;

                // Create the main motion event
                auto motionEvent = std::make_shared<jnivm::android::view::MotionEvent>(
                    devices[INPUT_ID_MOUSE],
                    jnivm::android::view::MotionEvent::ACTION_DOWN,
                    (float)e.button.x, (float)e.button.y);

                if (input_mouse_touch_mode) {
                    if(previousButtonState != 0)
                        break; // Don't send another event if another button is already pressed down
                    motionEvent->buttonState = jnivm::android::view::MotionEvent::BUTTON_PRIMARY;
                } else
                    motionEvent->buttonState = buttonState;

                // In non-touch mode, send an additional button press event
                if (!input_mouse_touch_mode && input_mouse_accurate_mode) {
                    auto motionEvent2 = std::make_shared<jnivm::android::view::MotionEvent>(
                        devices[INPUT_ID_MOUSE],
                        jnivm::android::view::MotionEvent::ACTION_BUTTON_PRESS,
                        (float)e.button.x, (float)e.button.y);
                    motionEvent2->buttonState = buttonState;
                    onMotion(motionEvent2);

                    if(previousButtonState == 0) // Additional HOVER_EXIT event is sent when the first button is pressed
                    {
                        auto motionEvent3 = std::make_shared<jnivm::android::view::MotionEvent>(
                            devices[INPUT_ID_MOUSE],
                            jnivm::android::view::MotionEvent::ACTION_HOVER_EXIT,
                            (float)e.button.x, (float)e.button.y);
                            motionEvent3->buttonState = buttonState;
                            onMotion(motionEvent3);
                    }
                }

                onMotion(motionEvent);
                break;
            }

            case SDL_MOUSEBUTTONUP: {
                if (!onMotion)
                    break;

                // Update button state based on which button was released
                if (e.button.button == SDL_BUTTON_LEFT)
                    buttonState &= ~jnivm::android::view::MotionEvent::BUTTON_PRIMARY;
                if (e.button.button == SDL_BUTTON_RIGHT)
                    buttonState &= ~jnivm::android::view::MotionEvent::BUTTON_SECONDARY;
                if (e.button.button == SDL_BUTTON_MIDDLE)
                    buttonState &= ~jnivm::android::view::MotionEvent::BUTTON_TERTIARY;

                // Create the main motion event
                auto motionEvent = std::make_shared<jnivm::android::view::MotionEvent>(
                    devices[INPUT_ID_MOUSE],
                    jnivm::android::view::MotionEvent::ACTION_UP,
                    (float)e.button.x, (float)e.button.y);

                if (input_mouse_touch_mode)
                {
                    if(buttonState != 0)
                        break; // Don't send another event if another button is still pressed down
                    motionEvent->buttonState = 0;
                }
                else
                    motionEvent->buttonState = buttonState;

                // In non-touch mode, send an additional button release event
                if (!input_mouse_touch_mode && input_mouse_accurate_mode) {
                    auto motionEvent2 = std::make_shared<jnivm::android::view::MotionEvent>(
                        devices[INPUT_ID_MOUSE],
                        jnivm::android::view::MotionEvent::ACTION_BUTTON_RELEASE,
                        (float)e.button.x, (float)e.button.y);
                    motionEvent2->buttonState = buttonState;
                    onMotion(motionEvent2);

                    if(buttonState == 0) // Additional HOVER_ENTER event is sent when the last button is released
                    {
                        auto motionEvent3 = std::make_shared<jnivm::android::view::MotionEvent>(
                            devices[INPUT_ID_MOUSE],
                            jnivm::android::view::MotionEvent::ACTION_HOVER_ENTER,
                            (float)e.button.x, (float)e.button.y);
                            motionEvent3->buttonState = buttonState;
                            onMotion(motionEvent3);
                    }
                }

                onMotion(motionEvent);
                break;
            }

            case SDL_CONTROLLERAXISMOTION: {
                dispatchControllerAxisMotion(e.caxis.axis, e.caxis.value);
                break;
            }

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                if (!input_enable_controller)
                    break;
                if (!onKey)
                    break;
                int action  = (e.type == SDL_CONTROLLERBUTTONDOWN)
                              ? jnivm::android::view::KeyEvent::ACTION_DOWN
                              : jnivm::android::view::KeyEvent::ACTION_UP;
                uint8_t physicalButton = e.cbutton.button;
                int logicalButton = bd_remap_dpad_button(physicalButton);
                int keyCode = toAndroidKeycode(e.cbutton);
                bd_exit_hotkey_update(physicalButton == SDL_CONTROLLER_BUTTON_START,
                                      physicalButton == SDL_CONTROLLER_BUTTON_BACK,
                                      action == jnivm::android::view::KeyEvent::ACTION_DOWN,
                                      "controller");
                bool is_dpad = is_dpad_button(physicalButton);
                if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                    BD_DEBUG("INPUT", "CONTROLLER button=%d logical=%d -> KEYCODE=%d",
                            physicalButton, logicalButton, keyCode);
                }
                auto keyEvent = std::make_shared<jnivm::android::view::KeyEvent>(
                    devices[INPUT_ID_XBOX], action, keyCode, 0);
                keyEvent->downTime = bd_stamp_keyevent_downtime(
                    INPUT_ID_XBOX, keyCode, action, keyEvent->timestamp);

                // Mirror dpad on the HAT axis (AOSP fans out KeyEvent+MotionEvent).
                // Release uses +0.0f to avoid -0.0f sign surprises downstream.
                // Skipped when dpad_synthesize_hat = false (Unity sticky-latch race).
                std::shared_ptr<jnivm::android::view::MotionEvent> motionEvent;
                if (onMotion && is_dpad && input_dpad_synthesize_hat) {
                    bool down = (action == jnivm::android::view::KeyEvent::ACTION_DOWN);
                    int   axis = -1;
                    float signedV = 0.0f;
                    switch (logicalButton) {
                        case SDL_CONTROLLER_BUTTON_DPAD_UP:
                            axis = jnivm::android::view::MotionEvent::AXIS_HAT_Y;
                            signedV = down ? -1.0f : 0.0f; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                            axis = jnivm::android::view::MotionEvent::AXIS_HAT_Y;
                            signedV = down ? +1.0f : 0.0f; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                            axis = jnivm::android::view::MotionEvent::AXIS_HAT_X;
                            signedV = down ? -1.0f : 0.0f; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                            axis = jnivm::android::view::MotionEvent::AXIS_HAT_X;
                            signedV = down ? +1.0f : 0.0f; break;
                    }
                    if (axis >= 0) {
                        mControllerAxisState[axis] = signedV;
                        auto dev = devices[INPUT_ID_XBOX];
                        motionEvent = std::make_shared<jnivm::android::view::MotionEvent>(
                            dev, jnivm::android::view::MotionEvent::ACTION_MOVE, 0.0f, 0.0f);
                        motionEvent->axisValues = mControllerAxisState;
                    }
                }

                onKey(keyEvent);
                if (motionEvent) onMotion(motionEvent);
                break;
            }

            default:
                break;
            }
        }
        SDL_Delay(4); // Be a good citizen
    }
}

// Edge-trigger a synthesized KeyEvent for one axis-direction slot.
// value in [-1,1] (sticks) or [0,1] (triggers); sign = ±1 selects the slot.
static void bd_axis_synth(BD_AxisDir dir, float value, int sign,
                          const std::shared_ptr<jnivm::android::view::InputDevice>& dev,
                          std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)>& onKey)
{
    auto it = g_axis_remap.find(dir);
    if (it == g_axis_remap.end()) return;        // not bound -> ignore
    if (!onKey) return;
    int keycode = it->second;
    float v = value * (float)sign;               // make "press direction" positive
    bool& pressed = g_axis_pressed[dir];
    if (!pressed && v >= BD_AXIS_PRESS_THR) {
        pressed = true;
        auto ev = std::make_shared<jnivm::android::view::KeyEvent>(
            dev, jnivm::android::view::KeyEvent::ACTION_DOWN, keycode, 0);
        ev->downTime = bd_stamp_keyevent_downtime(
            dev->id, keycode, jnivm::android::view::KeyEvent::ACTION_DOWN, ev->timestamp);
        onKey(ev);
    } else if (pressed && v <= BD_AXIS_RELEASE_THR) {
        pressed = false;
        auto ev = std::make_shared<jnivm::android::view::KeyEvent>(
            dev, jnivm::android::view::KeyEvent::ACTION_UP, keycode, 0);
        ev->downTime = bd_stamp_keyevent_downtime(
            dev->id, keycode, jnivm::android::view::KeyEvent::ACTION_UP, ev->timestamp);
        onKey(ev);
    }
}

static void bd_axis_synth_pair(BD_AxisDir negativeDir, BD_AxisDir positiveDir, float value,
                               const std::shared_ptr<jnivm::android::view::InputDevice>& dev,
                               std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)>& onKey)
{
    if (value < 0.0f) {
        bd_axis_synth(positiveDir, value, +1, dev, onKey);
        bd_axis_synth(negativeDir, value, -1, dev, onKey);
    } else if (value > 0.0f) {
        bd_axis_synth(negativeDir, value, -1, dev, onKey);
        bd_axis_synth(positiveDir, value, +1, dev, onKey);
    } else {
        bd_axis_synth(negativeDir, value, -1, dev, onKey);
        bd_axis_synth(positiveDir, value, +1, dev, onKey);
    }
}

int InputBackend::toAndroidKeycode(SDL_ControllerButtonEvent sdl_button)
{
    int button = bd_remap_dpad_button(sdl_button.button);
    auto it = g_button_remap.find(button);
    if (it != g_button_remap.end()) {
        return it->second;
    }
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_B:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_X:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_Y:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_BACK:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_GUIDE:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_MODE;
    case SDL_CONTROLLER_BUTTON_START:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_THUMBL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_THUMBR;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return jnivm::android::view::KeyEvent::KEYCODE_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_RIGHT;
    default:
        return jnivm::android::view::KeyEvent::KEYCODE_UNKNOWN;
    }
}

int InputBackend::toAndroidKeycode(SDL_Scancode sdl_scancode)
{
    auto dpad_it = g_dpad_scancode_remap.find(sdl_scancode);
    if (dpad_it != g_dpad_scancode_remap.end()) {
        return dpad_it->second;
    }
    auto it = g_scancode_remap.find(sdl_scancode);
    if (it != g_scancode_remap.end()) {
        return it->second;
    }
    switch (sdl_scancode) {
    case SDL_SCANCODE_0:
        return jnivm::android::view::KeyEvent::KEYCODE_0;
    case SDL_SCANCODE_1:
        return jnivm::android::view::KeyEvent::KEYCODE_1;
    case SDL_SCANCODE_2:
        return jnivm::android::view::KeyEvent::KEYCODE_2;
    case SDL_SCANCODE_3:
        return jnivm::android::view::KeyEvent::KEYCODE_3;
    case SDL_SCANCODE_4:
        return jnivm::android::view::KeyEvent::KEYCODE_4;
    case SDL_SCANCODE_5:
        return jnivm::android::view::KeyEvent::KEYCODE_5;
    case SDL_SCANCODE_6:
        return jnivm::android::view::KeyEvent::KEYCODE_6;
    case SDL_SCANCODE_7:
        return jnivm::android::view::KeyEvent::KEYCODE_7;
    case SDL_SCANCODE_8:
        return jnivm::android::view::KeyEvent::KEYCODE_8;
    case SDL_SCANCODE_9:
        return jnivm::android::view::KeyEvent::KEYCODE_9;
    case SDL_SCANCODE_A:
        return jnivm::android::view::KeyEvent::KEYCODE_A;
    case SDL_SCANCODE_APOSTROPHE:
        return jnivm::android::view::KeyEvent::KEYCODE_APOSTROPHE;
    case SDL_SCANCODE_B:
        return jnivm::android::view::KeyEvent::KEYCODE_B;
    case SDL_SCANCODE_BACKSLASH:
        return jnivm::android::view::KeyEvent::KEYCODE_BACKSLASH;
    case SDL_SCANCODE_BACKSPACE:
        return jnivm::android::view::KeyEvent::KEYCODE_DEL;
    case SDL_SCANCODE_C:
        return jnivm::android::view::KeyEvent::KEYCODE_C;
    case SDL_SCANCODE_CAPSLOCK:
        return jnivm::android::view::KeyEvent::KEYCODE_CAPS_LOCK;
    case SDL_SCANCODE_CLEAR:
        return jnivm::android::view::KeyEvent::KEYCODE_CLEAR;
    case SDL_SCANCODE_COMMA:
        return jnivm::android::view::KeyEvent::KEYCODE_COMMA;
    case SDL_SCANCODE_D:
        return jnivm::android::view::KeyEvent::KEYCODE_D;
    case SDL_SCANCODE_DELETE:
        return jnivm::android::view::KeyEvent::KEYCODE_FORWARD_DEL;
    case SDL_SCANCODE_DOWN:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_DOWN;
    case SDL_SCANCODE_E:
        return jnivm::android::view::KeyEvent::KEYCODE_E;
    case SDL_SCANCODE_END:
        return jnivm::android::view::KeyEvent::KEYCODE_MOVE_END;
    case SDL_SCANCODE_EQUALS:
        return jnivm::android::view::KeyEvent::KEYCODE_EQUALS;
    case SDL_SCANCODE_ESCAPE:
        return jnivm::android::view::KeyEvent::KEYCODE_ESCAPE;
    case SDL_SCANCODE_F:
        return jnivm::android::view::KeyEvent::KEYCODE_F;
    case SDL_SCANCODE_F1:
        return jnivm::android::view::KeyEvent::KEYCODE_F1;
    case SDL_SCANCODE_F2:
        return jnivm::android::view::KeyEvent::KEYCODE_F2;
    case SDL_SCANCODE_F3:
        return jnivm::android::view::KeyEvent::KEYCODE_F3;
    case SDL_SCANCODE_F4:
        return jnivm::android::view::KeyEvent::KEYCODE_F4;
    case SDL_SCANCODE_F5:
        return jnivm::android::view::KeyEvent::KEYCODE_F5;
    case SDL_SCANCODE_F6:
        return jnivm::android::view::KeyEvent::KEYCODE_F6;
    case SDL_SCANCODE_F7:
        return jnivm::android::view::KeyEvent::KEYCODE_F7;
    case SDL_SCANCODE_F8:
        return jnivm::android::view::KeyEvent::KEYCODE_F8;
    case SDL_SCANCODE_F9:
        return jnivm::android::view::KeyEvent::KEYCODE_F9;
    case SDL_SCANCODE_F10:
        return jnivm::android::view::KeyEvent::KEYCODE_F10;
    case SDL_SCANCODE_F11:
        return jnivm::android::view::KeyEvent::KEYCODE_F11;
    case SDL_SCANCODE_F12:
        return jnivm::android::view::KeyEvent::KEYCODE_F12;
    case SDL_SCANCODE_G:
        return jnivm::android::view::KeyEvent::KEYCODE_G;
    case SDL_SCANCODE_GRAVE:
        return jnivm::android::view::KeyEvent::KEYCODE_GRAVE;
    case SDL_SCANCODE_H:
        return jnivm::android::view::KeyEvent::KEYCODE_H;
    case SDL_SCANCODE_HOME:
        return jnivm::android::view::KeyEvent::KEYCODE_MOVE_HOME;
    case SDL_SCANCODE_I:
        return jnivm::android::view::KeyEvent::KEYCODE_I;
    case SDL_SCANCODE_INSERT:
        return jnivm::android::view::KeyEvent::KEYCODE_INSERT;
    case SDL_SCANCODE_J:
        return jnivm::android::view::KeyEvent::KEYCODE_J;
    case SDL_SCANCODE_K:
        return jnivm::android::view::KeyEvent::KEYCODE_K;
    case SDL_SCANCODE_KP_0:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_0;
    case SDL_SCANCODE_KP_1:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_1;
    case SDL_SCANCODE_KP_2:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_2;
    case SDL_SCANCODE_KP_3:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_3;
    case SDL_SCANCODE_KP_4:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_4;
    case SDL_SCANCODE_KP_5:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_5;
    case SDL_SCANCODE_KP_6:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_6;
    case SDL_SCANCODE_KP_7:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_7;
    case SDL_SCANCODE_KP_8:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_8;
    case SDL_SCANCODE_KP_9:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_9;
    case SDL_SCANCODE_KP_DIVIDE:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_DIVIDE;
    case SDL_SCANCODE_KP_ENTER:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_ENTER;
    case SDL_SCANCODE_KP_MINUS:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_SUBTRACT;
    case SDL_SCANCODE_KP_MULTIPLY:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_MULTIPLY;
    case SDL_SCANCODE_KP_PERIOD:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_DOT;
    case SDL_SCANCODE_KP_PLUS:
        return jnivm::android::view::KeyEvent::KEYCODE_NUMPAD_ADD;
    case SDL_SCANCODE_L:
        return jnivm::android::view::KeyEvent::KEYCODE_L;
    case SDL_SCANCODE_LALT:
        return jnivm::android::view::KeyEvent::KEYCODE_ALT_LEFT;
    case SDL_SCANCODE_LCTRL:
        return jnivm::android::view::KeyEvent::KEYCODE_CTRL_LEFT;
    case SDL_SCANCODE_LEFT:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_LEFT;
    case SDL_SCANCODE_LEFTBRACKET:
        return jnivm::android::view::KeyEvent::KEYCODE_LEFT_BRACKET;
    case SDL_SCANCODE_LSHIFT:
        return jnivm::android::view::KeyEvent::KEYCODE_SHIFT_LEFT;
    case SDL_SCANCODE_M:
        return jnivm::android::view::KeyEvent::KEYCODE_M;
    case SDL_SCANCODE_MENU:
        return jnivm::android::view::KeyEvent::KEYCODE_MENU;
    case SDL_SCANCODE_MINUS:
        return jnivm::android::view::KeyEvent::KEYCODE_MINUS;
    case SDL_SCANCODE_MUTE:
        return jnivm::android::view::KeyEvent::KEYCODE_MUTE;
    case SDL_SCANCODE_N:
        return jnivm::android::view::KeyEvent::KEYCODE_N;
    case SDL_SCANCODE_NUMLOCKCLEAR:
        return jnivm::android::view::KeyEvent::KEYCODE_NUM_LOCK;
    case SDL_SCANCODE_O:
        return jnivm::android::view::KeyEvent::KEYCODE_O;
    case SDL_SCANCODE_P:
        return jnivm::android::view::KeyEvent::KEYCODE_P;
    case SDL_SCANCODE_PAGEDOWN:
        return jnivm::android::view::KeyEvent::KEYCODE_PAGE_DOWN;
    case SDL_SCANCODE_PAGEUP:
        return jnivm::android::view::KeyEvent::KEYCODE_PAGE_UP;
    case SDL_SCANCODE_PAUSE:
        return jnivm::android::view::KeyEvent::KEYCODE_BREAK;
    case SDL_SCANCODE_PERIOD:
        return jnivm::android::view::KeyEvent::KEYCODE_PERIOD;
    case SDL_SCANCODE_POWER:
        return jnivm::android::view::KeyEvent::KEYCODE_POWER;
    case SDL_SCANCODE_PRINTSCREEN:
        return jnivm::android::view::KeyEvent::KEYCODE_SYSRQ;
    case SDL_SCANCODE_Q:
        return jnivm::android::view::KeyEvent::KEYCODE_Q;
    case SDL_SCANCODE_R:
        return jnivm::android::view::KeyEvent::KEYCODE_R;
    case SDL_SCANCODE_RALT:
        return jnivm::android::view::KeyEvent::KEYCODE_ALT_RIGHT;
    case SDL_SCANCODE_RCTRL:
        return jnivm::android::view::KeyEvent::KEYCODE_CTRL_RIGHT;
    case SDL_SCANCODE_RETURN:
        return jnivm::android::view::KeyEvent::KEYCODE_ENTER;
    case SDL_SCANCODE_RIGHT:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_RIGHT;
    case SDL_SCANCODE_RIGHTBRACKET:
        return jnivm::android::view::KeyEvent::KEYCODE_RIGHT_BRACKET;
    case SDL_SCANCODE_RSHIFT:
        return jnivm::android::view::KeyEvent::KEYCODE_SHIFT_RIGHT;
    case SDL_SCANCODE_S:
        return jnivm::android::view::KeyEvent::KEYCODE_S;
    case SDL_SCANCODE_SCROLLLOCK:
        return jnivm::android::view::KeyEvent::KEYCODE_SCROLL_LOCK;
    case SDL_SCANCODE_SELECT:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_CENTER;
    case SDL_SCANCODE_SEMICOLON:
        return jnivm::android::view::KeyEvent::KEYCODE_SEMICOLON;
    case SDL_SCANCODE_SLASH:
        return jnivm::android::view::KeyEvent::KEYCODE_SLASH;
    case SDL_SCANCODE_SPACE:
        return jnivm::android::view::KeyEvent::KEYCODE_SPACE;
    case SDL_SCANCODE_T:
        return jnivm::android::view::KeyEvent::KEYCODE_T;
    case SDL_SCANCODE_TAB:
        return jnivm::android::view::KeyEvent::KEYCODE_TAB;
    case SDL_SCANCODE_U:
        return jnivm::android::view::KeyEvent::KEYCODE_U;
    case SDL_SCANCODE_UP:
        return jnivm::android::view::KeyEvent::KEYCODE_DPAD_UP;
    case SDL_SCANCODE_V:
        return jnivm::android::view::KeyEvent::KEYCODE_V;
    case SDL_SCANCODE_VOLUMEDOWN:
        return jnivm::android::view::KeyEvent::KEYCODE_VOLUME_DOWN;
    case SDL_SCANCODE_VOLUMEUP:
        return jnivm::android::view::KeyEvent::KEYCODE_VOLUME_UP;
    case SDL_SCANCODE_W:
        return jnivm::android::view::KeyEvent::KEYCODE_W;
    case SDL_SCANCODE_X:
        return jnivm::android::view::KeyEvent::KEYCODE_X;
    case SDL_SCANCODE_Y:
        return jnivm::android::view::KeyEvent::KEYCODE_Y;
    case SDL_SCANCODE_Z:
        return jnivm::android::view::KeyEvent::KEYCODE_Z;
    default:
        return jnivm::android::view::KeyEvent::KEYCODE_UNKNOWN;
    }
}

// constexpr int InputBackend::toAndroidMetaState(SDL_KeyboardEvent event)
// {
//     SDL_Keymod mods = event.keysym.mod;
// }

void InputBackend::stop()
{
    running = false;
}

// ---- Callbacks ----
void InputBackend::setKeyCallback(std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)> cb) { onKey = std::move(cb); }
void InputBackend::setMotionCallback(std::function<void(std::shared_ptr<jnivm::android::view::MotionEvent>)> cb) { onMotion = std::move(cb); }
