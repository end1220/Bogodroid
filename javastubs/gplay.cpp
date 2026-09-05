#include "gplay.h"
#include "logging.h"
#include "toml++/toml.hpp"

extern toml::table config;

using namespace jnivm::com::google::android::vending::licensing;

AESObfuscator::AESObfuscator(std::shared_ptr<FakeJni::JByteArray>,
                             std::shared_ptr<FakeJni::JString>,
                             std::shared_ptr<FakeJni::JString>) {}

ServerManagedPolicy::ServerManagedPolicy(
    std::shared_ptr<jnivm::android::content::Context>,
    std::shared_ptr<Obfuscator>) {}

APKExpansionPolicy::APKExpansionPolicy(
    std::shared_ptr<jnivm::android::content::Context>,
    std::shared_ptr<Obfuscator>) {}

LicenseChecker::LicenseChecker(
    std::shared_ptr<jnivm::android::content::Context>,
    std::shared_ptr<Policy>,
    std::shared_ptr<FakeJni::JString>) {}

void LicenseChecker::checkAccess(std::shared_ptr<LicenseCheckerCallback> callback)
{
    if (!callback) {
        BD_LOG("GPLAY", "LVL checkAccess received a null callback");
        return;
    }

    if (config["google_play"]["offline"].value_or<bool>(false)) {
        BD_LOG("GPLAY", "offline LVL entitlement enabled; returning LICENSED");
        callback->allow(Policy::LICENSED);
        return;
    }

    BD_LOG("GPLAY", "Google Play unavailable and offline entitlement disabled");
    callback->applicationError(3); // ERROR_NOT_MARKET_MANAGED
}

void LicenseChecker::onDestroy() {}

jnivm::com::google::android::play::core::tasks::Task::Task(std::shared_ptr<FakeJni::JObject> res)
{
    this->result = res;
}

std::shared_ptr<jnivm::com::google::android::play::core::tasks::Task> jnivm::com::google::android::play::core::tasks::Task::addOnCompleteListener(std::shared_ptr<jnivm::com::google::android::play::core::tasks::OnCompleteListener> listener)
{
    this->onCompleteListener = listener;
    return std::dynamic_pointer_cast<Task>(shared_from_this());
}

std::shared_ptr<jnivm::com::google::android::play::core::tasks::Task> jnivm::com::google::android::play::core::tasks::Task::addOnSuccessListener(std::shared_ptr<jnivm::com::google::android::play::core::tasks::OnSuccessListener> listener)
{
    this->onSuccessListener = listener;
    return std::dynamic_pointer_cast<Task>(shared_from_this());
}

std::shared_ptr<jnivm::com::google::android::play::core::tasks::Task> jnivm::com::google::android::play::core::tasks::Task::addOnFailureListener(std::shared_ptr<jnivm::com::google::android::play::core::tasks::OnFailureListener> listener)
{
    this->onFailureListener = listener;
    return std::dynamic_pointer_cast<Task>(shared_from_this());
}



std::shared_ptr<jnivm::com::google::android::play::core::review::ReviewManager> jnivm::com::google::android::play::core::review::ReviewManagerFactory::create(std::shared_ptr<jnivm::android::content::Context> context)
{
    return std::make_shared<jnivm::com::google::android::play::core::review::ReviewManager>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::AchievementsClient> jnivm::com::google::android::gms::games::PlayGames::getAchievementsClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::AchievementsClient>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::EventsClient> jnivm::com::google::android::gms::games::PlayGames::getEventsClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::EventsClient>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::GamesSignInClient> jnivm::com::google::android::gms::games::PlayGames::getGamesSignInClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::GamesSignInClient>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::LeaderboardsClient> jnivm::com::google::android::gms::games::PlayGames::getLeaderboardsClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::LeaderboardsClient>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::PlayerStatsClient> jnivm::com::google::android::gms::games::PlayGames::getPlayerStatsClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::PlayerStatsClient>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::PlayersClient> jnivm::com::google::android::gms::games::PlayGames::getPlayersClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::PlayersClient>();
}

std::shared_ptr<jnivm::com::google::android::gms::games::SnapshotsClient> jnivm::com::google::android::gms::games::PlayGames::getSnapshotsClient(std::shared_ptr<jnivm::android::app::Activity> activity)
{
    return std::make_shared<jnivm::com::google::android::gms::games::SnapshotsClient>();
}

