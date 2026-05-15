// Minimal OpenSL ES shim that bridges FMOD's audio output to SDL.
//
// STATUS: DEFAULT-DISABLED via BD_ENABLE_OPENSLES_SHIM (see CMakeLists.txt).
// With this shim present, libunity's internal FMOD picks OpenSL ES as its
// output, briefly probes us, decides no progress is being made, and gives
// up *without* falling back. The functional audio path is AudioTrack via
// org.fmod.FMODAudioDevice → projects/unityloader/javastubs/fakefmod.cpp →
// SDL_QueueAudio. Letting dlsym(slCreateEngine) return nil (which happens
// automatically when this file is compiled with the macro unset) is what
// makes libunity skip OpenSL and reach the working AudioTrack path. Source
// is retained under thunks/opensles/ for any future attempt at fixing the
// OpenSL output.
//
// Pipeline (when active):
//   slCreateEngine -> Realize -> GetInterface(IID_ENGINE)
//   CreateOutputMix -> Realize
//   CreateAudioPlayer(BufferQueue src, OutputMix sink) -> Realize
//   GetInterface(IID_PLAY) + GetInterface(IID_ANDROIDSIMPLEBUFFERQUEUE)
//   RegisterCallback(playerBufQ, refillCb, ctx)
//   SetPlayState(PLAYING)
//   Enqueue(buf, size) — first buffer
//   Pump thread watches SDL queue level, fires refillCb so FMOD enqueues
//   more. Mimics OpenSL "buffer-done" via SDL queue depth.

#include "so_util.h"
#include "thunk_gen.h"

#ifndef BD_ENABLE_OPENSLES_SHIM

// Shim disabled. Expose an empty symtable so the extern declaration in
// projects/unityloader/main.cpp still links; libunity's dlsym(slCreateEngine)
// then walks an empty list and gets nil, which is exactly what we want.
DynLibFunction symtable_opensles[] = { { NULL, 0 } };

#else  // BD_ENABLE_OPENSLES_SHIM

#include <SDL2/SDL.h>
#include <pthread.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "platform.h"
#include "logging.h"

// ───────── OpenSL ES type / constant subset ─────────

typedef int32_t  SLresult;
typedef uint32_t SLuint32;
typedef uint64_t SLuint64;
typedef int32_t  SLint32;
typedef uint16_t SLuint16;
typedef int16_t  SLint16;
typedef uint8_t  SLuint8;
typedef uint32_t SLboolean;

#define SL_RESULT_SUCCESS                        ((SLresult) 0x00000000)
#define SL_RESULT_PARAMETER_INVALID              ((SLresult) 0x00000002)
#define SL_RESULT_FEATURE_UNSUPPORTED            ((SLresult) 0x0000000C)
#define SL_BOOLEAN_FALSE                         ((SLboolean) 0)
#define SL_PLAYSTATE_PLAYING                     ((SLuint32) 0x00000003)
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE  ((SLuint32) 0x800007BD)
#define SL_DATALOCATOR_OUTPUTMIX                 ((SLuint32) 0x00000006)
#define SL_DATAFORMAT_PCM                        ((SLuint32) 0x00000002)

// SL interface IDs — pointer-identity comparison. FMOD always passes the
// same global pointer per IID, so we can dispatch on raw pointer equality.
struct SLInterfaceID_ { uint32_t tag; };
typedef const struct SLInterfaceID_ *SLInterfaceID;
// HK enumerates many SL_IID_* at init and aborts if any is missing, so we
// declare every standard OpenSL ES IID. Each is an opaque tag pointer.
#define BD_OPENSLES_IIDS(X) \
    X(ENGINE)                  X(PLAY)              \
    X(ANDROIDSIMPLEBUFFERQUEUE) X(VOLUME)           \
    X(ANDROIDCONFIGURATION)    X(BUFFERQUEUE)       \
    X(PLAYBACKRATE)            X(PREFETCHSTATUS)    \
    X(MUTESOLO)                X(EFFECTSEND)        \
    X(RECORD)                  X(SEEK)              \
    X(OBJECT)                  X(OUTPUTMIX)         \
    X(PITCH)                   X(RATEPITCH)         \
    X(BASSBOOST)               X(EQUALIZER)         \
    X(PRESETREVERB)            X(ENVIRONMENTALREVERB) \
    X(VIRTUALIZER)             X(VISUALIZATION)     \
    X(METADATAEXTRACTION)      X(METADATATRAVERSAL) \
    X(THREADSYNC)              X(DEVICEVOLUME)      \
    X(DYNAMICINTERFACEMANAGEMENT) X(DYNAMICSOURCE)  \
    X(ENGINECAPABILITIES)      X(AUDIOIODEVICECAPABILITIES) \
    X(AUDIODECODERCAPABILITIES) X(AUDIOENCODER)     \
    X(AUDIOENCODERCAPABILITIES) X(LED)              \
    X(VIBRA)                   X(MIDIMESSAGE)       \
    X(MIDIMUTESOLO)            X(MIDITEMPO)         \
    X(MIDITIME)                X(ANDROIDEFFECT)     \
    X(ANDROIDEFFECTCAPABILITIES) X(ANDROIDEFFECTSEND) \
    X(ANDROIDBUFFERQUEUESOURCE)

// 3D* IIDs declared separately — macro-stringify would emit "SL_IID__3DDOPPLER"
// (extra underscore) because C identifiers can't start with a digit.
static const SLInterfaceID_ kIID_3DDOPPLER  = { 90 };
static const SLInterfaceID_ kIID_3DLOCATION = { 91 };
static const SLInterfaceID_ kIID_3DSOURCE   = { 92 };
static const SLInterfaceID_ kIID_3DGROUPING = { 93 };
extern "C" {
    const SLInterfaceID_* SL_IID_3DDOPPLER  = &kIID_3DDOPPLER;
    const SLInterfaceID_* SL_IID_3DLOCATION = &kIID_3DLOCATION;
    const SLInterfaceID_* SL_IID_3DSOURCE   = &kIID_3DSOURCE;
    const SLInterfaceID_* SL_IID_3DGROUPING = &kIID_3DGROUPING;
}

#define BD_DECL_IID(name) static const SLInterfaceID_ kIID_##name = { __COUNTER__ + 1 };
BD_OPENSLES_IIDS(BD_DECL_IID)
#undef BD_DECL_IID

extern "C" {
#define BD_EXPORT_IID(name) const SLInterfaceID_* SL_IID_##name = &kIID_##name;
    BD_OPENSLES_IIDS(BD_EXPORT_IID)
#undef BD_EXPORT_IID
}

// Interface handle types — pointer to pointer to vtable, COM style.
struct SLObjectItf_;
struct SLEngineItf_;
struct SLPlayItf_;
struct SLAndroidSimpleBufferQueueItf_;
struct SLVolumeItf_;
typedef const struct SLObjectItf_                    *const *SLObjectItf;
typedef const struct SLEngineItf_                    *const *SLEngineItf;
typedef const struct SLPlayItf_                      *const *SLPlayItf;
typedef const struct SLAndroidSimpleBufferQueueItf_  *const *SLAndroidSimpleBufferQueueItf;
typedef const struct SLVolumeItf_                    *const *SLVolumeItf;

typedef void (*slAndroidSimpleBufferQueueCallback)(SLAndroidSimpleBufferQueueItf caller, void *pContext);

// ───────── Vtable structs ─────────

struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf, SLboolean);
    SLresult (*Resume)(SLObjectItf, SLboolean);
    SLresult (*GetState)(SLObjectItf, SLuint32*);
    SLresult (*GetInterface)(SLObjectItf, const SLInterfaceID, void*);
    SLresult (*RegisterCallback)(SLObjectItf, void*, void*);
    void     (*AbortAsyncOperation)(SLObjectItf);
    void     (*Destroy)(SLObjectItf);
    SLresult (*SetPriority)(SLObjectItf, SLint32, SLboolean);
    SLresult (*GetPriority)(SLObjectItf, SLint32*, SLboolean*);
    SLresult (*SetLossOfControlInterfaces)(SLObjectItf, SLint16, SLInterfaceID*, SLboolean);
};

