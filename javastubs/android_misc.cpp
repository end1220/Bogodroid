#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "javac.h"
#include "logging.h"
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
    return std::make_shared<jnivm::android::content::res::Resources>();
}

std::shared_ptr<jnivm::android::content::res::AssetManager> jnivm::android::app::Activity::getAssets()
{
    BD_LOG("JBRIDGE", "Activity.getAssets()");
    return std::make_shared<jnivm::android::content::res::AssetManager>();
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

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::hardware::display::DisplayManager) { FakeJni::Constructor<DisplayManager> {} },
    { FakeJni::Function<&DisplayManager::getDisplay> {}, "getDisplay", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::Activity) { FakeJni::Constructor<Activity> {} },
    { FakeJni::Function<&Activity::runOnUiThread> {}, "runOnUiThread", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getIntent> {}, "getIntent", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getRequestedOrientation> {}, "getRequestedOrientation", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::setRequestedOrientation> {}, "setRequestedOrientation", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getResources> {}, "getResources", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getAssets> {}, "getAssets", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getWindow> {}, "getWindow", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::getWindowManager> {}, "getWindowManager", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Activity::findViewById> {}, "findViewById", FakeJni::JMethodID::PUBLIC },
    // Also expose Context methods on Activity — Unity often GetMethodID's against
    // the concrete Activity class, and parent-walk + stub insert can leave the
    // Activity-local copy without a nativehandle.
    { FakeJni::Function<&Context::getPackageManager> {}, "getPackageManager", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getContentResolver> {}, "getContentResolver", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getObbDir> {}, "getObbDir", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Context::getObbDirs> {}, "getObbDirs", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    std::shared_ptr<FakeJni::JString> jnivm::android::app::Notification::getGroup()
    {
        return nullptr;
    }

    std::shared_ptr<FakeJni::JString> jnivm::android::app::Notification::getSortKey()
    {
        return nullptr;
    }

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::Notification) { FakeJni::Constructor<Notification> {} },
    { FakeJni::Field<&Notification::EXTRA_TITLE> {}, "EXTRA_TITLE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::EXTRA_TEXT> {}, "EXTRA_TEXT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::EXTRA_SHOW_CHRONOMETER> {}, "EXTRA_SHOW_CHRONOMETER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::EXTRA_BIG_TEXT> {}, "EXTRA_BIG_TEXT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::EXTRA_SHOW_WHEN> {}, "EXTRA_SHOW_WHEN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::FLAG_AUTO_CANCEL> {}, "FLAG_AUTO_CANCEL", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::FLAG_GROUP_SUMMARY> {}, "FLAG_GROUP_SUMMARY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&Notification::extras> {}, "extras", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Notification::flags> {}, "flags", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Notification::number> {}, "number", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Field<&Notification::when> {}, "when", FakeJni::JFieldID::PUBLIC },
    { FakeJni::Function<&Notification::getGroup> {}, "getGroup", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Notification::getSortKey> {}, "getSortKey", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::Notification::Builder) { FakeJni::Constructor<Builder> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::NativeActivity) { FakeJni::Constructor<NativeActivity> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::provider::Settings) { FakeJni::Constructor<Settings> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::provider::Settings::Secure) { FakeJni::Constructor<Secure> {} },
    { FakeJni::Field<&Secure::ANDROID_ID> {}, "ANDROID_ID", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&Secure::getString> {}, "getString", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR
