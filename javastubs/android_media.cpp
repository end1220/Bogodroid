#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "javac.h"
#include "logging.h"
#include <fstream>
#include <pthread.h>
#include <inttypes.h>

///// AudioDeviceInfo / AudioManager / etc.
//
// Trivial constant-return methods (AudioDeviceInfo::getType → 4,
// AudioManager::isBluetoothA2dpOn → false, AudioManager::getStreamVolume
// → 100) live in projects/unityloader/javastubs/binding.cpp under the
// "Trivial default returns" block via vm->setDefault(...) and don't need
// a C++ body here. Anything below has real logic.

///// AudioManager

std::shared_ptr<FakeJni::JString> jnivm::android::media::AudioManager::getProperty(std::shared_ptr<FakeJni::JString> property)
{
    if (*property == PROPERTY_OUTPUT_FRAMES_PER_BUFFER)
        return std::make_shared<FakeJni::JString>("64"); // ... uh i dunno i haven't written the audio implementation yet
    if (*property == PROPERTY_OUTPUT_SAMPLE_RATE)
        return std::make_shared<FakeJni::JString>("24000"); // ... uhhh sure
    return nullptr;
}

std::shared_ptr<jnivm::Array<jnivm::android::media::AudioDeviceInfo>> jnivm::android::media::AudioManager::getDevices(int type)
{
    auto array = std::make_shared<FakeJni::JArray<jnivm::android::media::AudioDeviceInfo>>(1);
    (*array)[0] = std::make_shared<jnivm::android::media::AudioDeviceInfo>();
    return array;
}

///// MediaRoute$RouteInfo

std::shared_ptr<jnivm::android::view::Display> jnivm::android::media::MediaRouterRouteInfo::getPresentationDisplay()
{
    return std::make_shared<jnivm::android::view::Display>();
}

///// MediaRouter

std::shared_ptr<jnivm::android::media::MediaRouterRouteInfo> jnivm::android::media::MediaRouter::getSelectedRoute(int type)
{
    return std::make_shared<jnivm::android::media::MediaRouterRouteInfo>();
}

///// MediaExtractor / MediaFormat / MediaCodec — factory-stubbed.
// See android_descriptors.cpp for the factory registry.

///// Media Descriptors

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::AudioDeviceInfo) { FakeJni::Constructor<AudioDeviceInfo> {} },
    { FakeJni::Field<&AudioDeviceInfo::TYPE_BLUETOOTH_A2DP> {}, "TYPE_BLUETOOTH_A2DP", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&AudioDeviceInfo::TYPE_WIRED_HEADPHONES> {}, "TYPE_WIRED_HEADPHONES", FakeJni::JFieldID::STATIC },
    // getType -> see binding.cpp setDefault entry.
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::AudioManager) { FakeJni::Constructor<AudioManager> {} },
    { FakeJni::Field<&AudioManager::PROPERTY_OUTPUT_FRAMES_PER_BUFFER> {}, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&AudioManager::PROPERTY_OUTPUT_SAMPLE_RATE> {}, "PROPERTY_OUTPUT_SAMPLE_RATE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&AudioManager::GET_DEVICES_OUTPUTS> {}, "GET_DEVICES_OUTPUTS", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&AudioManager::STREAM_MUSIC> {}, "STREAM_MUSIC", FakeJni::JFieldID::STATIC },
    // isBluetoothA2dpOn / getStreamVolume -> see binding.cpp setDefault entries.
    { FakeJni::Function<&AudioManager::getProperty> {}, "getProperty", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AudioManager::getDevices> {}, "getDevices", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::MediaRouterRouteInfo) { FakeJni::Constructor<MediaRouterRouteInfo> {} },
    { FakeJni::Function<&MediaRouterRouteInfo::getPresentationDisplay> {}, "getPresentationDisplay", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::MediaRouter) { FakeJni::Constructor<MediaRouter> {} },
    { FakeJni::Field<&MediaRouter::ROUTE_TYPE_LIVE_VIDEO> {}, "ROUTE_TYPE_LIVE_VIDEO", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&MediaRouter::getSelectedRoute> {}, "getSelectedRoute", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::MediaExtractor)
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::MediaFormat)
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::media::MediaCodec)
    END_NATIVE_DESCRIPTOR