struct SLEngineItf_ {
    SLresult (*CreateLEDDevice)(SLEngineItf, SLObjectItf*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateVibraDevice)(SLEngineItf, SLObjectItf*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateAudioPlayer)(SLEngineItf, SLObjectItf*, void*, void*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateAudioRecorder)(SLEngineItf, SLObjectItf*, void*, void*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateMidiPlayer)(SLEngineItf, SLObjectItf*, void*, void*, void*, void*, void*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateListener)(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*Create3DGroup)(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateOutputMix)(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateMetadataExtractor)(SLEngineItf, SLObjectItf*, void*, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*CreateExtensionObject)(SLEngineItf, SLObjectItf*, void*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*);
    SLresult (*QueryNumSupportedInterfaces)(SLEngineItf, SLuint32, SLuint32*);
    SLresult (*QuerySupportedInterfaces)(SLEngineItf, SLuint32, SLuint32, SLInterfaceID*);
    SLresult (*QueryNumSupportedExtensions)(SLEngineItf, SLuint32*);
    SLresult (*QuerySupportedExtension)(SLEngineItf, SLuint32, void*, SLint16*);
    SLresult (*IsExtensionSupported)(SLEngineItf, void*, SLboolean*);
};

struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf, SLuint32);
    SLresult (*GetPlayState)(SLPlayItf, SLuint32*);
    SLresult (*GetDuration)(SLPlayItf, SLuint64*);
    SLresult (*GetPosition)(SLPlayItf, SLuint32*);
    SLresult (*RegisterCallback)(SLPlayItf, void*, void*);
    SLresult (*SetCallbackEventsMask)(SLPlayItf, SLuint32);
    SLresult (*GetCallbackEventsMask)(SLPlayItf, SLuint32*);
    SLresult (*SetMarkerPosition)(SLPlayItf, SLuint32);
    SLresult (*ClearMarkerPosition)(SLPlayItf);
    SLresult (*GetMarkerPosition)(SLPlayItf, SLuint32*);
    SLresult (*SetPositionUpdatePeriod)(SLPlayItf, SLuint32);
    SLresult (*GetPositionUpdatePeriod)(SLPlayItf, SLuint32*);
};

struct SLAndroidSimpleBufferQueueItf_ {
    SLresult (*Enqueue)(SLAndroidSimpleBufferQueueItf, const void*, SLuint32);
    SLresult (*Clear)(SLAndroidSimpleBufferQueueItf);
    SLresult (*GetState)(SLAndroidSimpleBufferQueueItf, void*);
    SLresult (*RegisterCallback)(SLAndroidSimpleBufferQueueItf, slAndroidSimpleBufferQueueCallback, void*);
};

struct SLVolumeItf_ {
    SLresult (*SetVolumeLevel)(SLVolumeItf, SLint16);
    SLresult (*GetVolumeLevel)(SLVolumeItf, SLint16*);
    SLresult (*GetMaxVolumeLevel)(SLVolumeItf, SLint16*);
    SLresult (*SetMute)(SLVolumeItf, SLboolean);
    SLresult (*GetMute)(SLVolumeItf, SLboolean*);
    SLresult (*EnableStereoPosition)(SLVolumeItf, SLboolean);
    SLresult (*IsEnabledStereoPosition)(SLVolumeItf, SLboolean*);
    SLresult (*SetStereoPosition)(SLVolumeItf, SLint16);
    SLresult (*GetStereoPosition)(SLVolumeItf, SLint16*);
};

// Android-specific config — FMOD passes "androidPerformanceMode" /
// "androidStreamType". We accept and ignore.
typedef const struct SLAndroidConfigurationItf_ *const *SLAndroidConfigurationItf;
struct SLAndroidConfigurationItf_ {
    SLresult (*SetConfiguration)(SLAndroidConfigurationItf, const char*, const void*, SLuint32);
    SLresult (*GetConfiguration)(SLAndroidConfigurationItf, const char*, SLuint32*, void*);
    SLresult (*AcquireJavaProxy)(SLAndroidConfigurationItf, const char*, void*);
    SLresult (*ReleaseJavaProxy)(SLAndroidConfigurationItf, const char*);
};

struct SLDataFormat_PCM {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;     // milliHz on Android
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
};
struct SLDataSource {
    void* pLocator;
    void* pFormat;
};

// ───────── Internal state ─────────

struct OpenSLObj {
    const void* vtable;     // = &gObjectVTbl (must be first — SLObjectItf points here)
    SLuint32 type;          // 0=engine, 1=outputmix, 2=audioplayer
    void* impl;
};

struct EngineState {
    const void* engineVTbl;
};

