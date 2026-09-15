#ifndef __UNITY_H__
#define __UNITY_H__

#include "android.h"
#include "baron/baron.h"

// ── Android Game SDK (AGDK) GameActivity ──────────────────────────────────
// Unity 6's default Android entry point is a GameActivity: libgame.so
// (System.loadLibrary("game")) owns the native app glue, and
// com.unity3d.player.UnityPlayerGameActivity extends it. Registering the class
// name keeps FindClass()/RegisterNatives() working for that layout. The loader
// itself drives the ActivityOrService path today, so this is a name (plus an
// Activity base) rather than a full reimplementation — see docs/UNITY6.md.
namespace jnivm {
namespace com {
    namespace google {
        namespace androidgamesdk {
            class GameActivity : public jnivm::android::app::Activity {
            public:
                DEFINE_CLASS_NAME("com/google/androidgamesdk/GameActivity", jnivm::android::app::Activity)
            };
        }
    }
}
}

namespace jnivm {
namespace com {
    namespace unity3d {
        namespace player {

            class UnityPlayer;

            // Defined further down; UnityPlayer stores one (see m_HFPStatus).
            class HFPStatus;

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

            // Unity 6 renamed the mobile-data dialog callback and its method
            // (see the PlayAssetDeliveryUnityWrapper overloads below). Both
            // names stay declared so a 2020/2022 build of the same stub file
            // still resolves.
            class IAssetPackManagerConfirmationDialogCallback : public virtual FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/IAssetPackManagerConfirmationDialogCallback")
                virtual void onConfirmationDialogResult(FakeJni::JBoolean accepted);
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
                // Unity 6: init(UnityPlayer, Context). The 2020/2022 build of
                // this stub only had the Context parameter, which made every
                // lookup miss (the JNI signature lists both arguments):
                //   Unknown Static ... Member=init
                //   Sig=(Lcom/unity3d/player/UnityPlayer;Landroid/content/Context;)Lcom/unity3d/player/PlayAssetDeliveryUnityWrapper;
                static std::shared_ptr<PlayAssetDeliveryUnityWrapper> init(
                    std::shared_ptr<UnityPlayer> player,
                    std::shared_ptr<jnivm::android::content::Context> context);
                static std::shared_ptr<PlayAssetDeliveryUnityWrapper> init(
                    std::shared_ptr<jnivm::android::content::Context> context);
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
                // Unity 6 name for the same entry point (and its dialog-only
                // twin). The callback interface is
                // IAssetPackManagerConfirmationDialogCallback there.
                void requestToUseMobileData(std::shared_ptr<jnivm::android::app::Activity> activity,
                    std::shared_ptr<IAssetPackManagerConfirmationDialogCallback> callback);
                void showConfirmationDialog(std::shared_ptr<jnivm::android::app::Activity> activity,
                    std::shared_ptr<IAssetPackManagerConfirmationDialogCallback> callback);
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

                // Java keeps an HFPStatus here (UnityPlayer.m_HFPStatus) for its
                // Bluetooth hands-free audio routing; libunity's native side
                // caches the object when the HFPStatus constructor calls its
                // initHFPStatusJni(). Our Java side does not run, so main.cpp
                // fills this in and makes that call. Deliberately *not*
                // registered as a JNI field: libunity reaches the object through
                // its own cache, so this member only has to keep it alive.
                std::shared_ptr<HFPStatus> m_HFPStatus;

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

                // ── Unity 6 (6000.x) UnityPlayer methods ─────────────────────
                // libunity.so calls these on its UnityPlayerForActivityOrService
                // instance. In a real APK they are Java-side bookkeeping (phone
                // call listener, main-thread job queue, splash/content hiding,
                // UaaL detection, proxy lookup, orientation listener, insets
                // controller); the Java player is stubbed out here, so each one
                // answers with the "Java side absent" value. Registered on
                // UnityPlayer, not on UnityPlayerForActivityOrService, so the
                // lookup from the Unity 6 class is satisfied by inheritance —
                // same as getLaunchURL/hideSoftInput above.
                void addPhoneCallListener();
                void executeMainThreadJobs();
                std::shared_ptr<FakeJni::JString> getNetworkProxySettings(
                    std::shared_ptr<FakeJni::JString> url);
                void hidePreservedContent();
                bool isUaaLUseCase();
                // Returns false on purpose: a "true" here tells Unity the
                // library is already loaded into the Java classloader, and it
                // then skips its own lookup+dlopen. The loader does the dlopen
                // itself ([BD-DLOPEN]), which is the path that actually works.
                bool loadLibrary(std::shared_ptr<FakeJni::JString> library);
                bool shouldSetGameState();
                bool startOrientationListener(FakeJni::JInt orientation);
                bool supportsWindowInsetController();

