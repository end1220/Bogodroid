    
#include "../globals.h"
#include "jnibridge.h"
#include "javac.h"
#include "baron/baron.h"
#include "logging.h"
#include "platform.h"
#include <atomic>
#include <cstring>
#include <set>
#include <string>

using namespace jnivm::bitter::jnibridge;

// FrameCallback.doFrame / Handler.handleMessage fire every frame and drown
// useful diagnostics. Keep a few early samples plus a sparse heartbeat.
static bool bd_should_log_jni_invoke(const char* className, const char* methodName)
{
    const bool hot =
        (methodName && std::strcmp(methodName, "doFrame") == 0) ||
        (methodName && std::strcmp(methodName, "handleMessage") == 0);
    if (!hot)
        return true;

    static std::atomic<uint32_t> hot_count{0};
    const uint32_t n = ++hot_count;
    if (n <= 3 || n == 60 || (n % 600) == 0) {
        BD_LOG("JNIBridge", "hot invoke #%u %s->%s (further doFrame/handleMessage suppressed)",
               n, className ? className : "?", methodName ? methodName : "?");
    }
    return false;
}

// --- JNIBridgeProxy Implementation ---

JNIBridgeProxy::JNIBridgeProxy(long handle, const std::set<std::string>& interfaces,
                               InvocationMode mode)
    : nativeHandle(handle), implementedInterfaces(interfaces), invocationMode(mode) {}

template <typename... Args>
void JNIBridgeProxy::invoke(const char* className, const char* methodName,
                           const char* methodSig, Args... args) {
    if (invocationMode == InvocationMode::ManagedGCHandle) {
        JNIBridge::invokeManaged(nativeHandle, methodName, args...);
        return;
    }
    JNIBridge::invoke(nativeHandle, className, methodName, methodSig, args...);
}

void JNIBridgeProxy::run() {
    // Runtime check: does this instance actually implement Runnable?
    if (implementedInterfaces.count("java/lang/Runnable")) {
        // Yes, so forward the call to the native engine via the invoker.
        invoke("java/lang/Runnable", "run", "()V");
    } else {
        // This should not happen if the engine is well-behaved.
        verbose("JNIBridgeProxy", "run() called on a proxy that doesn't implement java/lang/Runnable!");
    }
}

bool JNIBridgeProxy::handleMessage(std::shared_ptr<jnivm::android::os::Message> msg) {
    // Runtime check: does this instance actually implement Handler.Callback?
    if (implementedInterfaces.count("android/os/Handler$Callback")) {
        // Yes, so forward the call.
        invoke("android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z", msg);
        // The native invoke probably returns a Boolean object. We'll assume null means 'false'.
        return true;
    } else {
        verbose("JNIBridgeProxy", "handleMessage() called on a proxy that doesn't implement android/os/Handler$Callback!");
        return false;
    }
}

void JNIBridgeProxy::doFrame(jlong frameTimeNanos) {
    if (implementedInterfaces.count("android/view/Choreographer$FrameCallback")) {
        // Note: The native method probably doesn't take an argument. The frame time
        // is usually queried from a native system. We just call the method.
        invoke("android/view/Choreographer$FrameCallback", "doFrame", "(J)V" /* Check signature! */, frameTimeNanos);
    } else {
        verbose("JNIBridgeProxy", "doFrame() called on a proxy that doesn't implement FrameCallback!");
    }
}

void JNIBridgeProxy::onStatusResult(
    FakeJni::JLong sequence,
    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
    std::shared_ptr<FakeJni::JIntArray> statuses,
    std::shared_ptr<FakeJni::JIntArray> errors) {
    if (implementedInterfaces.count(
            "com/unity3d/player/IAssetPackManagerStatusQueryCallback")) {
        invoke(
            "com/unity3d/player/IAssetPackManagerStatusQueryCallback",
            "onStatusResult",
            "(J[Ljava/lang/String;[I[I)V",
            sequence, names, statuses, errors);
    } else {
        verbose("JNIBridgeProxy", "onStatusResult() called on a proxy that doesn't implement PAD status callback!");
    }
}

void JNIBridgeProxy::onStatusUpdate(
    std::shared_ptr<FakeJni::JString> name,
    FakeJni::JInt status,
    FakeJni::JLong transferred,
    FakeJni::JLong total,
    FakeJni::JInt error,
    FakeJni::JInt errorCode) {
    if (implementedInterfaces.count(
            "com/unity3d/player/IAssetPackManagerDownloadStatusCallback")) {
        invoke(
            "com/unity3d/player/IAssetPackManagerDownloadStatusCallback",
            "onStatusUpdate",
            "(Ljava/lang/String;IJJII)V",
            name, status, transferred, total, error, errorCode);
    } else {
        verbose("JNIBridgeProxy", "onStatusUpdate() called on a proxy that doesn't implement PAD download callback!");
    }
}

void JNIBridgeProxy::onMobileDataConfirmationResult(FakeJni::JBoolean accepted) {
    if (implementedInterfaces.count(
            "com/unity3d/player/IAssetPackManagerMobileDataConfirmationCallback")) {
        invoke(
            "com/unity3d/player/IAssetPackManagerMobileDataConfirmationCallback",
            "onMobileDataConfirmationResult",
            "(Z)V",
            accepted);
    } else {
        verbose("JNIBridgeProxy", "onMobileDataConfirmationResult() called on a proxy that doesn't implement PAD mobile-data callback!");
    }
}

#ifdef BD_ENABLE_GPLAY
void JNIBridgeProxy::allow(jint reason) {
    JNIBridge::invoke(nativeHandle,
        "com/google/android/vending/licensing/LicenseCheckerCallback",
        "allow", "(I)V", reason);
}