// Each *VTbl field holds &gXxxVTbl. SLxxxItf is `const T * const *` so the
// caller derefs once to reach the vtable struct and once more to load a slot.
// Object_GetInterface must therefore hand back &p->xxxVTbl directly — not
// pointer-to-pointer-to-vtable, which would add a third indirection and land
// `(*itf)->method` on the vtable struct address itself (in .data.rel.ro, not
// executable → SEGV_ACCERR).
struct PlayerState {
    const void* playVTbl;   // = &gPlayVTbl
    const void* bufQVTbl;   // = &gBufQVTbl
    const void* volVTbl;    // = &gVolumeVTbl
    const void* cfgVTbl;    // = &gCfgVTbl

    SDL_AudioDeviceID dev;
    int sample_rate;
    int channels;
    int bits_per_sample;
    uint64_t samples_queued;  // monotonic; backs Play_GetPosition

    slAndroidSimpleBufferQueueCallback cb;
    void* cb_ctx;

    std::atomic<bool> playing;
    std::atomic<bool> stopping;
    pthread_t pump_thread;
    bool pump_started;
};

// ───────── Forward decls ─────────

static SLresult Object_Realize(SLObjectItf, SLboolean);
static SLresult Object_Resume(SLObjectItf, SLboolean);
static SLresult Object_GetState(SLObjectItf, SLuint32*);
static SLresult Object_GetInterface(SLObjectItf, const SLInterfaceID, void*);
static SLresult Object_RegisterCallback(SLObjectItf, void*, void*);
static void     Object_AbortAsyncOperation(SLObjectItf);
static void     Object_Destroy(SLObjectItf);
static SLresult Object_SetPriority(SLObjectItf, SLint32, SLboolean);
static SLresult Object_GetPriority(SLObjectItf, SLint32*, SLboolean*);
static SLresult Object_SetLossOfControlInterfaces(SLObjectItf, SLint16, SLInterfaceID*, SLboolean);

static SLresult Engine_CreateOutputMix(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*);
static SLresult Engine_CreateAudioPlayer(SLEngineItf, SLObjectItf*, void*, void*, SLuint32, const SLInterfaceID*, const SLboolean*);
static SLresult Engine_NoOp(void*, void*, void*, void*, void*, void*, void*, void*);

static SLresult Play_SetPlayState(SLPlayItf, SLuint32);
static SLresult Play_GetPosition(SLPlayItf, SLuint32*);
static SLresult Play_NoOp(void*, void*, void*, void*, void*, void*, void*, void*);

static SLresult BufQ_Enqueue(SLAndroidSimpleBufferQueueItf, const void*, SLuint32);
static SLresult BufQ_Clear(SLAndroidSimpleBufferQueueItf);
static SLresult BufQ_GetState(SLAndroidSimpleBufferQueueItf, void*);
static SLresult BufQ_RegisterCallback(SLAndroidSimpleBufferQueueItf, slAndroidSimpleBufferQueueCallback, void*);

static SLresult Vol_NoOp(void*, void*, void*, void*, void*, void*, void*, void*);

static SLresult Cfg_SetConfiguration(SLAndroidConfigurationItf, const char*, const void*, SLuint32);
static SLresult Cfg_GetConfiguration(SLAndroidConfigurationItf, const char*, SLuint32*, void*);
static SLresult Cfg_AcquireJavaProxy(SLAndroidConfigurationItf, const char*, void*);
static SLresult Cfg_ReleaseJavaProxy(SLAndroidConfigurationItf, const char*);

// ───────── Vtables ─────────

static const struct SLObjectItf_ gObjectVTbl = {
    Object_Realize, Object_Resume, Object_GetState, Object_GetInterface,
    Object_RegisterCallback, Object_AbortAsyncOperation, Object_Destroy,
    Object_SetPriority, Object_GetPriority, Object_SetLossOfControlInterfaces
};

static const struct SLEngineItf_ gEngineVTbl = {
    (SLresult (*)(SLEngineItf, SLObjectItf*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLObjectItf*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    Engine_CreateAudioPlayer,
    (SLresult (*)(SLEngineItf, SLObjectItf*, void*, void*, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLObjectItf*, void*, void*, void*, void*, void*, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    Engine_CreateOutputMix,
    (SLresult (*)(SLEngineItf, SLObjectItf*, void*, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLObjectItf*, void*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLuint32, SLuint32*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLuint32, SLuint32, SLInterfaceID*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLuint32*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, SLuint32, void*, SLint16*))Engine_NoOp,
    (SLresult (*)(SLEngineItf, void*, SLboolean*))Engine_NoOp,
};

static const struct SLPlayItf_ gPlayVTbl = {
    Play_SetPlayState,
    (SLresult (*)(SLPlayItf, SLuint32*))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint64*))Play_NoOp,
    Play_GetPosition,
    (SLresult (*)(SLPlayItf, void*, void*))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint32))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint32*))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint32))Play_NoOp,
    (SLresult (*)(SLPlayItf))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint32*))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint32))Play_NoOp,
    (SLresult (*)(SLPlayItf, SLuint32*))Play_NoOp,
};

