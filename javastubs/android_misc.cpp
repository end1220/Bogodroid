#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "javac.h"
#include "logging.h"
#include <algorithm>
#include <fstream>
#include <inttypes.h>
#include <pthread.h>

///// Uri

std::shared_ptr<FakeJni::JString> jnivm::android::net::Uri::encode(std::shared_ptr<FakeJni::JString> string)
{
    return string;
}

std::shared_ptr<FakeJni::JString> jnivm::android::net::Uri::decode(std::shared_ptr<FakeJni::JString> string)
{
    // Real percent decoding: turns "%XX" back into bytes, "+" into space.
    // Game may URL-encode some payload before putString and decode after
    // getString — identity stub would corrupt that round-trip.
    if (!string) return std::make_shared<FakeJni::JString>("");
    const std::string& in = *string;
    std::string out;
    out.reserve(in.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (in[i] == '+') { out.push_back(' '); continue; }
        out.push_back(in[i]);
    }
    return std::make_shared<FakeJni::JString>(out.c_str());
}

///// DisplayManager

std::shared_ptr<jnivm::android::view::Display>
jnivm::android::hardware::display::DisplayManager::getDisplay(int disp)
{
    return std::make_shared<jnivm::android::view::Display>();
}

std::shared_ptr<jnivm::Array<jnivm::android::view::Display>>
jnivm::android::hardware::display::DisplayManager::getDisplays()
{
    // Exactly one built-in display, matching the single Display object the rest
    // of the shim hands out (WindowManager.getDefaultDisplay,
    // DisplayManager.getDisplay). Unity 6 walks this array to pick the panel it
    // reports, so an empty array would leave it with nothing.
    auto array = std::make_shared<jnivm::Array<jnivm::android::view::Display>>(1);
    (*array)[0] = std::make_shared<jnivm::android::view::Display>();
    return array;
}

void jnivm::android::hardware::display::DisplayManager::DisplayListener::onDisplayAdded(int displayId) { }
void jnivm::android::hardware::display::DisplayManager::DisplayListener::onDisplayChanged(int displayId) { }
void jnivm::android::hardware::display::DisplayManager::DisplayListener::onDisplayRemoved(int displayId) { }

void jnivm::android::hardware::display::DisplayManager::registerDisplayListener(
    std::shared_ptr<DisplayListener> listener, std::shared_ptr<jnivm::android::os::Handler> handler)
{
    // Held, never invoked: bd_device_display_* does not change at runtime, so
    // there is no display change to report. The listener is the Java player's
    // own class in a real APK, and the Java player is stubbed out here, so
    // firing it would re-enter native code that expects a live UnityPlayer.
    BD_LOG("JBRIDGE", "DisplayManager.registerDisplayListener(%s, %s)",
           listener ? "listener" : "null", handler ? "handler" : "null");
    if (listener)
        listeners.push_back(listener);
}

void jnivm::android::hardware::display::DisplayManager::unregisterDisplayListener(
    std::shared_ptr<DisplayListener> listener)
{
    listeners.erase(std::remove(listeners.begin(), listeners.end(), listener), listeners.end());
}

///// InputManager

std::shared_ptr<jnivm::android::view::InputDevice> jnivm::android::hardware::input::InputManager::getInputDevice(int device)
{
    return jnivm::android::view::InputDevice::getDevice(device);
}

std::shared_ptr<jnivm::Array<int>> jnivm::android::hardware::input::InputManager::getInputDeviceIds()
{
    return jnivm::android::view::InputDevice::getDeviceIds();
}

void jnivm::android::hardware::input::InputManager::registerInputDeviceListener(std::shared_ptr<InputDeviceListener> listener, std::shared_ptr<jnivm::android::os::Handler> handler)
{
}

///// Activity

void jnivm::android::app::Activity::runOnUiThread(std::shared_ptr<jnivm::java::lang::Runnable> runnable)
{
    verbose("JBRIDGE", "RunOnUiThread Running runnable!");
    runnable->run();
}

std::shared_ptr<jnivm::android::content::Intent>
jnivm::android::app::Activity::getIntent()
{
    return std::make_shared<jnivm::android::content::Intent>();
}

int jnivm::android::app::Activity::getRequestedOrientation()
{
    return config["device"]["displayOrientation"].value_or<int>(0);
}

void jnivm::android::app::Activity::setRequestedOrientation(int orientation)
{
    // Stub
}

std::shared_ptr<jnivm::android::content::res::Resources> jnivm::android::app::Activity::getResources()
{
    // Delegate to the Context implementation so an Activity and the Context it
    // came from hand back the same Resources (and therefore the same
    // Configuration/DisplayMetrics). Real Android does the same.
    return jnivm::android::content::Context::getResources();
}

std::shared_ptr<jnivm::android::view::Window> jnivm::android::app::Activity::getWindow()
{
    return std::make_shared<jnivm::android::view::Window>();
}

std::shared_ptr<jnivm::android::view::WindowManager>
jnivm::android::app::Activity::getWindowManager()
{
    BD_LOG("JBRIDGE", "Activity.getWindowManager()");
    return std::make_shared<jnivm::android::view::WindowManager>();
}

std::shared_ptr<jnivm::android::view::View> jnivm::android::app::Activity::findViewById(int id)
{
    return std::make_shared<jnivm::android::view::SurfaceView>(); // Sure, lol
}

///// Settings$Secure

