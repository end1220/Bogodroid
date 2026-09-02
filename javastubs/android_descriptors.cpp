#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "javac.h"
#include "logging.h"
#include <fstream>
#include <inttypes.h>
#include <pthread.h>

///// JNI Class Registration

void InitJNIAndroidClasses(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Initializing Android JNI Classes");
    // Net
    vm->registerClass<jnivm::android::net::Uri>();

    // Util
    vm->registerClass<jnivm::android::util::DisplayMetrics>();

    // View
    vm->registerClass<jnivm::android::view::DisplayMode>();
    vm->registerClass<jnivm::android::view::Display>();
    vm->registerClass<jnivm::android::view::Surface>();
    vm->registerClass<jnivm::android::view::Window>();
    vm->registerClass<jnivm::android::view::WindowManager>();
    vm->registerClass<jnivm::android::view::View>();
    vm->registerClass<jnivm::android::view::SurfaceView>();
    vm->registerClass<jnivm::android::view::Choreographer>();
    vm->registerClass<jnivm::android::view::Choreographer::FrameCallback>();
    vm->registerClass<jnivm::android::view::ContextThemeWrapper>();
    vm->registerClass<jnivm::android::view::MotionRange>();
    vm->registerClass<jnivm::android::view::InputDevice>();
    vm->registerClass<jnivm::android::view::InputEvent>();
    vm->registerClass<jnivm::android::view::KeyEvent>();
    vm->registerClass<jnivm::android::view::MotionEvent>();
    vm->registerClass<jnivm::android::view::KeyCharacterMap>();

    // Hardware
    vm->registerClass<jnivm::android::hardware::display::DisplayManager>();
    vm->registerClass<jnivm::android::hardware::input::InputManager>();
    vm->registerClass<jnivm::android::hardware::input::InputManager::InputDeviceListener>();

    // Media
    vm->registerClass<jnivm::android::media::MediaRouterRouteInfo>();
    vm->registerClass<jnivm::android::media::MediaRouter>();
    vm->registerClass<jnivm::android::media::AudioDeviceInfo>();
    vm->registerClass<jnivm::android::media::AudioManager>();
    vm->registerClass<jnivm::android::media::MediaExtractor>();
    vm->registerClass<jnivm::android::media::MediaFormat>();
    vm->registerClass<jnivm::android::media::MediaCodec>();

    // OS
    vm->registerClass<jnivm::android::os::Build>();
    vm->registerClass<jnivm::android::os::BuildVersion>();
    vm->registerClass<jnivm::android::os::Process>();
    vm->registerClass<jnivm::android::os::Bundle>();
    vm->registerClass<jnivm::android::os::Message>();
    vm->registerClass<jnivm::android::os::Looper>();
    vm->registerClass<jnivm::android::os::Handler>();
    vm->registerClass<jnivm::android::os::Handler::Callback>();
    vm->registerClass<jnivm::android::os::HandlerThread>();
    vm->registerClass<jnivm::android::os::Environment>();
    vm->registerClass<jnivm::android::os::StatFs>();
    vm->registerClass<jnivm::android::os::PowerManager>();
    vm->registerClass<jnivm::android::os::ParcelFileDescriptor>();

    // Content
    vm->registerClass<jnivm::android::content::SharedPreferences>();
    vm->registerClass<jnivm::android::content::SharedPreferencesEditor>();
    vm->registerClass<jnivm::android::content::Context>();
    vm->registerClass<jnivm::android::content::Intent>();
    vm->registerClass<jnivm::android::content::ContentResolver>();

    // Content.pm
    vm->registerClass<jnivm::android::content::pm::ActivityInfo>();
    vm->registerClass<jnivm::android::content::pm::PackageInfo>();
    vm->registerClass<jnivm::android::content::pm::ApplicationInfo>();
    vm->registerClass<jnivm::android::content::pm::PackageManager>();

    // Content.res
    vm->registerClass<jnivm::android::content::res::AssetManager>();
    vm->registerClass<jnivm::android::content::res::Resources>();

    // App
    vm->registerClass<jnivm::android::app::Activity>();
    vm->registerClass<jnivm::android::app::NativeActivity>();
    vm->registerClass<jnivm::android::app::DialogInterface>();
    vm->registerClass<jnivm::android::app::DialogInterfaceOnClickListener>();
    vm->registerClass<jnivm::android::app::DialogInterfaceOnCancelListener>();
    vm->registerClass<jnivm::android::app::AlertDialog>();
    vm->registerClass<jnivm::android::app::AlertDialogBuilder>();

    // Provider
    vm->registerClass<jnivm::android::provider::Settings>();
    vm->registerClass<jnivm::android::provider::Settings::Secure>();

    // Factory registrations — classes whose only role is "be a non-null
    // instance the guest can hold and ignore". defaultVal<jobject> parses
    // the JNI signature, looks up the JNI class name here, and builds the
    // typed instance via std::make_shared<T>(). Method calls on the
    // returned dummy hit the STUB-MISS path and fall to type-default
    // returns.
    //
    // Adding a new entry replaces a hand-written one-line C++ method like
    //   X Context::getX() { return std::make_shared<X>(); }
    // plus its descriptor entry. The class still needs to exist in
    // android.h (so make_shared<T>() compiles) and have at least an empty
    // BEGIN_NATIVE_DESCRIPTOR so jnivm can dispatch into it.
    vm->registerFactory<jnivm::android::media::MediaExtractor>("android/media/MediaExtractor");
    vm->registerFactory<jnivm::android::media::MediaFormat>("android/media/MediaFormat");
    vm->registerFactory<jnivm::android::media::MediaCodec>("android/media/MediaCodec");
    vm->registerFactory<jnivm::android::os::ParcelFileDescriptor>("android/os/ParcelFileDescriptor");
    vm->registerFactory<jnivm::java::io::FileDescriptor>("java/io/FileDescriptor");

    // Context return types — methods on Context that just hand back a
    // default-constructed instance of each. Migrated from hand-written
    // bodies in android_content.cpp.
    vm->registerFactory<jnivm::android::content::res::AssetManager>("android/content/res/AssetManager");
    vm->registerFactory<jnivm::android::content::res::Resources>("android/content/res/Resources");
    vm->registerFactory<jnivm::android::content::pm::PackageManager>("android/content/pm/PackageManager");
    vm->registerFactory<jnivm::android::content::pm::PackageInfo>("android/content/pm/PackageInfo");
    vm->registerFactory<jnivm::android::content::ContentResolver>("android/content/ContentResolver");
    vm->registerFactory<jnivm::android::view::Window>("android/view/Window");
    vm->registerFactory<jnivm::android::view::WindowManager>("android/view/WindowManager");
    vm->registerFactory<jnivm::android::view::Display>("android/view/Display");
    vm->registerFactory<jnivm::android::os::Bundle>("android/os/Bundle");
    vm->registerFactory<jnivm::java::util::Map>("java/util/Map");
}