static const struct SLAndroidSimpleBufferQueueItf_ gBufQVTbl = {
    BufQ_Enqueue, BufQ_Clear, BufQ_GetState, BufQ_RegisterCallback
};

static const struct SLVolumeItf_ gVolumeVTbl = {
    (SLresult (*)(SLVolumeItf, SLint16))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLint16*))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLint16*))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLboolean))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLboolean*))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLboolean))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLboolean*))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLint16))Vol_NoOp,
    (SLresult (*)(SLVolumeItf, SLint16*))Vol_NoOp,
};

static const struct SLAndroidConfigurationItf_ gCfgVTbl = {
    Cfg_SetConfiguration, Cfg_GetConfiguration,
    Cfg_AcquireJavaProxy, Cfg_ReleaseJavaProxy,
};

// ───────── Object methods ─────────

static SLresult Object_Realize(SLObjectItf, SLboolean)                  { return SL_RESULT_SUCCESS; }
static SLresult Object_Resume(SLObjectItf, SLboolean)                   { return SL_RESULT_SUCCESS; }
static SLresult Object_GetState(SLObjectItf, SLuint32 *p)               { if (p) *p = 2; return SL_RESULT_SUCCESS; }
static SLresult Object_RegisterCallback(SLObjectItf, void*, void*)      { return SL_RESULT_SUCCESS; }
static void     Object_AbortAsyncOperation(SLObjectItf)                 {}
static SLresult Object_SetPriority(SLObjectItf, SLint32, SLboolean)     { return SL_RESULT_SUCCESS; }
static SLresult Object_GetPriority(SLObjectItf, SLint32*, SLboolean*)   { return SL_RESULT_SUCCESS; }
static SLresult Object_SetLossOfControlInterfaces(SLObjectItf, SLint16, SLInterfaceID*, SLboolean) { return SL_RESULT_SUCCESS; }

static SLresult Object_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *pInterface)
{
    if (!self || !pInterface || !iid) return SL_RESULT_PARAMETER_INVALID;
    OpenSLObj* obj = (OpenSLObj*)self;
    if (obj->type == 0) { // engine
        EngineState* e = (EngineState*)obj->impl;
        if (iid == SL_IID_ENGINE) {
            *(const void**)pInterface = &e->engineVTbl;
            return SL_RESULT_SUCCESS;
        }
    } else if (obj->type == 2) { // audio player
        PlayerState* p = (PlayerState*)obj->impl;
        if (iid == SL_IID_PLAY)                       { *(const void**)pInterface = &p->playVTbl; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE)   { *(const void**)pInterface = &p->bufQVTbl; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_VOLUME)                     { *(const void**)pInterface = &p->volVTbl;  return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDCONFIGURATION)       { *(const void**)pInterface = &p->cfgVTbl;  return SL_RESULT_SUCCESS; }
    }
    // Null the output so callers that don't check the return value get a
    // clean NULL deref instead of dereferencing stack garbage.
    *(void**)pInterface = nullptr;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static void Object_Destroy(SLObjectItf self)
{
    if (!self) return;
    OpenSLObj* obj = (OpenSLObj*)self;
    if (obj->type == 2 && obj->impl) {
        PlayerState* p = (PlayerState*)obj->impl;
        p->stopping.store(true);
        p->playing.store(false);
        if (p->pump_started) pthread_join(p->pump_thread, nullptr);
        if (p->dev) {
            SDL_PauseAudioDevice(p->dev, 1);
            SDL_CloseAudioDevice(p->dev);
        }
        delete p;
    } else if (obj->impl) {
        free(obj->impl);
    }
    free(obj);
}

// ───────── Play methods ─────────

static void* player_pump_thread(void* arg);

static SLresult Play_SetPlayState(SLPlayItf self, SLuint32 state)
{
    if (!self) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, playVTbl));
    if (state == SL_PLAYSTATE_PLAYING) {
        if (p->dev) SDL_PauseAudioDevice(p->dev, 0);
        if (!p->pump_started) {
            p->playing.store(true);
            if (pthread_create(&p->pump_thread, nullptr, player_pump_thread, p) == 0)
                p->pump_started = true;
        }
    } else {
        p->playing.store(false);
        if (p->dev) SDL_PauseAudioDevice(p->dev, 1);
    }
    return SL_RESULT_SUCCESS;
}