std::shared_ptr<FakeJni::JString> jnivm::android::provider::Settings::Secure::getString(std::shared_ptr<jnivm::android::content::ContentResolver> resolver, std::shared_ptr<FakeJni::JString> key)
{
    if(key == nullptr)
        return nullptr;

    if(key.get()->asStdString() == ANDROID_ID)
        return std::make_shared<FakeJni::JString>("B06015BADC0DE00F");

    verbose("JBRIDGE","Secure.getString() called with unknown key: %s", key.get()->c_str());
    return nullptr;
}


///// Misc Descriptors

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::util::DisplayMetrics) { FakeJni::Constructor<DisplayMetrics> {} },
    { FakeJni::Field<&DisplayMetrics::widthPixels> {}, "widthPixels", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&DisplayMetrics::heightPixels> {}, "heightPixels", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&DisplayMetrics::densityDpi> {}, "densityDpi", FakeJni::JFieldID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::net::Uri) { FakeJni::Constructor<Uri> {} },
    { FakeJni::Function<&Uri::encode> {}, "encode", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Uri::decode> {}, "decode", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::hardware::input::InputManager) { FakeJni::Constructor<InputManager> {} },
    { FakeJni::Function<&InputManager::getInputDeviceIds> {}, "getInputDeviceIds", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputManager::getInputDevice> {}, "getInputDevice", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputManager::registerInputDeviceListener> {}, "registerInputDeviceListener", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::hardware::input::InputManager::InputDeviceListener) { FakeJni::Constructor<InputDeviceListener> {} },
    END_NATIVE_DESCRIPTOR

    // Only the TYPE_* constants: Unity 6 reads them off android/hardware/Sensor
    // after Class.forName(). No SensorManager (and therefore no sensor list) is
    // stubbed, so no Sensor instance is ever handed out.
    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::hardware::Sensor) { FakeJni::Constructor<Sensor> {} },
    { FakeJni::Field<&Sensor::TYPE_ACCELEROMETER> {}, "TYPE_ACCELEROMETER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_MAGNETIC_FIELD> {}, "TYPE_MAGNETIC_FIELD", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_ORIENTATION> {}, "TYPE_ORIENTATION", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_GYROSCOPE> {}, "TYPE_GYROSCOPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_LIGHT> {}, "TYPE_LIGHT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_PRESSURE> {}, "TYPE_PRESSURE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_TEMPERATURE> {}, "TYPE_TEMPERATURE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_PROXIMITY> {}, "TYPE_PROXIMITY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_GRAVITY> {}, "TYPE_GRAVITY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_LINEAR_ACCELERATION> {}, "TYPE_LINEAR_ACCELERATION", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_ROTATION_VECTOR> {}, "TYPE_ROTATION_VECTOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_RELATIVE_HUMIDITY> {}, "TYPE_RELATIVE_HUMIDITY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_AMBIENT_TEMPERATURE> {}, "TYPE_AMBIENT_TEMPERATURE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_MAGNETIC_FIELD_UNCALIBRATED> {}, "TYPE_MAGNETIC_FIELD_UNCALIBRATED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_GAME_ROTATION_VECTOR> {}, "TYPE_GAME_ROTATION_VECTOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_GYROSCOPE_UNCALIBRATED> {}, "TYPE_GYROSCOPE_UNCALIBRATED", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_SIGNIFICANT_MOTION> {}, "TYPE_SIGNIFICANT_MOTION", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_STEP_DETECTOR> {}, "TYPE_STEP_DETECTOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_STEP_COUNTER> {}, "TYPE_STEP_COUNTER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_GEOMAGNETIC_ROTATION_VECTOR> {}, "TYPE_GEOMAGNETIC_ROTATION_VECTOR", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_HEART_RATE> {}, "TYPE_HEART_RATE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_POSE_6DOF> {}, "TYPE_POSE_6DOF", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_STATIONARY_DETECT> {}, "TYPE_STATIONARY_DETECT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_MOTION_DETECT> {}, "TYPE_MOTION_DETECT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_HEART_BEAT> {}, "TYPE_HEART_BEAT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_LOW_LATENCY_OFFBODY_DETECT> {}, "TYPE_LOW_LATENCY_OFFBODY_DETECT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Sensor::TYPE_ACCELEROMETER_UNCALIBRATED> {}, "TYPE_ACCELEROMETER_UNCALIBRATED", FakeJni::JFieldID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::hardware::display::DisplayManager) { FakeJni::Constructor<DisplayManager> {} },
    { FakeJni::Function<&DisplayManager::getDisplay> {}, "getDisplay", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&DisplayManager::getDisplays> {}, "getDisplays", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&DisplayManager::registerDisplayListener> {}, "registerDisplayListener", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&DisplayManager::unregisterDisplayListener> {}, "unregisterDisplayListener", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::hardware::display::DisplayManager::DisplayListener) { FakeJni::Constructor<DisplayListener> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::Activity) { FakeJni::Constructor<Activity> {} },
    { FakeJni::Function<&Activity::runOnUiThread> {}, "runOnUiThread", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getIntent> {}, "getIntent", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getRequestedOrientation> {}, "getRequestedOrientation", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::setRequestedOrientation> {}, "setRequestedOrientation", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getResources> {}, "getResources", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getWindow> {}, "getWindow", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getWindowManager> {}, "getWindowManager", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::findViewById> {}, "findViewById", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::NativeActivity) { FakeJni::Constructor<NativeActivity> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::provider::Settings) { FakeJni::Constructor<Settings> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::provider::Settings::Secure) { FakeJni::Constructor<Secure> {} },
    { FakeJni::Field<&Secure::ANDROID_ID> {}, "ANDROID_ID", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&Secure::getString> {}, "getString", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR
