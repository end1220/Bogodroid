#include "ump.h"
#include "logging.h"

// See ump.h for why the whole com.google.android.ump package has to exist and
// why the signatures below are the *erased* ones Unity asks for.
//
// Every entry logs through BD_LOG("UMP", ...) on purpose: the managed consent
// flow is the first thing Blackmoor 2 runs, and the only way to tell "the stub
// answered" apart from "the lookup missed and jnivm answered with a default" is
// to see the stub's own line in the log. A missing BD-UMP line next to a
// [STUB-MISS] means the signature did not match, not that the code is wrong.

namespace {

// One instance for the whole process, mirroring the real SDK: ConsentInformation
// is a process-wide singleton and the ad SDK caches the reference anyway.
std::shared_ptr<jnivm::com::google::android::ump::ConsentInformation> g_consent_information;

std::shared_ptr<jnivm::com::google::android::ump::ConsentInformation> consent_information()
{
    if (!g_consent_information)
        g_consent_information = std::make_shared<jnivm::com::google::android::ump::ConsentInformation>();
    return g_consent_information;
}

const char* consent_status_name(int status)
{
    switch (status) {
    case 0: return "UNKNOWN";
    case 1: return "NOT_REQUIRED";
    case 2: return "REQUIRED";
    case 3: return "OBTAINED";
    default: return "?";
    }
}

} // namespace

/// FormError
FakeJni::JInt jnivm::com::google::android::ump::FormError::getErrorCode()
{
    // 0 == the real SDK's "no error" value.
    BD_LOG("UMP", "FormError.getErrorCode() -> 0");
    return 0;
}

std::shared_ptr<FakeJni::JString> jnivm::com::google::android::ump::FormError::getMessage()
{
    BD_LOG("UMP", "FormError.getMessage() -> \"\"");
    return std::make_shared<FakeJni::JString>("");
}

/// ConsentDebugSettings::Builder
jnivm::com::google::android::ump::ConsentDebugSettings::Builder::Builder()
{
    BD_LOG("UMP", "new ConsentDebugSettings$Builder()");
}

jnivm::com::google::android::ump::ConsentDebugSettings::Builder::Builder(
    std::shared_ptr<jnivm::android::app::Activity> activity)
{
    BD_LOG("UMP", "new ConsentDebugSettings$Builder(activity=%p)", activity.get());
}

std::shared_ptr<jnivm::Object>
jnivm::com::google::android::ump::ConsentDebugSettings::Builder::setDebugGeography(FakeJni::JInt geography)
{
    BD_LOG("UMP", "ConsentDebugSettings$Builder.setDebugGeography(%d)", (int)geography);
    return std::static_pointer_cast<jnivm::Object>(shared_from_this());
}

std::shared_ptr<jnivm::Object>
jnivm::com::google::android::ump::ConsentDebugSettings::Builder::addTestDeviceHashedId(
    std::shared_ptr<FakeJni::JString> id)
{
    BD_LOG("UMP", "ConsentDebugSettings$Builder.addTestDeviceHashedId(%s)",
           id ? id->asStdString().c_str() : "(null)");
    return std::static_pointer_cast<jnivm::Object>(shared_from_this());
}

std::shared_ptr<jnivm::com::google::android::ump::ConsentDebugSettings>
jnivm::com::google::android::ump::ConsentDebugSettings::Builder::build()
{
    BD_LOG("UMP", "ConsentDebugSettings$Builder.build() -> ConsentDebugSettings");
    return std::make_shared<ConsentDebugSettings>();
}

/// ConsentRequestParameters::Builder
jnivm::com::google::android::ump::ConsentRequestParameters::Builder::Builder()
{
    BD_LOG("UMP", "new ConsentRequestParameters$Builder()");
}

std::shared_ptr<jnivm::Object>
jnivm::com::google::android::ump::ConsentRequestParameters::Builder::setTagForUnderAgeOfConsent(
    FakeJni::JBoolean tag)
{
    BD_LOG("UMP", "ConsentRequestParameters$Builder.setTagForUnderAgeOfConsent(%d)", (int)tag);
    return std::static_pointer_cast<jnivm::Object>(shared_from_this());
}

