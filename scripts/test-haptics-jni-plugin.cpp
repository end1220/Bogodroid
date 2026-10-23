// Diagnostic plugin for an isolated container game copy, never distribution.
// Validates the actual loader's JNI descriptors after class registration.
#ifdef NDEBUG
#error "Haptics JNI probe requires assertions enabled"
#endif
#include "plugin_api.h"
#include <fake-jni/fake-jni.h>
#include <cassert>
#include <cstring>
#include <cstdio>

static BogoPluginApi api_storage;
static const BogoPluginApi* api = &api_storage;

static void probe(void*)
{
    auto* vm = static_cast<FakeJni::Jvm*>(api->jvm);
    auto* java_vm = static_cast<JavaVM*>(vm);
    JNIEnv* env = nullptr;
    const auto status = java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    const bool attached = status == JNI_EDETACHED;
    if (attached)
        assert(java_vm->AttachCurrentThread(&env, nullptr) == JNI_OK);
    else
        assert(status == JNI_OK);
    assert(env);
    env->PushLocalFrame(32);
    const auto context = env->FindClass("android/content/Context");
    const auto field = env->GetStaticFieldID(context, "VIBRATOR_SERVICE", "Ljava/lang/String;");
    const auto service = static_cast<jstring>(env->GetStaticObjectField(context, field));
    assert(service);
    const char* name = env->GetStringUTFChars(service, nullptr);
    assert(name && std::strcmp(name, "vibrator") == 0);
    env->ReleaseStringUTFChars(service, name);

    const auto permission = env->FindClass("android/Manifest$permission");
    const auto permission_field = env->GetStaticFieldID(permission, "VIBRATE", "Ljava/lang/String;");
    const auto permission_string = static_cast<jstring>(env->GetStaticObjectField(permission, permission_field));
    assert(permission_string);
    const char* value = env->GetStringUTFChars(permission_string, nullptr);
    assert(value && std::strcmp(value, "android.permission.VIBRATE") == 0);
    env->ReleaseStringUTFChars(permission_string, value);

    const auto ctor = env->GetMethodID(context, "<init>", "()V");
    const auto object = env->NewObject(context, ctor);
    const auto get_service = env->GetMethodID(context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    const auto vibrator = env->CallObjectMethod(object, get_service, service);
    const auto vibrator_class = env->FindClass("android/os/Vibrator");
    assert(vibrator && env->IsInstanceOf(vibrator, vibrator_class));
    const auto has = env->GetMethodID(vibrator_class, "hasVibrator", "()Z");
    const auto vibrate = env->GetMethodID(vibrator_class, "vibrate", "(J)V");
    const auto cancel = env->GetMethodID(vibrator_class, "cancel", "()V");
    assert(has && vibrate && cancel);
    // Container has no physical controller; must not advertise a fake motor.
    assert(env->CallBooleanMethod(vibrator, has) == JNI_FALSE);
    env->CallVoidMethod(vibrator, vibrate, static_cast<jlong>(500));
    env->CallVoidMethod(vibrator, cancel);
    assert(!env->ExceptionCheck());
    env->PopLocalFrame(nullptr);
    if (attached) java_vm->DetachCurrentThread();
    // Probe evidence must survive a production loader built with LOG=OFF.
    std::fprintf(stderr, "HAPTICS-TEST PASS: JNI service, permission, hasVibrator=false, vibrate(J)V, cancel()V\n");
}

extern "C" int bogodroid_plugin_init(const BogoPluginApi* host)
{
    api_storage = *host;
    return api->register_il2cpp_post_init(&probe, nullptr) ? BOGO_PLUGIN_OK : -1;
}
