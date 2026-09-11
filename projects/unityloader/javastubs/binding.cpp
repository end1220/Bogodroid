#include "android.h"
#include "baron/baron.h"
#include "fakefmod.h"
#ifdef BD_ENABLE_GPLAY
#include "gplay.h"
#endif
#include "javac.h"
#include "jnibridge.h"
#include "logging.h"
#include "unity.h"

static std::shared_ptr<jnivm::android::view::WindowManager>
hook_getWindowManager(jnivm::ENV*, jnivm::Object*)
{
    BD_LOG("JBRIDGE", "hook Activity.getWindowManager()");
    return std::make_shared<jnivm::android::view::WindowManager>();
}

void InitJNIBinding(FakeJni::Jvm* vm)
{

    InitJNIJavaClasses(vm);
    InitJNIAndroidClasses(vm);
#ifdef BD_ENABLE_GPLAY
    InitJNIGooglePlayClasses(vm);
#endif

    // vm->registerClass<jnivm::java::util::NoSuchElementException>();
    // vm->registerClass<jnivm::com::unity3d::player::NativeLoader>();
    // vm->registerClass<jnivm::com::unity3d::player::UnityPlayer>();
    // vm->registerClass<jnivm::com::unity3d::player::GoogleARCoreApi>();
    // vm->registerClass<jnivm::com::unity3d::player::Camera2Wrapper>();
    // vm->registerClass<jnivm::com::unity3d::player::HFPStatus>();
    // vm->registerClass<jnivm::com::unity3d::player::AudioVolumeHandler>();
    vm->registerClass<jnivm::com::unity3d::player::IAssetPackManagerStatusQueryCallback>();
    vm->registerClass<jnivm::com::unity3d::player::IAssetPackManagerDownloadStatusCallback>();
    vm->registerClass<jnivm::com::unity3d::player::IAssetPackManagerMobileDataConfirmationCallback>();
    vm->registerClass<jnivm::com::unity3d::player::UnityCoreAssetPacksStatusCallbacks>();
    // vm->registerClass<jnivm::com::unity3d::player::OrientationLockListener>();
    // vm->registerClass<jnivm::com::google::androidgamesdk::ChoreographerCallback>();
    // vm->registerClass<jnivm::com::google::androidgamesdk::SwappyDisplayManager>();

    vm->registerClass<jnivm::com::unity3d::player::PlayAssetDeliveryUnityWrapper>();
    vm->registerClass<jnivm::com::unity3d::player::UnityPlayerActivity>();
    vm->registerClass<jnivm::com::unity3d::player::UnityPlayer>();
    vm->registerClass<jnivm::com::unity3d::player::ReflectionHelper>();
    vm->registerClass<jnivm::com::unity3d::player::ReflectionHelper::InvocationError>();
    vm->registerClass<jnivm::com::unity::androidnotifications::NotificationCallback>();
    vm->registerClass<jnivm::com::unity::androidnotifications::UnityNotificationManager>();
    vm->registerClass<jnivm::bitter::jnibridge::JNIBridge>();
    vm->registerClass<jnivm::bitter::jnibridge::JNIBridgeProxy>();

    // Fake FMOD

    vm->registerClass<jnivm::org::fmod::FMODAudioDevice>();

    // ── Trivial default returns ───────────────────────────────────────
    // Methods that always answer with a fixed value go here instead of
    // getting a hand-written C++ body + a descriptor entry. When the
    // game calls these via JNI, jnivm finds no method, takes the
    // STUB-MISS path, and consults this registry before returning the
    // generic type-default. Adding a new entry: pick the JNI class /
    // method / signature triple and the value. No code change in
    // javastubs/, no descriptor edit, no rebuild of a header.
    //
    // If the same method ever needs real logic, just write the C++
    // member function + descriptor entry as before; the descriptor
    // path takes precedence over this registry.
    vm->setDefault<jboolean>("android/media/AudioManager",    "isBluetoothA2dpOn", "()Z", JNI_FALSE);
    vm->setDefault<jint>    ("android/media/AudioManager",    "getStreamVolume",   "(I)I", 100);
    vm->setDefault<jint>    ("android/media/AudioDeviceInfo", "getType",           "()I",
                              jnivm::android::media::AudioDeviceInfo::TYPE_WIRED_HEADPHONES);

    HookStringExtensions(vm);
    HookClassExtensions(vm);
    HookIntExtensions(vm);
    HookObjectExtensions(vm);

    FakeJni::LocalFrame frame(*vm);
    auto classClass = vm->findClass("java/lang/Class");

    // Unity 2022 BlitType=Auto looks up Activity.getWindowManager() and
    // dereferences the Display. Descriptor inheritance does not always
    // expose Activity methods on the jobject Unity actually calls; hook
    // the method on every class Unity might FindClass.
    if (auto act = vm->findClass("android/app/Activity"))
        act->HookInstanceFunction(&frame.getJniEnv(), "getWindowManager", &hook_getWindowManager);
    if (auto upa = vm->findClass("com/unity3d/player/UnityPlayerActivity"))
        upa->HookInstanceFunction(&frame.getJniEnv(), "getWindowManager", &hook_getWindowManager);
    if (auto ctx = vm->findClass("android/content/Context"))
        ctx->HookInstanceFunction(&frame.getJniEnv(), "getWindowManager", &hook_getWindowManager);

    // libjnivm Field.getDeclaringClass — Unity GetFieldID for instance fields
    // re-resolves via declaring class + JNI GetFieldID. Prefer Field::declaringClass
    // (set by ReflectionHelper); keep a few hardcoded names as fallback.
    auto fieldClass = vm->findClass("java/lang/reflect/Field");
    fieldClass->HookInstanceFunction(&frame.getJniEnv(), "getDeclaringClass", [vm](jnivm::ENV* env, jnivm::Object* self) -> std::shared_ptr<jnivm::java::lang::Class> {
        if (self == nullptr)
            return nullptr;
        auto selfField = dynamic_cast<jnivm::java::lang::reflect::Field*>(self);
        if (!selfField)
            return nullptr;
        if (auto decl = selfField->declaringClass.lock())
            return decl;
        BD_LOG("UnityReflection", "getDeclaringClass fallback name=%s type=%s",
               selfField->name.c_str(), selfField->type.c_str());
        if (selfField->name == "currentActivity")
            return vm->findClass("com/unity3d/player/UnityPlayer");
        if (selfField->name == "PressedStates" || selfField->name == "mUnityPlayer" ||
            selfField->name == "MouseMode" || selfField->name == "MouseInside")
            return vm->findClass("com/unity3d/player/UnityPlayerActivity");
        if (selfField->name == "extras" || selfField->name == "flags" ||
            selfField->name == "number" || selfField->name == "when")
            return vm->findClass("android/app/Notification");
        return nullptr;
    });
}