std::shared_ptr<jnivm::Object>
jnivm::com::google::android::ump::ConsentRequestParameters::Builder::setConsentDebugSettings(
    std::shared_ptr<jnivm::Object> settings)
{
    BD_LOG("UMP", "ConsentRequestParameters$Builder.setConsentDebugSettings(%p)", settings.get());
    return std::static_pointer_cast<jnivm::Object>(shared_from_this());
}

std::shared_ptr<jnivm::com::google::android::ump::ConsentRequestParameters>
jnivm::com::google::android::ump::ConsentRequestParameters::Builder::build()
{
    BD_LOG("UMP", "ConsentRequestParameters$Builder.build() -> ConsentRequestParameters");
    return std::make_shared<ConsentRequestParameters>();
}

/// ConsentInformation
FakeJni::JInt jnivm::com::google::android::ump::ConsentInformation::getConsentStatus()
{
    // 1 == CONSENT_STATUS_NOT_REQUIRED. Offline port: there is no consent
    // manager to talk to, and "not required" is the only answer that lets the
    // ad SDK move on instead of opening a form it cannot render.
    BD_LOG("UMP", "ConsentInformation.getConsentStatus() -> 1 (NOT_REQUIRED)");
    return 1;
}

FakeJni::JBoolean jnivm::com::google::android::ump::ConsentInformation::canRequestAds()
{
    BD_LOG("UMP", "ConsentInformation.canRequestAds() -> true");
    return JNI_TRUE;
}

FakeJni::JBoolean jnivm::com::google::android::ump::ConsentInformation::isConsentFormAvailable()
{
    BD_LOG("UMP", "ConsentInformation.isConsentFormAvailable() -> false");
    return JNI_FALSE;
}

FakeJni::JInt jnivm::com::google::android::ump::ConsentInformation::getPrivacyOptionsRequirementStatus()
{
    // 2 == PRIVACY_OPTIONS_NOT_REQUIRED.
    BD_LOG("UMP", "ConsentInformation.getPrivacyOptionsRequirementStatus() -> 2 (NOT_REQUIRED)");
    return 2;
}

void jnivm::com::google::android::ump::ConsentInformation::requestConsentInfoUpdate(
    std::shared_ptr<jnivm::Object> activity, std::shared_ptr<jnivm::Object> params,
    std::shared_ptr<jnivm::Object> successListener, std::shared_ptr<jnivm::Object> failureListener)
{
    // Listener deliberately not invoked -- see the "Deliberately no callbacks"
    // note in ump.h. State is already "not required", so a caller that polls
    // getConsentStatus()/canRequestAds() gets a usable answer even without the
    // callback ever firing.
    BD_LOG("UMP",
           "ConsentInformation.requestConsentInfoUpdate(activity=%p, params=%p, ok=%p, fail=%p) -- no-op",
           activity.get(), params.get(), successListener.get(), failureListener.get());
}

void jnivm::com::google::android::ump::ConsentInformation::reset()
{
    BD_LOG("UMP", "ConsentInformation.reset() -- no-op");
}

/// ConsentForm
void jnivm::com::google::android::ump::ConsentForm::show(std::shared_ptr<jnivm::Object> activity,
    std::shared_ptr<jnivm::Object> listener)
{
    BD_LOG("UMP", "ConsentForm.show(activity=%p, listener=%p) -- no-op", activity.get(),
           listener.get());
}

/// UserMessagingPlatform
std::shared_ptr<jnivm::Object>
jnivm::com::google::android::ump::UserMessagingPlatform::getConsentInformation(
    std::shared_ptr<jnivm::com::unity3d::player::UnityPlayerActivity> activity)
{
    auto info = consent_information();
    BD_LOG("UMP",
           "UserMessagingPlatform.getConsentInformation(UnityPlayerActivity=%p) -> %p (erased sig)",
           activity.get(), info.get());
    return std::static_pointer_cast<jnivm::Object>(info);
}

std::shared_ptr<jnivm::com::google::android::ump::ConsentInformation>
jnivm::com::google::android::ump::UserMessagingPlatform::getConsentInformation(
    std::shared_ptr<jnivm::android::content::Context> context)
{
    auto info = consent_information();
    BD_LOG("UMP", "UserMessagingPlatform.getConsentInformation(Context=%p) -> %p", context.get(),
           info.get());
    return info;
}

