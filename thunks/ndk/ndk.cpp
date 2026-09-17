
#include "toml++/toml.hpp"
extern toml::table config;
#include "ndk.h"
#include "device_display.h"
#include "logging.h"
#include "thunk_gen.h"
#include "platform.h"
#include "so_util.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <poll.h>
#include "alooper.h"
#include "asset_manager.h"
#include "anative_activity.h"
#include "media.h"

extern "C" {
extern const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT;
extern const char* AMEDIAFORMAT_KEY_COLOR_FORMAT;
extern const char* AMEDIAFORMAT_KEY_COLOR_RANGE;
extern const char* AMEDIAFORMAT_KEY_COLOR_STANDARD;
extern const char* AMEDIAFORMAT_KEY_DURATION;
extern const char* AMEDIAFORMAT_KEY_ENCODER_DELAY;
extern const char* AMEDIAFORMAT_KEY_FRAME_RATE;
extern const char* AMEDIAFORMAT_KEY_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_LANGUAGE;
extern const char* AMEDIAFORMAT_KEY_MIME;
extern const char* AMEDIAFORMAT_KEY_ROTATION;
extern const char* AMEDIAFORMAT_KEY_SAMPLE_RATE;
extern const char* AMEDIAFORMAT_KEY_SLICE_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_STRIDE;
extern const char* AMEDIAFORMAT_KEY_WIDTH;
}

static ANativeWindow* default_native_window;

ABI_ATTR AConfiguration *AConfiguration_new()
{
    AConfiguration *config = new AConfiguration;
    memset(config, 0, sizeof(AConfiguration));
    return config;
}

ABI_ATTR void AConfiguration_fromAssetManager(AConfiguration *out, void *am)
{
}

ABI_ATTR int32_t AConfiguration_getMcc(AConfiguration *aconfig)
{
    return 0;
}
ABI_ATTR int32_t AConfiguration_getMnc(AConfiguration *aconfig)
{
    return 0;
}
ABI_ATTR void AConfiguration_getLanguage(AConfiguration *aconfig, char *outLanguage)
{
    const char *lang = config["device"]["language"].value_or("en");
    outLanguage[0] = lang[0];
    outLanguage[1] = lang[1];
}
ABI_ATTR void AConfiguration_getCountry(AConfiguration *aconfig, char *outCountry)
{
    const char *country = config["device"]["country"].value_or("US");
    outCountry[0] = country[0];
    outCountry[1] = country[1];
}
ABI_ATTR int32_t AConfiguration_getOrientation(AConfiguration *aconfig)
{
    return config["device"]["displayRotation"].value_or<int>(2); // LAND
}
ABI_ATTR int32_t AConfiguration_getTouchscreen(AConfiguration *aconfig)
{
    return config["device"]["displayTouchscreen"].value_or<int>(1); // NOTOUCH
}
ABI_ATTR int32_t AConfiguration_getDensity(AConfiguration *aconfig)
{
    return config["device"]["displayDensity"].value_or<int>(160); // MEDIUM
}
ABI_ATTR int32_t AConfiguration_getKeyboard(AConfiguration *aconfig)
{
    return config["device"]["keyboard"].value_or<int>(2); // QWERTY
}
ABI_ATTR int32_t AConfiguration_getNavigation(AConfiguration *aconfig)
{
    return 0;
}
ABI_ATTR int32_t AConfiguration_getKeysHidden(AConfiguration *aconfig)
{
    return 0;
}
ABI_ATTR int32_t AConfiguration_getNavHidden(AConfiguration *aconfig)
{
    return 0;
}
ABI_ATTR int32_t AConfiguration_getSdkVersion(AConfiguration *aconfig)
{
    // Match Build.VERSION.SDK_INT. Compressed AssetBundle VideoClips need
    // API 29+ so Unity uses AMediaDataSource instead of archive→file remap.
    return 29;
}
ABI_ATTR int32_t AConfiguration_getScreenSize(AConfiguration *aconfig)
{
    return 2;
}
ABI_ATTR int32_t AConfiguration_getScreenLong(AConfiguration *aconfig)
{
    return 1;
}
ABI_ATTR int32_t AConfiguration_getUiModeType(AConfiguration *aconfig)
{
    return 0;
}
ABI_ATTR int32_t AConfiguration_getUiModeNight(AConfiguration *aconfig)
{
    return 0;
}

ABI_ATTR int32_t ret0()
{
    return 0;
}

ABI_ATTR ANativeWindow* ANativeWindow_fromSurface(void*, void*)
{
    if(default_native_window==NULL)
    {
        default_native_window = (ANativeWindow*)malloc(sizeof(ANativeWindow));
    }
    return default_native_window;
}

