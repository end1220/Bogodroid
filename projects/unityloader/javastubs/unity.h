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

                // Unity 2022 player JNI used at boot; return safe no-ops.
                static std::shared_ptr<jnivm::android::app::Notification> getNotificationFromIntent(
                    std::shared_ptr<jnivm::android::content::Intent> intent);
                static FakeJni::JBoolean isUaaLUseCase();
                static std::shared_ptr<FakeJni::JString> getNetworkProxySettings(
                    std::shared_ptr<FakeJni::JString> url);
                void addPhoneCallListener();
                void hidePreservedContent();

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

namespace jnivm {
namespace com {
namespace unity {
namespace androidnotifications {
    class NotificationCallback : public FakeJni::JObject {
    public:
        DEFINE_CLASS_NAME("com/unity/androidnotifications/NotificationCallback")
    };

    // Minimal stubs so Unity.Notifications.Android.JniApi.FindMethod succeeds.
    class UnityNotificationManager : public FakeJni::JObject {
    public:
        DEFINE_CLASS_NAME("com/unity/androidnotifications/UnityNotificationManager")

        inline static FakeJni::JString KEY_FIRE_TIME = (FakeJni::JString)"fireTime";
        inline static FakeJni::JString KEY_ID = (FakeJni::JString)"id";
        inline static FakeJni::JString KEY_INTENT_DATA = (FakeJni::JString)"data";
        inline static FakeJni::JString KEY_LARGE_ICON = (FakeJni::JString)"largeIcon";
        inline static FakeJni::JString KEY_REPEAT_INTERVAL = (FakeJni::JString)"repeatInterval";
        inline static FakeJni::JString KEY_NOTIFICATION = (FakeJni::JString)"unityNotification";
        inline static FakeJni::JString KEY_SMALL_ICON = (FakeJni::JString)"smallIcon";
        inline static FakeJni::JString KEY_SHOW_IN_FOREGROUND = (FakeJni::JString)"showInForeground";
        inline static FakeJni::JString KEY_BIG_PICTURE = (FakeJni::JString)"bigPicture";
        inline static FakeJni::JString KEY_BIG_LARGE_ICON = (FakeJni::JString)"bigLargeIcon";
        inline static FakeJni::JString KEY_BIG_CONTENT_TITLE = (FakeJni::JString)"bigContentTitle";
        inline static FakeJni::JString KEY_BIG_SUMMARY_TEXT = (FakeJni::JString)"bigSummaryText";
        inline static FakeJni::JString KEY_BIG_CONTENT_DESCRIPTION = (FakeJni::JString)"bigContentDescription";
        inline static FakeJni::JString KEY_BIG_SHOW_WHEN_COLLAPSED = (FakeJni::JString)"bigShowWhenCollapsed";

        static std::shared_ptr<jnivm::Object> getNotificationManagerImpl(
            std::shared_ptr<jnivm::Object> activity,
            std::shared_ptr<jnivm::Object> callback);

        std::shared_ptr<jnivm::android::app::Notification> getNotificationFromIntent(
            std::shared_ptr<jnivm::android::content::Intent> intent);

        static void setNotificationIcon(
            std::shared_ptr<jnivm::Object> builder,
            std::shared_ptr<FakeJni::JString> largeIconPath,
            std::shared_ptr<FakeJni::JString> smallIconPath);
        static void setNotificationColor(std::shared_ptr<jnivm::Object> builder, FakeJni::JInt color);
        static FakeJni::JInt getNotificationColor(std::shared_ptr<jnivm::Object> notification);
        static void setNotificationUsesChronometer(std::shared_ptr<jnivm::Object> builder, FakeJni::JBoolean uses);
        static void setNotificationGroupAlertBehavior(std::shared_ptr<jnivm::Object> builder, FakeJni::JInt behavior);
        static FakeJni::JInt getNotificationGroupAlertBehavior(std::shared_ptr<jnivm::Object> notification);
        static std::shared_ptr<FakeJni::JString> getNotificationChannelId(std::shared_ptr<jnivm::Object> notification);

        // Instance API on the manager returned by getNotificationManagerImpl.
        FakeJni::JInt scheduleNotification(std::shared_ptr<jnivm::Object> builder, FakeJni::JBoolean customized);
        void cancelNotification(FakeJni::JInt id);
        void cancelAllNotifications();
        void cancelAllPendingNotificationIntents();
        FakeJni::JBoolean areNotificationsEnabled();
        FakeJni::JInt areNotificationsEnabledInt();
        std::shared_ptr<jnivm::android::app::Notification::Builder> createNotificationBuilder(
            std::shared_ptr<FakeJni::JString> channelId);
    };
}
}
}
}

#endif