// Position in milliseconds. FMOD polls this to confirm the device is making
// progress; if it stays 0 FMOD declares the device broken and gives up.
static SLresult Play_GetPosition(SLPlayItf self, SLuint32 *pMsec)
{
    if (!self || !pMsec) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, playVTbl));
    uint64_t total = p->samples_queued;
    if (p->dev) {
        uint32_t pending_bytes = SDL_GetQueuedAudioSize(p->dev);
        uint64_t pending_samples = pending_bytes / (p->channels * (p->bits_per_sample / 8));
        if (pending_samples < total) total -= pending_samples;
        else total = 0;
    }
    *pMsec = (SLuint32)(total * 1000ULL / (p->sample_rate ? p->sample_rate : 48000));
    return SL_RESULT_SUCCESS;
}

// Fixed-args no-op. ARM64 ABI requires non-varargs in vtable slots; varargs
// reads from a different register layout and crashes.
static SLresult Play_NoOp(void*, void*, void*, void*, void*, void*, void*, void*) {
    return SL_RESULT_SUCCESS;
}

// ───────── BufferQueue methods ─────────

static SLresult BufQ_Enqueue(SLAndroidSimpleBufferQueueItf self, const void *pBuffer, SLuint32 size)
{
    if (!self || !pBuffer || size == 0) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, bufQVTbl));
    if (p->dev) SDL_QueueAudio(p->dev, pBuffer, size);
    p->samples_queued += (uint64_t)(size / (p->channels * (p->bits_per_sample / 8)));
    return SL_RESULT_SUCCESS;
}

static SLresult BufQ_Clear(SLAndroidSimpleBufferQueueItf self)
{
    if (!self) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, bufQVTbl));
    if (p->dev) SDL_ClearQueuedAudio(p->dev);
    return SL_RESULT_SUCCESS;
}

static SLresult BufQ_GetState(SLAndroidSimpleBufferQueueItf, void *pState)
{
    if (pState) memset(pState, 0, 8);
    return SL_RESULT_SUCCESS;
}

static SLresult BufQ_RegisterCallback(SLAndroidSimpleBufferQueueItf self,
                                       slAndroidSimpleBufferQueueCallback callback,
                                       void *pContext)
{
    if (!self) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, bufQVTbl));
    p->cb = callback;
    p->cb_ctx = pContext;
    return SL_RESULT_SUCCESS;
}

// ───────── Volume + Android config (no-ops) ─────────

static SLresult Vol_NoOp(void*, void*, void*, void*, void*, void*, void*, void*) {
    return SL_RESULT_SUCCESS;
}

