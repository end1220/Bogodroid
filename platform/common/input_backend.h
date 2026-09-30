#pragma once

#include "android.h" // Your main android shim header
#include <SDL2/SDL.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define INPUT_ID_KEYBOARD 1
#define INPUT_ID_XBOX 2
#define INPUT_ID_MOUSE 3

// Oddmar's IL2CPP input shim uses these edge-triggered pulses for the two
// menu entry points.  The gate is opt-in and inert for every other port.
namespace bd_oddmar_input_gate {
void configure(bool enabled);
void note_controller_button(int button, bool down);
void note_controller_axis(int axis, int value);
void begin_frame();
uint32_t controller_buttons();
bool controller_active();
bool jump_pressed();
bool attack_pressed();
bool attack2_pressed();
uint64_t frame_press_serial();
bool allow_pause_transition(bool playing);
bool ui_command_available();
bool raw_button_state(int index);
bool consume_menu_button();
bool consume_menu_back();
}

class InputBackend {
private:
    std::unordered_map<int, std::shared_ptr<jnivm::android::view::InputDevice>> devices;
    std::mutex deviceMutex;
    bool running = false;

    // Callbacks to dispatch events to the Android shim layer
    std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)> onKey;
    std::function<void(std::shared_ptr<jnivm::android::view::MotionEvent>)> onMotion;

    std::unordered_map<int, float> mControllerAxisState;
    std::unordered_map<int, float> mControllerRawAxisState;
    float mLeftStickRange = 1.0f;
    float mRightStickRange = 1.0f;
    bool mClampStickVector = false;

    InputBackend(); // Private constructor for singleton
    void dispatchControllerAxisMotion(int sdlAxis, Sint16 rawValue);

public:
    // Singleton access
    static InputBackend& instance();

    ~InputBackend();

    // Prevent copying
    InputBackend(const InputBackend&) = delete;
    void operator=(const InputBackend&) = delete;

    // ---- Device Management ----
    std::shared_ptr<jnivm::android::view::InputDevice> addDevice(int id, const std::string& name, int vendor, int product, int sources);
    std::shared_ptr<jnivm::android::view::InputDevice> getDevice(int id);
    std::shared_ptr<FakeJni::JArray<FakeJni::JInt>> getDeviceIds();

    // ---- Event Loop ----
    void runEventLoop();
    void stop();

    // ---- Callbacks ----
    void setKeyCallback(std::function<void(std::shared_ptr<jnivm::android::view::KeyEvent>)> cb);
    void setMotionCallback(std::function<void(std::shared_ptr<jnivm::android::view::MotionEvent>)> cb);

    // Utilities
    static int toAndroidKeycode(SDL_Scancode sdl_scancode);
    static int toAndroidKeycode(SDL_ControllerButtonEvent sdl_button);
};
