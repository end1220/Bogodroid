#ifndef __UNITY_H__
#define __UNITY_H__

#include "android.h"
#include "baron/baron.h"
namespace jnivm {
namespace com {
    namespace unity3d {
        namespace player {
            class UnityPlayer;

            class PlayAssetDeliveryUnityWrapper : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/PlayAssetDeliveryUnityWrapper")
                bool playCoreApiMissing();
                static std::shared_ptr<PlayAssetDeliveryUnityWrapper> init(std::shared_ptr<jnivm::android::content::Context> context);
            };

            class UnityPlayerActivity : public jnivm::android::app::Activity {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayerActivity", jnivm::android::app::Activity)
                std::shared_ptr<UnityPlayer> mUnityPlayer;
                FakeJni::JInt MouseMode = -1;
                FakeJni::JBoolean MouseInside = JNI_TRUE;
                std::shared_ptr<FakeJni::JBooleanArray> PressedStates = std::make_shared<FakeJni::JBooleanArray>(330);

                bool injectEvent(std::shared_ptr<android::view::InputEvent> event);
            };

            class UnityPlayer : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayer")

                bool initializeGoogleAr();
                std::shared_ptr<FakeJni::JString> getLaunchURL();
                void hideSoftInput();
                void showSoftInput(std::shared_ptr<FakeJni::JString> text, FakeJni::JInt keyboardType,
                                   FakeJni::JBoolean autocorrection, FakeJni::JBoolean multiline,
                                   FakeJni::JBoolean secure, FakeJni::JBoolean alert,
                                   std::shared_ptr<FakeJni::JString> placeholder,
                                   FakeJni::JInt characterLimit, FakeJni::JBoolean reuseKeyboard,
                                   FakeJni::JBoolean hideInput);
                void setSoftInputStr(std::shared_ptr<FakeJni::JString> text);
                void setSoftInputStrWithAction(std::shared_ptr<FakeJni::JString> text, FakeJni::JInt action);
                FakeJni::JInt getKeyboardLayout();
                void startActivityIndicator(FakeJni::JInt unused);
                void stopActivityIndicator();

                static std::shared_ptr<UnityPlayerActivity> currentActivity;
    
            };

            class ReflectionHelper : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/ReflectionHelper")
                static std::shared_ptr<jnivm::java::lang::reflect::Constructor> getConstructorID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> signature);
                static std::shared_ptr<jnivm::java::lang::reflect::Method> getMethodID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> methodName, std::shared_ptr<FakeJni::JString> signature, bool isStatic);
                static std::shared_ptr<jnivm::java::lang::reflect::Field> getFieldID(std::shared_ptr<jnivm::java::lang::Class> clazz, std::shared_ptr<FakeJni::JString> fieldName, std::shared_ptr<FakeJni::JString> signature, bool isStatic);
                static std::shared_ptr<FakeJni::JString> getFieldSignature(std::shared_ptr<jnivm::java::lang::reflect::Field> field);
                static std::shared_ptr<jnivm::Object> newProxyInstance(std::shared_ptr<UnityPlayer> player, long nativeHandle, std::shared_ptr<jnivm::Class> interfaces);
                static std::shared_ptr<jnivm::Object> createInvocationError(long nativeHandle, bool toggle);

                class InvocationError : public FakeJni::JObject {
                    public:
                    DEFINE_CLASS_NAME("com/unity3d/player/ReflectionHelper$InvocationError")
                    InvocationError(long handle, bool toggle) { printf("InvocationError: %ld %d\n",handle,toggle);}
                };
            };
        }

    }
}
}
#endif
