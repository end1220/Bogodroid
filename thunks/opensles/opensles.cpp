// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
//
// Minimal OpenSL ES shim that bridges the buffer-queue audio pipeline to SDL.
//
// STATUS: compiled in by default (BD_ENABLE_OPENSLES_SHIM, see CMakeLists.txt)
// and handed to exactly one caller. dlsym_impl() in thunks/libc/misc.cpp gates
// slCreateEngine / SL_IID_* on the caller's module:
//
//   * libAkSoundEngine (Wwise) gets them. Its Android sink is what feeds the
//     hardware watchdog; without OpenSL, AK::SoundEngine::Init still reports
//     success and every counter stays clean, but the watchdog fires seconds
//     later and the engine drops to "Hardware audio subsystem stopped
//     responding. Silent mode is enabled." -- a silent game that looks healthy.
//   * everyone else -- notably libunity's built-in FMOD, which probes OpenSL the
//     same way -- gets nil and keeps the path that already works: AudioTrack via
//     org.fmod.FMODAudioDevice -> javastubs/fakefmod.cpp -> the shared mixer.
//     Giving FMOD the shim makes it select OpenSL, find no progress, and give up
//     *without* falling back, so that gate is what keeps the AudioTrack path
//     alive.
//
// Static UND references need no gating: libAkSoundEngine.so carries
// slCreateEngine + SL_IID_ENGINE/PLAY/BUFFERQUEUE/ANDROIDCONFIGURATION as UND
// symbols, and relocations resolve with the requesting module's own identity, so
// only that module can reach them.
//
// Pipeline (when active):
//   slCreateEngine -> Realize -> GetInterface(IID_ENGINE)
//   CreateOutputMix -> Realize
//   CreateAudioPlayer(BufferQueue src, OutputMix sink) -> Realize
//   GetInterface(IID_PLAY) + GetInterface(IID_ANDROIDSIMPLEBUFFERQUEUE)
//   RegisterCallback(playerBufQ, refillCb, ctx)
//   SetPlayState(PLAYING)
//   Enqueue(buf, size) — first buffer; every buffer lands in Wwise's ring of the
//   shared mixer (platform/common/audio_bus.cpp) rather than SDL's FIFO
//   Pump thread watches Wwise's own backlog, fires refillCb so Wwise enqueues
//   more, and pumps the mixer. Mimics OpenSL "buffer-done" via backlog depth.

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
#include <vector>

#include "audio_bus.h"
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
    bool owns_device;     // true when we opened it ourselves; false when it is
                          // FMOD's device, which must never be paused from here
                          // (the mixer feeds FMOD through the same device)
    int sample_rate;
    int channels;
    int bits_per_sample;
    int dev_freq;         // device's actual rate; differs from sample_rate when we
    int dev_channels;     // had to reuse FMOD's device (see audio_bus.h)
    uint64_t samples_queued;  // monotonic; backs Play_GetPosition
    std::vector<int16_t> resample_buf;

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

