
#ifndef __JNIBRIDGE_H__
#define __JNIBRIDGE_H__

#include "android.h" // For Handler::Callback
#include "baron/baron.h"
#include "javac.h"
#include "logging.h"
#include "unity.h"
#ifdef BD_ENABLE_GPLAY
#include "gplay.h"
#endif
#include <set>
#include <string>

namespace jnivm {
namespace bitter {
    namespace jnibridge {

        /**
         * A single, concrete proxy class that can dynamically implement multiple interfaces.
         * It inherits from all possible interfaces to satisfy the JNI type system.
         * At runtime, it checks which interfaces it was created to proxy.
         */
        class JNIBridgeProxy : public jnivm::java::lang::Runnable,
                               public jnivm::android::os::Handler::Callback,
                               public jnivm::android::view::Choreographer::FrameCallback,
                               public jnivm::android::view::ViewOnLayoutChangeListener,
                               public jnivm::android::hardware::input::InputManager::InputDeviceListener,
                               public jnivm::android::hardware::display::DisplayManager::DisplayListener,
                               public jnivm::com::unity3d::player::IAssetPackManagerStatusQueryCallback,
                               public jnivm::com::unity3d::player::IAssetPackManagerDownloadStatusCallback,
                               public jnivm::com::unity3d::player::IAssetPackManagerMobileDataConfirmationCallback,
                               public jnivm::com::unity3d::player::IAssetPackManagerConfirmationDialogCallback
#ifdef BD_ENABLE_GPLAY
                               , public jnivm::com::google::android::vending::licensing::LicenseCheckerCallback
#endif
                                {
        public:
            // This gives the class a stable, registerable name for your JNI layer.
            // Keep the VM descriptor flat. jnivm's descriptor helper cannot
            // select one final getClassInternal() override from several
            // interface descriptors; the C++ inheritance above still makes
            // shared_ptr casts to each callback interface valid.
            DEFINE_CLASS_NAME("bitter/jnibridge/JNIBridgeProxy")

            enum class InvocationMode {
                NativePointer,
                ManagedGCHandle
            };

            long nativeHandle;

            // Set by bitter.jnibridge.JNIBridge.disableInterfaceProxy() when
            // the engine retires a proxy (it is about to drop the native handle
            // the proxy would call back into). Java stops forwarding at that
            // point; so do we. Each drop is logged, so a proxy that Unity
            // retires too early is visible in a trace instead of showing up as
            // "input/rendering silently stopped".
            bool disabled = false;

        private:
            // Stores the names of the interfaces this specific instance should implement.
            std::set<std::string> implementedInterfaces;
            InvocationMode invocationMode;

            template <typename... Args>
            void invoke(const char* className, const char* methodName,
                        const char* methodSig, Args... args);

        public:
            JNIBridgeProxy(long handle, const std::set<std::string>& interfaces,
                           InvocationMode mode = InvocationMode::NativePointer);

            // --- Implementation of java.lang.Runnable ---
            void run() override;

            // --- Implementation of android.os.Handler.Callback ---
            bool handleMessage(std::shared_ptr<jnivm::android::os::Message> msg) override;

            // --- Implementation of android.view.Choreographer.FrameCallback ---
            void doFrame(jlong frameTimeNanos) override;

            // --- android.view.View$OnLayoutChangeListener /
            //     android.hardware.display.DisplayManager$DisplayListener ---
            // Unity 6 forName's both interfaces and passes a JNIBridge proxy in
            // (see registerDisplayListener / addOnLayoutChangeListener in
            // javastubs/android_misc.cpp and android_view.cpp). The proxy has to
            // derive from both or jnivm's parameter unpack throws
            // "Invalid Reference, Unexpected Type" before the stub body runs.
            void onLayoutChange(std::shared_ptr<jnivm::android::view::View> view,
                                int left, int top, int right, int bottom,
                                int oldLeft, int oldTop, int oldRight, int oldBottom) override;
            void onDisplayAdded(int displayId) override;
            void onDisplayChanged(int displayId) override;
            void onDisplayRemoved(int displayId) override;

            // --- Implementation of Unity Play Asset Delivery callbacks ---
            void onStatusResult(
                FakeJni::JLong sequence,
                std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
                std::shared_ptr<FakeJni::JIntArray> statuses,
                std::shared_ptr<FakeJni::JIntArray> errors) override;

            void onStatusUpdate(
                std::shared_ptr<FakeJni::JString> name,
                FakeJni::JInt status,
                FakeJni::JLong transferred,
                FakeJni::JLong total,
                FakeJni::JInt error,
                FakeJni::JInt errorCode) override;

            void onMobileDataConfirmationResult(FakeJni::JBoolean accepted) override;

            // Unity 6 name for the same callback (see the interface classes in
            // unity.h); forwarded to whichever native handle this proxy holds.
            void onConfirmationDialogResult(FakeJni::JBoolean accepted) override;

#ifdef BD_ENABLE_GPLAY
            void allow(jint reason) override;
            void dontAllow(jint reason) override;
            void applicationError(jint errorCode) override;
#endif
        };

        /**
         * The factory class that creates and manages proxies.
         */
        class JNIBridge : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("bitter/jnibridge/JNIBridge")

            // The factory function is now much simpler.
            static std::shared_ptr<jnivm::java::lang::Object> newInterfaceProxy(FakeJni::JLong j, std::shared_ptr<FakeJni::JArray<FakeJni::JClass>> classes);

            // Java: static void disableInterfaceProxy(Object proxy). Unity 6
            // calls it on a proxy it has finished with (it is Java code in the
            // real APK, so without this stub the call is a STUB-MISS).
            static void disableInterfaceProxy(std::shared_ptr<jnivm::Object> proxy);

            // The static invoker remains the same powerful, generic helper.
            template <typename... Args>
            static void invoke(long nativeHandle, const char* className, const char* methodName, const char* methodSig, Args... args);

            template <typename... Args>
            static void invokeManaged(long nativeHandle, const char* methodName, Args... args);
        };

    }
}
}
#endif
