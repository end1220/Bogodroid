#ifndef __PLAYGAMES_H__
#define __PLAYGAMES_H__

#include "android.h"
#include "baron/baron.h"
#include "javac.h"

void InitJNIGooglePlayClasses(FakeJni::Jvm* vm);

namespace jnivm {
namespace com {
    namespace google {
        namespace android {
            namespace vending {
                namespace licensing {
                    class Obfuscator : public virtual FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/Obfuscator")
                    };

                    class AESObfuscator : public Obfuscator {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/AESObfuscator", Obfuscator)
                        AESObfuscator(std::shared_ptr<FakeJni::JByteArray> salt,
                                      std::shared_ptr<FakeJni::JString> packageName,
                                      std::shared_ptr<FakeJni::JString> deviceId);
                    };

                    class Policy : public virtual FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/Policy")
                        static constexpr jint LICENSED = 0x0100;
                    };

                    class ServerManagedPolicy : public Policy {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/ServerManagedPolicy", Policy)
                        ServerManagedPolicy(std::shared_ptr<jnivm::android::content::Context> context,
                                            std::shared_ptr<Obfuscator> obfuscator);
                    };

                    class APKExpansionPolicy : public Policy {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/APKExpansionPolicy", Policy)
                        APKExpansionPolicy(std::shared_ptr<jnivm::android::content::Context> context,
                                           std::shared_ptr<Obfuscator> obfuscator);
                    };

                    class LicenseCheckerCallback : public virtual FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/LicenseCheckerCallback")
                        virtual void allow(jint reason) = 0;
                        virtual void dontAllow(jint reason) = 0;
                        virtual void applicationError(jint errorCode) = 0;
                    };

                    class LicenseChecker : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/vending/licensing/LicenseChecker")
                        LicenseChecker(std::shared_ptr<jnivm::android::content::Context> context,
                                       std::shared_ptr<Policy> policy,
                                       std::shared_ptr<FakeJni::JString> publicKey);
                        void checkAccess(std::shared_ptr<LicenseCheckerCallback> callback);
                        void onDestroy();
                    };
                }
            }
        }

        namespace android {
            namespace play {
                namespace core {
                    namespace review {
                        class ReviewManager : public FakeJni::JObject {
                        public:
                            DEFINE_CLASS_NAME("com/google/android/play/core/review/ReviewManager")
                        };
                        class ReviewManagerFactory : public FakeJni::JObject {
                        public:
                            DEFINE_CLASS_NAME("com/google/android/play/core/review/ReviewManagerFactory")
                            static std::shared_ptr<ReviewManager> create(std::shared_ptr<jnivm::android::content::Context> context);
                        };
                    }
                    namespace tasks {
                        class OnCompleteListener : public FakeJni::JObject {
                        public:
                            DEFINE_CLASS_NAME("com/google/android/play/core/tasks/OnCompleteListener")
                        };

                        class OnSuccessListener : public FakeJni::JObject {
                        public:
                            DEFINE_CLASS_NAME("com/google/android/play/core/tasks/OnSuccessListener")
                        };

                        class OnFailureListener : public FakeJni::JObject {
                        public:
                            DEFINE_CLASS_NAME("com/google/android/play/core/tasks/OnFailureListener")
                        };

                        class Task : public FakeJni::JObject {
                        public:
                            DEFINE_CLASS_NAME("com/google/android/play/core/tasks/Task")
                            Task(std::shared_ptr<FakeJni::JObject> result);
                            std::shared_ptr<FakeJni::JObject> result = nullptr;
                            std::shared_ptr<OnCompleteListener> onCompleteListener = nullptr;
                            std::shared_ptr<OnSuccessListener> onSuccessListener = nullptr;
                            std::shared_ptr<OnFailureListener> onFailureListener = nullptr;

                            std::shared_ptr<Task> addOnCompleteListener(std::shared_ptr<OnCompleteListener> listener);
                            std::shared_ptr<Task> addOnSuccessListener(std::shared_ptr<OnSuccessListener> listener);
                            std::shared_ptr<Task> addOnFailureListener(std::shared_ptr<OnFailureListener> listener);
                        };

                    }
                }
            }

            namespace gms {
                namespace games {
                    class AchievementsClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/AchievementsClient")
                    };

                    class EventsClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/EventsClient")
                    };

                    class AuthenticationResult : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/auth/api/signin/AuthenticationResult")
                    };

                    class GamesSignInClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/GamesSignInClient")
                        std::shared_ptr<jnivm::com::google::android::play::core::tasks::Task> isAuthenticated();
                    };

                    class LeaderboardsClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/LeaderboardsClient")
                    };

                    class PlayerStatsClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/PlayerStatsClient")
                    };

                    class PlayersClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/PlayersClient")
                    };

                    class SnapshotsClient : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/SnapshotsClient")
                    };

                    class PlayGames : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/PlayGames")
                        static std::shared_ptr<jnivm::com::google::android::gms::games::AchievementsClient> getAchievementsClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                        static std::shared_ptr<jnivm::com::google::android::gms::games::EventsClient> getEventsClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                        static std::shared_ptr<jnivm::com::google::android::gms::games::GamesSignInClient> getGamesSignInClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                        static std::shared_ptr<jnivm::com::google::android::gms::games::LeaderboardsClient> getLeaderboardsClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                        static std::shared_ptr<jnivm::com::google::android::gms::games::PlayerStatsClient> getPlayerStatsClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                        static std::shared_ptr<jnivm::com::google::android::gms::games::PlayersClient> getPlayersClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                        static std::shared_ptr<jnivm::com::google::android::gms::games::SnapshotsClient> getSnapshotsClient(std::shared_ptr<jnivm::android::app::Activity> activity);
                    };

                    class PlayGamesSdk : public FakeJni::JObject {
                    public:
                        DEFINE_CLASS_NAME("com/google/android/gms/games/PlayGamesSdk")
                        static void initialize(std::shared_ptr<jnivm::android::content::Context> context);
                    };
                }
            }
        }
    }
}
}

#endif