BEGIN_NATIVE_DESCRIPTOR(Obfuscator)
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AESObfuscator) {
    FakeJni::Constructor<AESObfuscator, std::shared_ptr<FakeJni::JByteArray>,
                         std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>> {}
},
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(Policy)
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ServerManagedPolicy) {
    FakeJni::Constructor<ServerManagedPolicy,
                         std::shared_ptr<jnivm::android::content::Context>,
                         std::shared_ptr<Obfuscator>> {}
},
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(APKExpansionPolicy) {
    FakeJni::Constructor<APKExpansionPolicy,
                         std::shared_ptr<jnivm::android::content::Context>,
                         std::shared_ptr<Obfuscator>> {}
},
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LicenseCheckerCallback)
    { FakeJni::Function<&LicenseCheckerCallback::allow> {}, "allow", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&LicenseCheckerCallback::dontAllow> {}, "dontAllow", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&LicenseCheckerCallback::applicationError> {}, "applicationError", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LicenseChecker) {
    FakeJni::Constructor<LicenseChecker,
                         std::shared_ptr<jnivm::android::content::Context>,
                         std::shared_ptr<Policy>, std::shared_ptr<FakeJni::JString>> {}
},
    { FakeJni::Function<&LicenseChecker::checkAccess> {}, "checkAccess", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&LicenseChecker::onDestroy> {}, "onDestroy", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

void jnivm::com::google::android::gms::games::PlayGamesSdk::initialize(std::shared_ptr<jnivm::android::content::Context> context)
{
    // Initialize the Play Games SDK with the provided context
}

std::shared_ptr<jnivm::com::google::android::play::core::tasks::Task> jnivm::com::google::android::gms::games::GamesSignInClient::isAuthenticated()
{
    return std::make_shared<jnivm::com::google::android::play::core::tasks::Task>(std::make_shared<jnivm::com::google::android::gms::games::AuthenticationResult>());
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::play::core::review::ReviewManager) { FakeJni::Constructor<ReviewManager> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::play::core::review::ReviewManagerFactory) { FakeJni::Constructor<ReviewManagerFactory> {} },
    { FakeJni::Function<&ReviewManagerFactory::create> {}, "create", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::play::core::tasks::Task) { FakeJni::Constructor<Task, std::shared_ptr<FakeJni::JObject>> {} },
    { FakeJni::Function<&Task::addOnCompleteListener> {}, "addOnCompleteListener", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Task::addOnSuccessListener> {}, "addOnSuccessListener", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Task::addOnFailureListener> {}, "addOnFailureListener", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::play::core::tasks::OnCompleteListener) { FakeJni::Constructor<OnCompleteListener> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::play::core::tasks::OnSuccessListener) { FakeJni::Constructor<OnSuccessListener> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::play::core::tasks::OnFailureListener) { FakeJni::Constructor<OnFailureListener> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::AchievementsClient) { FakeJni::Constructor<AchievementsClient> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::EventsClient) { FakeJni::Constructor<EventsClient> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::AuthenticationResult) { FakeJni::Constructor<AuthenticationResult> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::GamesSignInClient) { FakeJni::Constructor<GamesSignInClient> {} },
    { FakeJni::Function<&GamesSignInClient::isAuthenticated> {}, "isAuthenticated", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::LeaderboardsClient) { FakeJni::Constructor<LeaderboardsClient> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::PlayerStatsClient) { FakeJni::Constructor<PlayerStatsClient> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::PlayersClient) { FakeJni::Constructor<PlayersClient> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::SnapshotsClient) { FakeJni::Constructor<SnapshotsClient> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::PlayGames) {
        FakeJni::Constructor<PlayGames> {},
    },
    { FakeJni::Function<&PlayGames::getAchievementsClient> {}, "getAchievementsClient", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayGames::getEventsClient> {}, "getEventsClient", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayGames::getGamesSignInClient> {}, "getGamesSignInClient", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayGames::getLeaderboardsClient> {}, "getLeaderboardsClient", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayGames::getPlayerStatsClient> {}, "getPlayerStatsClient", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayGames::getPlayersClient> {}, "getPlayersClient", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&PlayGames::getSnapshotsClient> {}, "getSnapshotsClient", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::com::google::android::gms::games::PlayGamesSdk) {
        FakeJni::Constructor<PlayGamesSdk> {},
    },
    { FakeJni::Function<&PlayGamesSdk::initialize> {}, "initialize", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

void InitJNIGooglePlayClasses(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Initializing Play Games JNI Classes");
    vm->registerClass<Obfuscator>();
    vm->registerClass<AESObfuscator>();
    vm->registerClass<Policy>();
    vm->registerClass<ServerManagedPolicy>();
    vm->registerClass<APKExpansionPolicy>();
    vm->registerClass<LicenseCheckerCallback>();
    vm->registerClass<LicenseChecker>();
    vm->registerClass<jnivm::com::google::android::play::core::review::ReviewManager>();
    vm->registerClass<jnivm::com::google::android::play::core::review::ReviewManagerFactory>();

    vm->registerClass<jnivm::com::google::android::play::core::tasks::Task>();
    vm->registerClass<jnivm::com::google::android::play::core::tasks::OnCompleteListener>();
    vm->registerClass<jnivm::com::google::android::play::core::tasks::OnSuccessListener>();
    vm->registerClass<jnivm::com::google::android::play::core::tasks::OnFailureListener>();

    vm->registerClass<jnivm::com::google::android::gms::games::AchievementsClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::EventsClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::AuthenticationResult>();
    vm->registerClass<jnivm::com::google::android::gms::games::GamesSignInClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::LeaderboardsClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::PlayerStatsClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::PlayersClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::SnapshotsClient>();
    vm->registerClass<jnivm::com::google::android::gms::games::PlayGames>();
    vm->registerClass<jnivm::com::google::android::gms::games::PlayGamesSdk>();
}
