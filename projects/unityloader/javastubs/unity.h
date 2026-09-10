#ifndef __UNITY_H__
#define __UNITY_H__

#include "android.h"
#include "baron/baron.h"
namespace jnivm {
namespace com {
    namespace unity3d {
        namespace player {
            class UnityPlayer;

            class IAssetPackManagerStatusQueryCallback : public virtual FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/IAssetPackManagerStatusQueryCallback")
                virtual void onStatusResult(FakeJni::JLong sequence,
                    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
                    std::shared_ptr<FakeJni::JIntArray> statuses,
                    std::shared_ptr<FakeJni::JIntArray> errors);
            };

            class IAssetPackManagerDownloadStatusCallback : public virtual FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/IAssetPackManagerDownloadStatusCallback")
                virtual void onStatusUpdate(std::shared_ptr<FakeJni::JString> name,
                    FakeJni::JInt status, FakeJni::JLong transferred,
                    FakeJni::JLong total, FakeJni::JInt error,
                    FakeJni::JInt errorCode);
            };

            class IAssetPackManagerMobileDataConfirmationCallback : public virtual FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/IAssetPackManagerMobileDataConfirmationCallback")
                virtual void onMobileDataConfirmationResult(FakeJni::JBoolean accepted);
            };

            class UnityCoreAssetPacksStatusCallbacks
                : public IAssetPackManagerStatusQueryCallback,
                  public IAssetPackManagerDownloadStatusCallback {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityCoreAssetPacksStatusCallbacks")
                void onStatusResult(FakeJni::JLong sequence,
                    std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
                    std::shared_ptr<FakeJni::JIntArray> statuses,
                    std::shared_ptr<FakeJni::JIntArray> errors) override;
                void onStatusUpdate(std::shared_ptr<FakeJni::JString> name,
                    FakeJni::JInt status, FakeJni::JLong transferred,
                    FakeJni::JLong total, FakeJni::JInt error,
                    FakeJni::JInt errorCode) override;

            private:
                void report(std::shared_ptr<FakeJni::JString> name,
                    FakeJni::JInt status, FakeJni::JInt error);
            };

            class PlayAssetDeliveryUnityWrapper : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/PlayAssetDeliveryUnityWrapper")
                bool playCoreApiMissing();
                static std::shared_ptr<PlayAssetDeliveryUnityWrapper> getInstance();
                static std::shared_ptr<PlayAssetDeliveryUnityWrapper> init(std::shared_ptr<jnivm::android::content::Context> context);
                void cancelAssetPackDownload(std::shared_ptr<FakeJni::JString> name);
                void cancelAssetPackDownloads(std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names);
                void downloadAssetPack(std::shared_ptr<FakeJni::JString> name,
                    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback);
                void downloadAssetPacks(std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
                    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback);
                std::shared_ptr<FakeJni::JString> getAssetPackPath(std::shared_ptr<FakeJni::JString> name);
                void getAssetPackState(std::shared_ptr<FakeJni::JString> name,
                    std::shared_ptr<IAssetPackManagerStatusQueryCallback> callback);
                void getAssetPackStates(std::shared_ptr<FakeJni::JArray<FakeJni::JString>> names,
                    std::shared_ptr<IAssetPackManagerStatusQueryCallback> callback);
                std::shared_ptr<jnivm::Object> registerDownloadStatusListener(
                    std::shared_ptr<IAssetPackManagerDownloadStatusCallback> callback);
                void removeAssetPack(std::shared_ptr<FakeJni::JString> name);
                void requestToUseMobileData(std::shared_ptr<jnivm::android::app::Activity> activity,
                    std::shared_ptr<IAssetPackManagerMobileDataConfirmationCallback> callback);
                void unregisterDownloadStatusListener(std::shared_ptr<jnivm::Object> token);

            private:
                static std::shared_ptr<PlayAssetDeliveryUnityWrapper> instance;
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
                static void setNativeExceptionOnProxy(std::shared_ptr<jnivm::Object> proxy, long nativeHandle, bool hasException);
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
