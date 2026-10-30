#ifndef __UMP_H__
#define __UMP_H__

#include "android.h"
#include "baron/baron.h"
// UnityPlayerActivity appears in one erased-overload signature below; the
// descriptor macros need the complete type to derive it.
#include "unity.h"

// ── Google User Messaging Platform (UMP) consent stubs ───────────────────
//
// Why this file exists
// --------------------
// Blackmoor 2 (com.fourfats.blackmoor2) drives Google's UMP consent SDK from
// managed code before it touches anything else:
//
//   [BD-ANDROID] Unity: FFAds EnsureInitialized starting consent + init flow
//   [BD-ANDROID] Unity: FFAds GatherConsent starting
//   [BD-ANDROID] Unity: FFAds UMP test device IDs configured (1)
//   [JNIVM]: [STUB-MISS] Native MethodID: Class=`com/google/android/ump/
//       UserMessagingPlatform` Member=`getConsentInformation`
//       Sig=`(Lcom.unity3d.player.UnityPlayerActivity;)Ljava/lang/Object;`
//       -> returning default
//   [BD-JavaReflect] getConstructorID: com/google/android/ump/
//       ConsentRequestParameters$Builder ... methods=0
//   [BD-ANDROID] Unity: NullReferenceException: Object reference not set to an
//       instance of an object.
//   [BD-SEGV] signal 11 si_addr=0x7          (managed exception -> thread dies)
//
// The whole com.google.android.ump package was unregistered, so every class in
// it came back as a PHANTOM auto-stub with an empty method table. A null
// ConsentInformation and a null ConsentRequestParameters are not survivable in
// the ad SDK's C# layer, so startup aborted before the first real frame.
//
// Signature erasure — read this before adding a method
// ---------------------------------------------------
// The caller is Unity's AndroidJavaObject bridge, not Java. Unity erases the
// *declared* type of every object-returning call and of every AndroidJavaObject
// argument down to java.lang.Object, so the signatures it asks for are
//   getConsentInformation  (Lcom/unity3d/player/UnityPlayerActivity;)Ljava/lang/Object;
//   setDebugGeography      (I)Ljava/lang/Object;
// and NOT the Java types from Google's javadoc. jnivm's GetMethodID matches
// name+signature exactly, so a method must be *declared* with the erased C++
// types to be found:
//
//   std::shared_ptr<jnivm::Object>            -> Ljava/lang/Object;
//   std::shared_ptr<jnivm::UnityPlayerActivity> -> Lcom/unity3d/player/UnityPlayerActivity;
//
// Where the real (typed) Java signature is also plausibly in use, both
// overloads are declared; jnivm dispatches on the exact signature, so the two
// never collide as long as the C++ parameter lists differ.
//
// Deliberately no callbacks
// -------------------------
// requestConsentInfoUpdate()/loadAndShowConsentFormIfRequired() do *not* invoke
// the OnConsentInfoUpdateSuccess/Failure listeners. Those listeners arrive as
// java.lang.Object (managed proxies); calling back into them needs the proxy
// dispatch path, and a port with no network has nothing to report anyway. The
// stubs answer "consent not required / ads may be requested" immediately, which
// is what an offline single-player build should look like. If a game ever waits
// on the listener to unblock its UI, add the invocation here rather than
// papering over it in the loader.

namespace jnivm {
namespace com {
    namespace google {
        namespace android {
            namespace ump {

                class ConsentInformation;
                class ConsentRequestParameters;
                class ConsentDebugSettings;
                class ConsentForm;

                // com.google.android.ump.FormError — only ever constructed by
                // the real SDK, which we do not have. Declared so a failure
                // path that news one up does not fall into a phantom class.
                class FormError : public FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("com/google/android/ump/FormError")
                    FakeJni::JInt getErrorCode();
                    std::shared_ptr<FakeJni::JString> getMessage();
                };

                // com.google.android.ump.ConsentDebugSettings
                //
                // The real Builder constructor takes an Activity. Unity passes
                // the concrete UnityPlayerActivity, so the registered signature
                // is "(Landroid/app/Activity;)V" while the lookup asks for
                // "(Lcom/unity3d/player/UnityPlayerActivity;)V" — they do not
                // match, and ReflectionHelper::getConstructorID falls back to
                // "first registered <init> with a body". That fallback is why
                // this still constructs; see unity.cpp around the
                // "no bound <init>" log line.
                class ConsentDebugSettings : public FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("com/google/android/ump/ConsentDebugSettings")