ABI_ATTR jobject ANativeWindow_toSurface(JNIEnv* env, ANativeWindow*)
{
    if (!env)
        return nullptr;
    jclass surface_class = env->FindClass("android/view/Surface");
    if (!surface_class)
        return nullptr;
    jmethodID constructor = env->GetMethodID(surface_class, "<init>", "()V");
    if (!constructor)
        return nullptr;
    jobject surface = env->NewObject(surface_class, constructor);
    BD_LOG("NDK", "ANativeWindow_toSurface -> %p", (void*)surface);
    return surface;
}

ABI_ATTR int32_t ANativeWindow_getWidth(ANativeWindow *window)
{
    int32_t width = bd_device_display_width();
    static bool logged = false;
    if (!logged) {
        logged = true;
        BD_LOG("NDK", "ANativeWindow_getWidth(%p) -> %d", (void*)window, width);
    }
    return width;
}

ABI_ATTR int32_t ANativeWindow_getHeight(ANativeWindow *window)
{
    int32_t height = bd_device_display_height();
    static bool logged = false;
    if (!logged) {
        logged = true;
        BD_LOG("NDK", "ANativeWindow_getHeight(%p) -> %d", (void*)window, height);
    }
    return height;
}

ABI_ATTR void __assert2(const char* __file, int __line, const char* __function, const char* __msg)
{
    fatal_error("ASSERT!! File: %s Line: %d Function: %s Message: %s\n",__file, __line, __function, __msg);
    exit(1);
}