                static std::shared_ptr<UnityPlayerActivity> currentActivity;
    
            };

            // com.unity3d.player.UnityPlayerUtilities. Unity 6 reflects this
            // class out of libunity.so and instantiates it with NewObject();
            // without a constructor here the engine logs
            //   Failed to create java object for com.unity3d.player.UnityPlayerUtilities
            // and skips the (debug-only) reference table dump it drives.
            class UnityPlayerUtilities : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayerUtilities")
                UnityPlayerUtilities();
                bool dumpReferenceTables();
            };

            // com.unity3d.player.HFPStatus. Unity's Java UnityPlayer news one of
            // these and keeps it in m_HFPStatus (both names are strings in
            // libunity.so, plus the four Java method names). The constructor is
            // what calls the private native initHFPStatusJni(), and *that* call
            // is how libunity caches the object it later drives for its
            // Bluetooth hands-free (SCO) audio routing:
            //   HFPStatus.<init> -> initHFPStatusJni()  [libunity caches this]
            //   ...              -> clearHFPStat()/getHFPStat()/setHFPRecordingStat()
            // None of that Java runs under this loader, so main.cpp does the
            // construction and the initHFPStatusJni() call itself. Skip them and
            // libunity ends up calling clearHFPStat() on a null object:
            //   CallMethod object is null
            //   [STUB-MISS] Unknown Member: Class=`Invalid` Member=`clearHFPStat`
            class HFPStatus : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/HFPStatus")

                explicit HFPStatus(std::shared_ptr<jnivm::android::content::Context> context);

                // Public Java surface (HFPStatus$1, the SCO broadcast receiver,
                // drives a()/b() through these). The loader has no Bluetooth
                // stack and never registers the receiver, so the cached SCO
                // state stays "disconnected" for the life of the process.
                void clearHFPStat();
                bool getHFPStat();
                void requestHFPStat();
                void setHFPRecordingStat(FakeJni::JBoolean recording);

            private:
                std::shared_ptr<jnivm::android::content::Context> mContext;
                // Java field d: the AudioManager the SCO calls go through.
                std::shared_ptr<jnivm::android::media::AudioManager> mAudioManager;
                // Java fields c/e/f: recording flag, SCO-stop-requested flag and
                // the SCO state cached from the broadcast (0 = DISCONNECTED).
                bool mRecording = false;
                bool mScoStopRequested = false;
                int mScoState = jnivm::android::media::AudioManager::SCO_AUDIO_STATE_DISCONNECTED;
            };

            // ── Unity 6 (6000.x) class layout ────────────────────────────
            // Unity 6 moved the player/render natives (nativeRender,
            // nativeResume, nativePause, nativeRecreateGfxState, ...) off
            // UnityPlayer onto UnityPlayerForActivityOrService, which is the
            // class UnityPlayerActivity instantiates, and added
            // UnityPlayerForGameActivity for the GameActivity entry point.
            // libunity.so FindClass()es these names from its JNI_OnLoad and
            // attaches the natives with RegisterNatives(), so the classes have
            // to exist in our JVM for that call to land anywhere. The natives
            // themselves come from libunity at runtime, not from here.
            class UnityPlayerForActivityOrService : public UnityPlayer {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayerForActivityOrService", UnityPlayer)
            };

            class UnityPlayerForGameActivity : public UnityPlayer {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayerForGameActivity", UnityPlayer)
            };

            class UnityPlayerForRenderService : public UnityPlayerForActivityOrService {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayerForRenderService", UnityPlayerForActivityOrService)
            };

            class UnityPlayerGameActivity : public jnivm::com::google::androidgamesdk::GameActivity {
            public:
                DEFINE_CLASS_NAME("com/unity3d/player/UnityPlayerGameActivity",
                                  jnivm::com::google::androidgamesdk::GameActivity)
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