// Diagnostics only: dispatch is by pointer identity, so an IID we do not handle
// would otherwise be an unreadable raw address in the log.
static const char* bd_iid_name(const SLInterfaceID iid)
{
#define BD_IID_PAIR(name) { "SL_IID_" #name, &kIID_##name },
    static const struct { const char* n; const SLInterfaceID_* p; } kPairs[] = {
        BD_OPENSLES_IIDS(BD_IID_PAIR)
        { "SL_IID_3DDOPPLER",  &kIID_3DDOPPLER  },
        { "SL_IID_3DLOCATION", &kIID_3DLOCATION },
        { "SL_IID_3DSOURCE",   &kIID_3DSOURCE   },
        { "SL_IID_3DGROUPING", &kIID_3DGROUPING },
    };
#undef BD_IID_PAIR
    for (const auto& e : kPairs) {
        if (e.p == iid) return e.n;
    }
    return "(unregistered IID)";
}

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
        // Wwise asks for the queue interface as SL_IID_BUFFERQUEUE -- that is the
        // name it links against (one of only four SL_IID_* UND symbols in
        // libAkSoundEngine.so). On Android the two IIDs denote the same queue, so
        // they must resolve to the same vtable. Missing this one is what made
        // Wwise retry sink creation ~156 times in 60 s.
        if (iid == SL_IID_BUFFERQUEUE)                { *(const void**)pInterface = &p->bufQVTbl; return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_VOLUME)                     { *(const void**)pInterface = &p->volVTbl;  return SL_RESULT_SUCCESS; }
        if (iid == SL_IID_ANDROIDCONFIGURATION)       { *(const void**)pInterface = &p->cfgVTbl;  return SL_RESULT_SUCCESS; }
    } else if (obj->type == 1) { // output mix
        PlayerState* p = (PlayerState*)obj->impl;
        if (p && iid == SL_IID_VOLUME)                { *(const void**)pInterface = &p->volVTbl;  return SL_RESULT_SUCCESS; }
    }
    // Null the output so callers that don't check the return value get a
    // clean NULL deref instead of dereferencing stack garbage.
    *(void**)pInterface = nullptr;
    BD_LOG("OPENSLES", "Object_GetInterface(type=%u) %s (%p) unsupported -> NULL", obj->type, bd_iid_name(iid), (const void*)iid);
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
        // Tear down only what we opened. A borrowed device belongs to FMOD and
        // is still carrying the mixer; closing it here would silence the whole
        // port the moment Wwise recycles a player.
        bd_audio_bus_clear(BD_AUDIO_OWNER_WWISE);
        if (p->dev && p->owns_device) {
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
        BD_LOG("OPENSLES", "SetPlayState(PLAYING) -> resuming SDL dev %u, pump=%d", (unsigned)p->dev, (int)p->pump_started);
        if (p->dev) SDL_PauseAudioDevice(p->dev, 0);
        if (!p->pump_started) {
            p->playing.store(true);
            if (pthread_create(&p->pump_thread, nullptr, player_pump_thread, p) == 0)
                p->pump_started = true;
        }
    } else {
        // Only a device we opened ourselves may be paused. A borrowed one is
        // FMOD's, and the mixer is running the movie soundtrack through it at
        // the same time -- pausing it on Wwise's behalf would take the whole
        // output down and make the score disappear along with it.
        p->playing.store(false);
        if (p->dev && p->owns_device) SDL_PauseAudioDevice(p->dev, 1);
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
    {
        // Backlog still held in Wwise's own ring. SDL's queue level is no longer
        // usable here: the mixer fills it from both engines.
        uint64_t pending = bd_audio_bus_available(BD_AUDIO_OWNER_WWISE);
        if (p->dev_freq > 0 && p->sample_rate > 0 && p->dev_freq != p->sample_rate)
            pending = pending * (uint64_t)p->sample_rate / (uint64_t)p->dev_freq;
        if (pending < total) total -= pending;
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

// Linear-interpolation resampler for 16-bit interleaved PCM.
//
// Wwise asks for its own output rate, while the device we ended up reusing was
// created by FMOD with whatever rate libunity reported (24000 Hz on Oddmar).
// SDL plays whatever it is handed at the *device* rate, so pushing Wwise's
// stream through unchanged would come out pitched and time-stretched. Linear
// interpolation is enough to keep it intelligible.
static void bd_resample_s16(const int16_t* in, size_t in_frames, int channels,
                            int in_rate, int out_rate, std::vector<int16_t>& out)
{
    out.clear();
    if (in_rate <= 0 || out_rate <= 0 || channels <= 0 || in_frames == 0) return;
    size_t out_frames = (size_t)((double)in_frames * (double)out_rate / (double)in_rate);
    if (out_frames == 0) return;
    out.resize(out_frames * (size_t)channels);
    const double step = (double)in_rate / (double)out_rate;
    for (size_t i = 0; i < out_frames; i++) {
        double pos = (double)i * step;
        size_t i0 = (size_t)pos;
        if (i0 >= in_frames) i0 = in_frames - 1;
        size_t i1 = (i0 + 1 < in_frames) ? (i0 + 1) : i0;
        double frac = pos - (double)i0;
        for (int c = 0; c < channels; c++) {
            double a = in[i0 * (size_t)channels + (size_t)c];
            double b = in[i1 * (size_t)channels + (size_t)c];
            out[i * (size_t)channels + (size_t)c] = (int16_t)(a + (b - a) * frac);
        }
    }
}

static SLresult BufQ_Enqueue(SLAndroidSimpleBufferQueueItf self, const void *pBuffer, SLuint32 size)
{
    if (!self || !pBuffer || size == 0) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, bufQVTbl));

    if (p->dev) {
        const bool need_resample = (p->bits_per_sample == 16) &&
                                   (p->dev_freq > 0) &&
                                   (p->dev_freq != p->sample_rate) &&
                                   (p->dev_channels == p->channels);
        if (need_resample) {
            size_t in_frames = (size_t)size / (size_t)(p->channels * 2);
            bd_resample_s16((const int16_t*)pBuffer, in_frames, p->channels,
                            p->sample_rate, p->dev_freq, p->resample_buf);
            if (!p->resample_buf.empty())
                bd_audio_bus_push(BD_AUDIO_OWNER_WWISE, p->resample_buf.data(),
                                  p->resample_buf.size());
        } else if (p->bits_per_sample == 16) {
            // Hand it to the mixer instead of SDL's FIFO: Unity's FMOD is on the
            // same device, and SDL_QueueAudio is a plain FIFO, so pushing both
            // streams there directly interleaves them into noise.
            bd_audio_bus_push(BD_AUDIO_OWNER_WWISE, (const int16_t*)pBuffer,
                              (size_t)size / sizeof(int16_t));
        } else {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                BD_LOG("OPENSLES", "unsupported PCM depth %d bit -> dropping buffers",
                       p->bits_per_sample);
        }
    }
    p->samples_queued += (uint64_t)(size / (p->channels * (p->bits_per_sample / 8)));

    // Rate-limited heartbeat: proves the refill callback is actually firing and
    // audio is flowing, without drowning the log (one Enqueue per ~100 ms).
    static std::atomic<uint64_t> enqueues{0};
    uint64_t n = ++enqueues;
    if (n <= 3 || (n % 250) == 0) {
        size_t backlog = bd_audio_bus_available(BD_AUDIO_OWNER_WWISE);
        BD_LOG("OPENSLES", "Enqueue #%lu: %u B (wwise backlog %zu smp%s)",
               (unsigned long)n, (unsigned)size, backlog,
               p->dev ? "" : ", NO SDL DEVICE");
    }
    return SL_RESULT_SUCCESS;
}