                    class Builder : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/ump/ConsentDebugSettings$Builder")
                        Builder();
                        Builder(std::shared_ptr<jnivm::android::app::Activity> activity);
                        // Returns this so both `builder.setX(...).setY(...)` and
                        // statement-style calls work. Declared as Object because
                        // that is the signature Unity asks for.
                        std::shared_ptr<jnivm::Object> setDebugGeography(FakeJni::JInt geography);
                        std::shared_ptr<jnivm::Object> addTestDeviceHashedId(std::shared_ptr<FakeJni::JString> id);
                        std::shared_ptr<ConsentDebugSettings> build();
                    };
                };

                // com.google.android.ump.ConsentRequestParameters
                class ConsentRequestParameters : public FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("com/google/android/ump/ConsentRequestParameters")

                    class Builder : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/ump/ConsentRequestParameters$Builder")
                        Builder();
                        std::shared_ptr<jnivm::Object> setTagForUnderAgeOfConsent(FakeJni::JBoolean tag);
                        std::shared_ptr<jnivm::Object> setConsentDebugSettings(std::shared_ptr<jnivm::Object> settings);
                        std::shared_ptr<ConsentRequestParameters> build();
                    };
                };

                // com.google.android.ump.ConsentInformation
                //
                // Answers the "we are offline, consent does not apply" state:
                //   getConsentStatus()                     -> 1 NOT_REQUIRED
                //   canRequestAds()                        -> true
                //   isConsentFormAvailable()               -> false
                //   getPrivacyOptionsRequirementStatus()   -> 2 NOT_REQUIRED
                // Anything stricter (canRequestAds=false) makes the ad SDK
                // declare itself unusable, which some integrations treat as a
                // fatal init error rather than a "no ads today".
                class ConsentInformation : public FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("com/google/android/ump/ConsentInformation")

                    FakeJni::JInt getConsentStatus();
                    FakeJni::JBoolean canRequestAds();
                    FakeJni::JBoolean isConsentFormAvailable();
                    FakeJni::JInt getPrivacyOptionsRequirementStatus();
                    void requestConsentInfoUpdate(std::shared_ptr<jnivm::Object> activity,
                        std::shared_ptr<jnivm::Object> params,
                        std::shared_ptr<jnivm::Object> successListener,
                        std::shared_ptr<jnivm::Object> failureListener);
                    void reset();
                };

                // com.google.android.ump.ConsentForm
                class ConsentForm : public FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("com/google/android/ump/ConsentForm")
                    void show(std::shared_ptr<jnivm::Object> activity,
                        std::shared_ptr<jnivm::Object> listener);
                };

                class UserMessagingPlatform : public FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("com/google/android/ump/UserMessagingPlatform")

                    // Erased form actually asked for by Unity's AndroidJavaObject
                    // bridge (see the header comment). Returns a live
                    // ConsentInformation, never null — null here is what killed
                    // startup.
                    static std::shared_ptr<jnivm::Object> getConsentInformation(
                        std::shared_ptr<jnivm::com::unity3d::player::UnityPlayerActivity> activity);
                    // Typed Java form, for callers that use the documented API.
                    static std::shared_ptr<ConsentInformation> getConsentInformation(
                        std::shared_ptr<jnivm::android::content::Context> context);

                    static void requestConsentInfoUpdate(std::shared_ptr<jnivm::Object> activity,
                        std::shared_ptr<jnivm::Object> params,
                        std::shared_ptr<jnivm::Object> successListener,
                        std::shared_ptr<jnivm::Object> failureListener);
                    static void requestConsentInfoUpdate(
                        std::shared_ptr<jnivm::android::app::Activity> activity,
                        std::shared_ptr<ConsentRequestParameters> params,
                        std::shared_ptr<jnivm::Object> successListener,
                        std::shared_ptr<jnivm::Object> failureListener);

                    static void loadAndShowConsentFormIfRequired(
                        std::shared_ptr<jnivm::Object> activity,
                        std::shared_ptr<jnivm::Object> listener);
                    static void showPrivacyOptionsForm(std::shared_ptr<jnivm::Object> activity,
                        std::shared_ptr<jnivm::Object> listener);
                    static FakeJni::JBoolean isPrivacyOptionsRequired();
                };
            }
        }
    }
}
}

#endif