void JNIBridgeProxy::dontAllow(jint reason) {
    JNIBridge::invoke(nativeHandle,
        "com/google/android/vending/licensing/LicenseCheckerCallback",
        "dontAllow", "(I)V", reason);
}

void JNIBridgeProxy::applicationError(jint errorCode) {
    JNIBridge::invoke(nativeHandle,
        "com/google/android/vending/licensing/LicenseCheckerCallback",
        "applicationError", "(I)V", errorCode);
}
#endif

// --- JNIBridge Factory and Invoker Implementation ---

std::shared_ptr<jnivm::java::lang::Object> JNIBridge::newInterfaceProxy(FakeJni::JLong j, std::shared_ptr<jnivm::Array<FakeJni::JClass>> classes) {
    
    std::set<std::string> interfaceNames;
    for (int i = 0; i < classes->getSize(); i++) {
        std::string name = (*classes)[i]->getName();
        interfaceNames.insert(name);
        verbose("JBRIDGE", "Requesting proxy to implement: %s", name.c_str());
    }

    // The factory is now trivial. It always creates the same C++ type,
    // just configured with a different set of interfaces to implement.
    auto proxy = std::make_shared<JNIBridgeProxy>(j, interfaceNames);
    return proxy;
}

// The generic, type-safe C++ function that calls back into the native engine.
template<typename... Args>
void JNIBridge::invoke(long nativeHandle, const char* className, const char* methodName, const char* methodSig, Args... args) {
    

    // Find the JNIBridge.invoke method
    auto jniBridgeClass = vm.findClass("bitter/jnibridge/JNIBridge").get();
    auto invokeMethod = jniBridgeClass->getMethod("(JLjava/lang/Class;Ljava/lang/reflect/Method;[Ljava/lang/Object;)Ljava/lang/Object;", "invoke");

    // Get the Class and Method objects for the interface method we're proxying
    auto interfaceClass = vm.findClass(className);
    auto interfaceMethod = std::shared_ptr<Method>((Method*)interfaceClass->getMethod(methodSig, methodName));

    // Package the C++ arguments into a Java Object array
    std::shared_ptr<FakeJni::JArray<jnivm::java::lang::Object>> argsArray = std::make_shared<FakeJni::JArray<jnivm::java::lang::Object>>(sizeof...(args));
    int i = 0;

    // We must explicitly cast each argument to the base Object type.
     ( ( (*argsArray)[i++] = autobox(args) ), ... );
    if (bd_should_log_jni_invoke(className, methodName))
        verbose("JNIBridge", "Invoking native handle %ld for %s->%s", nativeHandle, className, methodName);
    FakeJni::LocalFrame frame(vm);
    // The invoke call itself was correct, as you pointed out.
    invokeMethod.invoke(frame.getJniEnv(), jniBridgeClass, nativeHandle, interfaceClass, interfaceMethod, argsArray);
}

template<typename... Args>
void JNIBridge::invokeManaged(long nativeHandle, const char* methodName, Args... args) {
    auto reflectionClass = vm.findClass("com/unity3d/player/ReflectionHelper").get();
    // Unity 2020 registers this handle as a jlong. This is a managed GCHandle,
    // not the ProxyInvoker* accepted by bitter.jnibridge.JNIBridge.invoke.
    auto invokeMethod = reflectionClass->getMethod(
        "(JLjava/lang/String;[Ljava/lang/Object;)Ljava/lang/Object;",
        "nativeProxyInvoke");
    auto argsArray = std::make_shared<FakeJni::JArray<jnivm::java::lang::Object>>(sizeof...(args));
    int i = 0;
    (((*argsArray)[i++] = autobox(args)), ...);
    auto name = std::make_shared<FakeJni::JString>(methodName);
    if (bd_should_log_jni_invoke("ReflectionHelper", methodName))
        verbose("JNIBridge", "Invoking managed GCHandle %ld for %s", nativeHandle, methodName);
    FakeJni::LocalFrame frame(vm);
    invokeMethod.invoke(frame.getJniEnv(), reflectionClass, nativeHandle, name, argsArray);
}


// Explicit template instantiation is still required to prevent linker errors.
template void JNIBridge::invoke(long, const char*, const char*, const char*); // For Runnable.run()
template void JNIBridge::invoke(long, const char*, const char*, const char*, std::shared_ptr<jnivm::android::os::Message>); // For Handler.Callback.handleMessage()
template void JNIBridge::invoke(long, const char*, const char*, const char*, jlong); // for FrameCallback.doFrame()
template void JNIBridge::invoke(long, const char*, const char*, const char*, jint);
template void JNIBridge::invoke(long, const char*, const char*, const char*,
                                FakeJni::JLong,
                                std::shared_ptr<FakeJni::JArray<FakeJni::JString>>,
                                std::shared_ptr<FakeJni::JIntArray>,
                                std::shared_ptr<FakeJni::JIntArray>);
template void JNIBridge::invoke(long, const char*, const char*, const char*,
                                std::shared_ptr<FakeJni::JString>,
                                FakeJni::JInt,
                                FakeJni::JLong,
                                FakeJni::JLong,
                                FakeJni::JInt,
                                FakeJni::JInt);
template void JNIBridge::invoke(long, const char*, const char*, const char*, FakeJni::JBoolean);


BEGIN_NATIVE_DESCRIPTOR(jnivm::bitter::jnibridge::JNIBridge) { FakeJni::Function<&JNIBridge::newInterfaceProxy> {}, "newInterfaceProxy", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::bitter::jnibridge::JNIBridgeProxy)
        END_NATIVE_DESCRIPTOR