DynLibFunction symtable_ndk[] = {
NO_THUNK("AConfiguration_new", (uintptr_t)&AConfiguration_new),
NO_THUNK("AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager),
NO_THUNK("AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage),
NO_THUNK("AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry),
NO_THUNK("AConfiguration_getMcc", (uintptr_t)&AConfiguration_getMcc),
NO_THUNK("AConfiguration_getMnc", (uintptr_t)&AConfiguration_getMnc),
NO_THUNK("AConfiguration_getOrientation", (uintptr_t)&AConfiguration_getOrientation),
NO_THUNK("AConfiguration_getTouchscreen", (uintptr_t)&AConfiguration_getTouchscreen),
NO_THUNK("AConfiguration_getDensity", (uintptr_t)&AConfiguration_getDensity),
NO_THUNK("AConfiguration_getKeyboard", (uintptr_t)&AConfiguration_getKeyboard),
NO_THUNK("AConfiguration_getNavigation", (uintptr_t)&AConfiguration_getNavigation),
NO_THUNK("AConfiguration_getKeysHidden", (uintptr_t)&AConfiguration_getKeysHidden),
NO_THUNK("AConfiguration_getNavHidden", (uintptr_t)&AConfiguration_getNavHidden),
NO_THUNK("AConfiguration_getSdkVersion", (uintptr_t)&AConfiguration_getSdkVersion),
NO_THUNK("AConfiguration_getScreenSize", (uintptr_t)&AConfiguration_getScreenSize),
NO_THUNK("AConfiguration_getScreenLong", (uintptr_t)&AConfiguration_getScreenLong),
NO_THUNK("AConfiguration_getUiModeType", (uintptr_t)&AConfiguration_getUiModeType),
NO_THUNK("AConfiguration_getUiModeNight", (uintptr_t)&AConfiguration_getUiModeNight),
NO_THUNK("ASensorManager_getInstanceForPackage", (uintptr_t)&ret0), //AWFUL
NO_THUNK("ASensorManager_getInstance", (uintptr_t)&ret0), //AWFUL
NO_THUNK("ASensorManager_getDefaultSensor", (uintptr_t)&ret0), //AWFUL
NO_THUNK("ASensorManager_createEventQueue", (uintptr_t)&ret0), //AWFUL
    NO_THUNK("ALooper_prepare", (uintptr_t)&ALooper_prepare),
    NO_THUNK("ALooper_forThread", (uintptr_t)&ALooper_forThread),
    NO_THUNK("ALooper_acquire", (uintptr_t)&ALooper_acquire),
    NO_THUNK("ALooper_release", (uintptr_t)&ALooper_release),
    NO_THUNK("ALooper_addFd", (uintptr_t)&ALooper_addFd),
    NO_THUNK("ALooper_removeFd", (uintptr_t)&ALooper_removeFd),
    NO_THUNK("ALooper_wake", (uintptr_t)&ALooper_wake),
    NO_THUNK("ALooper_pollAll", (uintptr_t)&ALooper_pollAll),
    NO_THUNK("ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce),
NO_THUNK("ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface), //AWFUL
NO_THUNK("ANativeWindow_toSurface", (uintptr_t)&ANativeWindow_toSurface),
NO_THUNK("ANativeWindow_acquire", (uintptr_t)&ret0), //AWFUL
NO_THUNK("ANativeWindow_release", (uintptr_t)&ret0), //AWFUL
NO_THUNK("ANativeWindow_setBuffersGeometry", (uintptr_t)&ret0), //AWFUL
NO_THUNK("ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth),
NO_THUNK("ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight),
NO_THUNK("__assert2", (uintptr_t)&__assert2),
NO_THUNK("AAssetManager_open",(uintptr_t)&AAssetManager_open),
NO_THUNK("AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava),
NO_THUNK("AAsset_getBuffer",(uintptr_t)&AAsset_getBuffer),
NO_THUNK("AAsset_getLength",(uintptr_t)&AAsset_getLength),
NO_THUNK("AAsset_close",(uintptr_t)&AAsset_close),
NO_THUNK("AAsset_read", (uintptr_t)&AAsset_read),
NO_THUNK("AMEDIAFORMAT_KEY_CHANNEL_COUNT", (uintptr_t)&AMEDIAFORMAT_KEY_CHANNEL_COUNT),
NO_THUNK("AMEDIAFORMAT_KEY_COLOR_FORMAT", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_FORMAT),
NO_THUNK("AMEDIAFORMAT_KEY_COLOR_RANGE", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_RANGE),
NO_THUNK("AMEDIAFORMAT_KEY_COLOR_STANDARD", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_STANDARD),
NO_THUNK("AMEDIAFORMAT_KEY_DURATION", (uintptr_t)&AMEDIAFORMAT_KEY_DURATION),
NO_THUNK("AMEDIAFORMAT_KEY_ENCODER_DELAY", (uintptr_t)&AMEDIAFORMAT_KEY_ENCODER_DELAY),
NO_THUNK("AMEDIAFORMAT_KEY_FRAME_RATE", (uintptr_t)&AMEDIAFORMAT_KEY_FRAME_RATE),
NO_THUNK("AMEDIAFORMAT_KEY_HEIGHT", (uintptr_t)&AMEDIAFORMAT_KEY_HEIGHT),
NO_THUNK("AMEDIAFORMAT_KEY_LANGUAGE", (uintptr_t)&AMEDIAFORMAT_KEY_LANGUAGE),
NO_THUNK("AMEDIAFORMAT_KEY_MIME", (uintptr_t)&AMEDIAFORMAT_KEY_MIME),
NO_THUNK("AMEDIAFORMAT_KEY_ROTATION", (uintptr_t)&AMEDIAFORMAT_KEY_ROTATION),
NO_THUNK("AMEDIAFORMAT_KEY_SAMPLE_RATE", (uintptr_t)&AMEDIAFORMAT_KEY_SAMPLE_RATE),
NO_THUNK("AMEDIAFORMAT_KEY_SLICE_HEIGHT", (uintptr_t)&AMEDIAFORMAT_KEY_SLICE_HEIGHT),
NO_THUNK("AMEDIAFORMAT_KEY_STRIDE", (uintptr_t)&AMEDIAFORMAT_KEY_STRIDE),
NO_THUNK("AMEDIAFORMAT_KEY_WIDTH", (uintptr_t)&AMEDIAFORMAT_KEY_WIDTH),
NO_THUNK("AMediaDataSource_new", (uintptr_t)&AMediaDataSource_new),
NO_THUNK("AMediaDataSource_delete", (uintptr_t)&AMediaDataSource_delete),
NO_THUNK("AMediaDataSource_setUserdata", (uintptr_t)&AMediaDataSource_setUserdata),
NO_THUNK("AMediaDataSource_setReadAt", (uintptr_t)&AMediaDataSource_setReadAt),
NO_THUNK("AMediaDataSource_setGetSize", (uintptr_t)&AMediaDataSource_setGetSize),
NO_THUNK("AMediaDataSource_setClose", (uintptr_t)&AMediaDataSource_setClose),
NO_THUNK("AMediaExtractor_new", (uintptr_t)&AMediaExtractor_new),
NO_THUNK("AMediaExtractor_delete", (uintptr_t)&AMediaExtractor_delete),
NO_THUNK("AMediaExtractor_setDataSource", (uintptr_t)&AMediaExtractor_setDataSource),
NO_THUNK("AMediaExtractor_setDataSourceFd", (uintptr_t)&AMediaExtractor_setDataSourceFd),
NO_THUNK("AMediaExtractor_setDataSourceCustom", (uintptr_t)&AMediaExtractor_setDataSourceCustom),
NO_THUNK("AMediaExtractor_getTrackCount", (uintptr_t)&AMediaExtractor_getTrackCount),
NO_THUNK("AMediaExtractor_getTrackFormat", (uintptr_t)&AMediaExtractor_getTrackFormat),
NO_THUNK("AMediaExtractor_selectTrack", (uintptr_t)&AMediaExtractor_selectTrack),
NO_THUNK("AMediaExtractor_getSampleTrackIndex", (uintptr_t)&AMediaExtractor_getSampleTrackIndex),
NO_THUNK("AMediaExtractor_readSampleData", (uintptr_t)&AMediaExtractor_readSampleData),
NO_THUNK("AMediaExtractor_getSampleTime", (uintptr_t)&AMediaExtractor_getSampleTime),
NO_THUNK("AMediaExtractor_advance", (uintptr_t)&AMediaExtractor_advance),
NO_THUNK("AMediaExtractor_seekTo", (uintptr_t)&AMediaExtractor_seekTo),
NO_THUNK("AMediaFormat_new", (uintptr_t)&AMediaFormat_new),
NO_THUNK("AMediaFormat_delete", (uintptr_t)&AMediaFormat_delete),
NO_THUNK("AMediaFormat_getInt32", (uintptr_t)&AMediaFormat_getInt32),
NO_THUNK("AMediaFormat_getInt64", (uintptr_t)&AMediaFormat_getInt64),
NO_THUNK("AMediaFormat_getFloat", (uintptr_t)&AMediaFormat_getFloat),
NO_THUNK("AMediaFormat_getString", (uintptr_t)&AMediaFormat_getString),
NO_THUNK("AMediaFormat_setInt32", (uintptr_t)&AMediaFormat_setInt32),
NO_THUNK("AMediaCodec_createDecoderByType", (uintptr_t)&AMediaCodec_createDecoderByType),
NO_THUNK("AMediaCodec_delete", (uintptr_t)&AMediaCodec_delete),
NO_THUNK("AMediaCodec_configure", (uintptr_t)&AMediaCodec_configure),
NO_THUNK("AMediaCodec_start", (uintptr_t)&AMediaCodec_start),
NO_THUNK("AMediaCodec_stop", (uintptr_t)&AMediaCodec_stop),
NO_THUNK("AMediaCodec_flush", (uintptr_t)&AMediaCodec_flush),
NO_THUNK("AMediaCodec_dequeueInputBuffer", (uintptr_t)&AMediaCodec_dequeueInputBuffer),
NO_THUNK("AMediaCodec_getInputBuffer", (uintptr_t)&AMediaCodec_getInputBuffer),
NO_THUNK("AMediaCodec_queueInputBuffer", (uintptr_t)&AMediaCodec_queueInputBuffer),
NO_THUNK("AMediaCodec_dequeueOutputBuffer", (uintptr_t)&AMediaCodec_dequeueOutputBuffer),
NO_THUNK("AMediaCodec_getOutputBuffer", (uintptr_t)&AMediaCodec_getOutputBuffer),
NO_THUNK("AMediaCodec_getOutputFormat", (uintptr_t)&AMediaCodec_getOutputFormat),
NO_THUNK("AMediaCodec_releaseOutputBuffer", (uintptr_t)&AMediaCodec_releaseOutputBuffer),
NO_THUNK("AMediaCodec_setOutputSurface", (uintptr_t)&AMediaCodec_setOutputSurface),
NO_THUNK("AImageReader_newWithUsage", (uintptr_t)&AImageReader_newWithUsage),
NO_THUNK("AImageReader_setImageListener", (uintptr_t)&AImageReader_setImageListener),
NO_THUNK("AImageReader_setBufferRemovedListener", (uintptr_t)&AImageReader_setBufferRemovedListener),
NO_THUNK("AImageReader_getWindow", (uintptr_t)&AImageReader_getWindow),
NO_THUNK("AImageReader_acquireLatestImage", (uintptr_t)&AImageReader_acquireLatestImage),
NO_THUNK("AImageReader_delete", (uintptr_t)&AImageReader_delete),
NO_THUNK("AImage_getHardwareBuffer", (uintptr_t)&AImage_getHardwareBuffer),
NO_THUNK("AImage_getWidth", (uintptr_t)&AImage_getWidth),
NO_THUNK("AImage_delete", (uintptr_t)&AImage_delete),
NO_THUNK("AImage_deleteAsync", (uintptr_t)&AImage_deleteAsync),
NO_THUNK("AHardwareBuffer_acquire", (uintptr_t)&AHardwareBuffer_acquire),
NO_THUNK("AHardwareBuffer_release", (uintptr_t)&AHardwareBuffer_release),
NO_THUNK("AHardwareBuffer_describe", (uintptr_t)&AHardwareBuffer_describe),
    {NULL, (uintptr_t)NULL}};