static SLresult BufQ_Clear(SLAndroidSimpleBufferQueueItf self)
{
    if (!self) return SL_RESULT_PARAMETER_INVALID;
    PlayerState* p = (PlayerState*)((char*)self - offsetof(PlayerState, bufQVTbl));
    bd_audio_bus_clear(BD_AUDIO_OWNER_WWISE);
    (void)p;
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
    p->owns_device = false;
    p->dev_freq = 0;
    p->dev_channels = 0;

    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
        SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_AudioSpec want = {}, have = {};
    want.freq     = p->sample_rate;
    want.format   = (p->bits_per_sample == 8) ? AUDIO_U8 : AUDIO_S16LSB;
    want.channels = (Uint8)p->channels;
    want.samples  = 1024;
    want.callback = nullptr;

    p->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (p->dev != 0) {
        p->owns_device = true;
        p->dev_freq = have.freq;
        p->dev_channels = have.channels;
    } else {
        // SDL hands out exactly one output device per process, and asking by name
        // does not help -- both fail with "Audio device already open" (verified
        // against SDL 2.0.10). On a Wwise title the FMOD path in
        // javastubs/fakefmod.cpp wires up the device first, so reuse its device
        // and share it through the mixer in platform/common/audio_bus.h. Without
        // this the refill callback below never fires, Wwise's watchdog expires,
        // and the engine reports silent mode with every counter looking healthy.
        int bus_freq = 0, bus_ch = 0, bus_bits = 0;
        SDL_AudioDeviceID shared = bd_audio_bus_device(&bus_freq, &bus_ch, &bus_bits);
        if (shared != 0) {
            p->dev = shared;
            p->dev_freq = bus_freq;
            p->dev_channels = bus_ch;
            bd_audio_bus_set_owner(BD_AUDIO_OWNER_WWISE);
            BD_LOG("OPENSLES", "reusing shared SDL dev %u (%d Hz x %d ch x %d bit); Wwise wants %d Hz x %d ch x %d bit -> %s",
                   (unsigned)shared, bus_freq, bus_ch, bus_bits,
                   p->sample_rate, p->channels, p->bits_per_sample,
                   (p->bits_per_sample == 16 && bus_freq > 0 && bus_freq != p->sample_rate) ? "resampling" : "direct");
        }
    }

    if (p->dev != 0)
        bd_audio_bus_set_capacity(BD_AUDIO_OWNER_WWISE,
                                  (size_t)p->sample_rate *
                                  (size_t)(p->channels > 0 ? p->channels : 2)); // ~1 s

    if (p->dev == 0)
        BD_LOG("OPENSLES", "no SDL device (own open failed: %s; bus empty) -> no hardware interrupts will reach Wwise", SDL_GetError());
    else
        BD_LOG("OPENSLES", "CreateAudioPlayer: %d Hz x %d ch x %d bit -> SDL dev %u (queued-depth target %u B)",
               p->sample_rate, p->channels, p->bits_per_sample, (unsigned)p->dev,
               (unsigned)(p->sample_rate * p->channels * (p->bits_per_sample / 8) / 10));

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
    // Wwise's refill callback hands over small blocks (256 B on Oddmar), so aim
    // at its own backlog rather than at SDL's queue level: the SDL queue is now
    // fed by the mixer in audio_bus.cpp, which drains it on the device's clock
    // and therefore says nothing about where Wwise is in its stream.
    const int rate = (p->dev_freq > 0) ? p->dev_freq : p->sample_rate;
    const size_t low_water =
        (size_t)rate * (size_t)(p->channels > 0 ? p->channels : 2) / 10; // ~100 ms
    while (!p->stopping.load()) {
        if (p->playing.load() && p->dev && p->cb) {
            // Refill until the backlog is whole again. One callback can be as
            // small as a couple of milliseconds of audio, so a single call per
            // tick would let the mixer starve and crackle.
            for (int i = 0; i < 64; i++) {
                if (bd_audio_bus_available(BD_AUDIO_OWNER_WWISE) >= low_water)
                    break;
                p->cb((SLAndroidSimpleBufferQueueItf)&p->bufQVTbl, p->cb_ctx);
            }
            // Also drive the mixer here, so a title whose FMOD device is never
            // pumped (or is pumped by a stalled thread) still gets output.
            bd_audio_bus_pump(p->dev);
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
    // A/B escape hatch: refuse to hand Wwise an engine, which puts it back on the
    // pre-shim path (silent audio, but otherwise identical). Lets one binary
    // answer "did enabling the shim change this?" without a rebuild.
    if (getenv("BD_OPENSLES_OFF") != NULL) {
        BD_LOG("OPENSLES", "slCreateEngine refused (BD_OPENSLES_OFF set) -> Wwise sees no OpenSL ES");
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }

    if (!pEngine) return SL_RESULT_PARAMETER_INVALID;
    OpenSLObj* obj = (OpenSLObj*)calloc(1, sizeof(OpenSLObj));
    EngineState* e = (EngineState*)calloc(1, sizeof(EngineState));
    e->engineVTbl = &gEngineVTbl;
    obj->vtable = &gObjectVTbl;
    obj->type = 0;
    obj->impl = e;
    *pEngine = (SLObjectItf)&obj->vtable;
    BD_LOG("OPENSLES", "slCreateEngine -> engine %p", (void*)obj);
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