static SLresult Cfg_SetConfiguration(SLAndroidConfigurationItf, const char*, const void*, SLuint32) {
    return SL_RESULT_SUCCESS;
}
static SLresult Cfg_GetConfiguration(SLAndroidConfigurationItf, const char*, SLuint32* pValueSize, void*) {
    if (pValueSize) *pValueSize = 0;
    return SL_RESULT_SUCCESS;
}
static SLresult Cfg_AcquireJavaProxy(SLAndroidConfigurationItf, const char*, void* pProxyObj) {
    if (pProxyObj) *(void**)pProxyObj = nullptr;
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
static SLresult Cfg_ReleaseJavaProxy(SLAndroidConfigurationItf, const char*) {
    return SL_RESULT_SUCCESS;
}

// ───────── Engine methods ─────────

static SLresult Engine_CreateOutputMix(SLEngineItf, SLObjectItf *pMix,
                                        SLuint32, const SLInterfaceID*, const SLboolean*)
{
    if (!pMix) return SL_RESULT_PARAMETER_INVALID;
    OpenSLObj* obj = (OpenSLObj*)calloc(1, sizeof(OpenSLObj));
    obj->vtable = &gObjectVTbl;
    obj->type = 1;
    obj->impl = nullptr;
    *pMix = (SLObjectItf)&obj->vtable;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_CreateAudioPlayer(SLEngineItf, SLObjectItf *pPlayer,
                                          void *pAudioSrc, void * /*pAudioSnk*/,
                                          SLuint32, const SLInterfaceID*, const SLboolean*)
{
    if (!pPlayer || !pAudioSrc) return SL_RESULT_PARAMETER_INVALID;

    SLDataSource* src = (SLDataSource*)pAudioSrc;
    SLDataFormat_PCM* fmt = (SLDataFormat_PCM*)src->pFormat;

    PlayerState* p = new PlayerState();
    p->playVTbl = &gPlayVTbl;
    p->bufQVTbl = &gBufQVTbl;
    p->volVTbl  = &gVolumeVTbl;
    p->cfgVTbl  = &gCfgVTbl;
    p->sample_rate     = fmt ? (int)(fmt->samplesPerSec / 1000) : 48000;
    p->channels        = fmt ? (int)fmt->numChannels : 2;
    p->bits_per_sample = fmt ? (int)fmt->bitsPerSample : 16;
    p->samples_queued = 0;
    p->cb = nullptr; p->cb_ctx = nullptr;
    p->playing.store(false); p->stopping.store(false);
    p->pump_started = false;
    p->dev = 0;

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_AudioSpec want = {}, have = {};
    want.freq     = p->sample_rate;
    want.format   = (p->bits_per_sample == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    want.channels = (Uint8)p->channels;
    want.samples  = 1024;
    want.callback = nullptr;

    p->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (p->dev == 0)
        BD_LOG("OPENSLES", "SDL_OpenAudioDevice failed: %s", SDL_GetError());

    OpenSLObj* obj = (OpenSLObj*)calloc(1, sizeof(OpenSLObj));
    obj->vtable = &gObjectVTbl;
    obj->type = 2;
    obj->impl = p;
    *pPlayer = (SLObjectItf)&obj->vtable;
    return SL_RESULT_SUCCESS;
}

static SLresult Engine_NoOp(void*, void*, void*, void*, void*, void*, void*, void*) {
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

// ───────── Pump thread ─────────

static void* player_pump_thread(void* arg)
{
    PlayerState* p = (PlayerState*)arg;
    const uint32_t target = (uint32_t)(p->sample_rate * p->channels * (p->bits_per_sample / 8) / 10); // ~100ms
    while (!p->stopping.load()) {
        if (p->playing.load() && p->dev && p->cb) {
            if (SDL_GetQueuedAudioSize(p->dev) < target) {
                p->cb((SLAndroidSimpleBufferQueueItf)&p->bufQVTbl, p->cb_ctx);
            }
        }
        SDL_Delay(5);
    }
    return nullptr;
}

// ───────── Top-level entry ─────────

extern "C" ABI_ATTR SLresult slCreateEngine_impl(
    SLObjectItf *pEngine, SLuint32 /*numOptions*/, void* /*pEngineOptions*/,
    SLuint32 /*numInterfaces*/, const SLInterfaceID* /*pInterfaceIds*/,
    const SLboolean* /*pInterfaceRequired*/)
{
    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
    OpenSLObj* obj = (OpenSLObj*)calloc(1, sizeof(OpenSLObj));
    EngineState* e = (EngineState*)calloc(1, sizeof(EngineState));
    e->engineVTbl = &gEngineVTbl;
    obj->vtable = &gObjectVTbl;
    obj->type = 0;
    obj->impl = e;
    *pEngine = (SLObjectItf)&obj->vtable;
    return SL_RESULT_SUCCESS;
}

#define BD_SYM_IID(name) NO_THUNK("SL_IID_" #name, (uintptr_t)&SL_IID_##name),
DynLibFunction symtable_opensles[] = {
    NO_THUNK("slCreateEngine", (uintptr_t)&slCreateEngine_impl),
    BD_OPENSLES_IIDS(BD_SYM_IID)
    NO_THUNK("SL_IID_3DDOPPLER",  (uintptr_t)&SL_IID_3DDOPPLER),
    NO_THUNK("SL_IID_3DLOCATION", (uintptr_t)&SL_IID_3DLOCATION),
    NO_THUNK("SL_IID_3DSOURCE",   (uintptr_t)&SL_IID_3DSOURCE),
    NO_THUNK("SL_IID_3DGROUPING", (uintptr_t)&SL_IID_3DGROUPING),
    { NULL, 0 },
};
#undef BD_SYM_IID

#endif  // BD_ENABLE_OPENSLES_SHIM