void jnivm::com::google::android::ump::UserMessagingPlatform::requestConsentInfoUpdate(
    std::shared_ptr<jnivm::Object> activity, std::shared_ptr<jnivm::Object> params,
    std::shared_ptr<jnivm::Object> successListener, std::shared_ptr<jnivm::Object> failureListener)
{
    BD_LOG("UMP",
           "UserMessagingPlatform.requestConsentInfoUpdate(activity=%p, params=%p, ok=%p, fail=%p)"
           " -- no-op",
           activity.get(), params.get(), successListener.get(), failureListener.get());
}

void jnivm::com::google::android::ump::UserMessagingPlatform::requestConsentInfoUpdate(
    std::shared_ptr<jnivm::android::app::Activity> activity,
    std::shared_ptr<jnivm::com::google::android::ump::ConsentRequestParameters> params,
    std::shared_ptr<jnivm::Object> successListener, std::shared_ptr<jnivm::Object> failureListener)
{
    BD_LOG("UMP",
           "UserMessagingPlatform.requestConsentInfoUpdate(typed: activity=%p, params=%p, ok=%p,"
           " fail=%p) -- no-op",
           activity.get(), params.get(), successListener.get(), failureListener.get());
}

void jnivm::com::google::android::ump::UserMessagingPlatform::loadAndShowConsentFormIfRequired(
    std::shared_ptr<jnivm::Object> activity, std::shared_ptr<jnivm::Object> listener)
{
    // Nothing to show: isConsentFormAvailable() is false and the status is
    // NOT_REQUIRED. Dismissing is the correct answer, but the listener is a
    // managed proxy we do not call back into (see ump.h).
    BD_LOG("UMP",
           "UserMessagingPlatform.loadAndShowConsentFormIfRequired(activity=%p, listener=%p)"
           " -- no form, listener not invoked",
           activity.get(), listener.get());
}

void jnivm::com::google::android::ump::UserMessagingPlatform::showPrivacyOptionsForm(
    std::shared_ptr<jnivm::Object> activity, std::shared_ptr<jnivm::Object> listener)
{
    BD_LOG("UMP", "UserMessagingPlatform.showPrivacyOptionsForm(activity=%p, listener=%p) -- no-op",
           activity.get(), listener.get());
}

FakeJni::JBoolean jnivm::com::google::android::ump::UserMessagingPlatform::isPrivacyOptionsRequired()
{
    BD_LOG("UMP", "UserMessagingPlatform.isPrivacyOptionsRequired() -> false");
    return JNI_FALSE;
}

/// Descriptors
namespace jnivm {
namespace com {
    namespace google {
        namespace android {
            namespace ump {

                BEGIN_NATIVE_DESCRIPTOR(FormError) { FakeJni::Constructor<FormError> {} },
                    { FakeJni::Function<&FormError::getErrorCode> {}, "getErrorCode", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&FormError::getMessage> {}, "getMessage", FakeJni::JMethodID::PUBLIC },
                    END_NATIVE_DESCRIPTOR

                BEGIN_NATIVE_DESCRIPTOR(ConsentDebugSettings) { FakeJni::Constructor<ConsentDebugSettings> {} },
                    END_NATIVE_DESCRIPTOR

                BEGIN_NATIVE_DESCRIPTOR(ConsentDebugSettings::Builder) { FakeJni::Constructor<ConsentDebugSettings::Builder> {} },
                    { FakeJni::Constructor<ConsentDebugSettings::Builder, std::shared_ptr<jnivm::android::app::Activity>> {} },
                    { FakeJni::Function<&ConsentDebugSettings::Builder::setDebugGeography> {}, "setDebugGeography", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentDebugSettings::Builder::addTestDeviceHashedId> {}, "addTestDeviceHashedId", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentDebugSettings::Builder::build> {}, "build", FakeJni::JMethodID::PUBLIC },
                    END_NATIVE_DESCRIPTOR

                BEGIN_NATIVE_DESCRIPTOR(ConsentRequestParameters) { FakeJni::Constructor<ConsentRequestParameters> {} },
                    END_NATIVE_DESCRIPTOR

                BEGIN_NATIVE_DESCRIPTOR(ConsentRequestParameters::Builder) { FakeJni::Constructor<ConsentRequestParameters::Builder> {} },
                    { FakeJni::Function<&ConsentRequestParameters::Builder::setTagForUnderAgeOfConsent> {}, "setTagForUnderAgeOfConsent", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentRequestParameters::Builder::setConsentDebugSettings> {}, "setConsentDebugSettings", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentRequestParameters::Builder::build> {}, "build", FakeJni::JMethodID::PUBLIC },
                    END_NATIVE_DESCRIPTOR

                BEGIN_NATIVE_DESCRIPTOR(ConsentInformation) { FakeJni::Constructor<ConsentInformation> {} },
                    { FakeJni::Function<&ConsentInformation::getConsentStatus> {}, "getConsentStatus", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentInformation::canRequestAds> {}, "canRequestAds", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentInformation::isConsentFormAvailable> {}, "isConsentFormAvailable", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentInformation::getPrivacyOptionsRequirementStatus> {}, "getPrivacyOptionsRequirementStatus", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentInformation::requestConsentInfoUpdate> {}, "requestConsentInfoUpdate", FakeJni::JMethodID::PUBLIC },
                    { FakeJni::Function<&ConsentInformation::reset> {}, "reset", FakeJni::JMethodID::PUBLIC },
                    END_NATIVE_DESCRIPTOR

                BEGIN_NATIVE_DESCRIPTOR(ConsentForm) { FakeJni::Constructor<ConsentForm> {} },
                    { FakeJni::Function<&ConsentForm::show> {}, "show", FakeJni::JMethodID::PUBLIC },
                    END_NATIVE_DESCRIPTOR

                // The two getConsentInformation overloads differ only in their C++
                // parameter type, which is exactly what jnivm keys on.
                using UmpGetInfoErased = std::shared_ptr<jnivm::Object> (*)(
                    std::shared_ptr<jnivm::com::unity3d::player::UnityPlayerActivity>);
                using UmpGetInfoTyped = std::shared_ptr<ConsentInformation> (*)(
                    std::shared_ptr<jnivm::android::content::Context>);
                using UmpRequestErased = void (*)(std::shared_ptr<jnivm::Object>,
                    std::shared_ptr<jnivm::Object>, std::shared_ptr<jnivm::Object>,
                    std::shared_ptr<jnivm::Object>);
                using UmpRequestTyped = void (*)(std::shared_ptr<jnivm::android::app::Activity>,
                    std::shared_ptr<ConsentRequestParameters>, std::shared_ptr<jnivm::Object>,
                    std::shared_ptr<jnivm::Object>);

                BEGIN_NATIVE_DESCRIPTOR(UserMessagingPlatform) { FakeJni::Constructor<UserMessagingPlatform> {} },
                    { FakeJni::Function<static_cast<UmpGetInfoErased>(&UserMessagingPlatform::getConsentInformation)> {}, "getConsentInformation", FakeJni::JMethodID::STATIC },
                    { FakeJni::Function<static_cast<UmpGetInfoTyped>(&UserMessagingPlatform::getConsentInformation)> {}, "getConsentInformation", FakeJni::JMethodID::STATIC },
                    { FakeJni::Function<static_cast<UmpRequestErased>(&UserMessagingPlatform::requestConsentInfoUpdate)> {}, "requestConsentInfoUpdate", FakeJni::JMethodID::STATIC },
                    { FakeJni::Function<static_cast<UmpRequestTyped>(&UserMessagingPlatform::requestConsentInfoUpdate)> {}, "requestConsentInfoUpdate", FakeJni::JMethodID::STATIC },
                    { FakeJni::Function<&UserMessagingPlatform::loadAndShowConsentFormIfRequired> {}, "loadAndShowConsentFormIfRequired", FakeJni::JMethodID::STATIC },
                    { FakeJni::Function<&UserMessagingPlatform::showPrivacyOptionsForm> {}, "showPrivacyOptionsForm", FakeJni::JMethodID::STATIC },
                    { FakeJni::Function<&UserMessagingPlatform::isPrivacyOptionsRequired> {}, "isPrivacyOptionsRequired", FakeJni::JMethodID::STATIC },
                    END_NATIVE_DESCRIPTOR
            }
        }
    }
}
}
