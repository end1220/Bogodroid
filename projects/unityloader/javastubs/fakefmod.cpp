#include "fakefmod.h"
#include "../globals.h" 
#include "android.h"
#include "audio_bus.h"
#include "logging.h"
#include "sys_volume.h"
#include <SDL2/SDL.h>
#include <cmath> 
#include <algorithm>
#include <cstdint>
#include <exception>
#include <jnivm/bytebuffer.h>
#include <thread>
#include <chrono>

using namespace jnivm::org::fmod;

FMODAudioDevice::FMODAudioDevice() : mRunning(false), mAudioDevice(0) { }

FMODAudioDevice::~FMODAudioDevice() {
    stop();
}

static FakeJni::JClass* fmodClass = nullptr;

// One-shot diagnostic: dump every native entry jnivm holds for
// org/fmod/FMODAudioDevice, so a mis-resolved entry can be spotted against the
// JNINativeMethod table the game's libunity registers (fmodGetInfo must be
// libunity+0xafb5e4, fmodProcess +0xafb6ac, fmodProcessMicData +0xafb738 for
// this build; see tools/fmodtab.py).
static void bd_dump_fmod_methods(FakeJni::JClass* cl) {
    if (!cl) {
        BD_LOG("AUDIO", "fmodClass is null");
        return;
    }
    BD_LOG("AUDIO", "FMODAudioDevice class=%s methods=%zu natives=%zu",
           cl->nativeprefix.c_str(), cl->methods.size(), cl->natives.size());
    for (auto& m : cl->methods) {
        if (!m) continue;
        if (m->name.rfind("fmod", 0) != 0) continue;
        BD_LOG("AUDIO", "  reg name=%-22s sig=%-30s native=%p static=%d handle=%p",
               m->name.c_str(), m->signature.c_str(), m->native, (int)m->_static,
               (void*)m->nativehandle.get());
    }
    for (auto& kv : cl->natives) {
        BD_LOG("AUDIO", "  map name=%-22s native=%p", kv.first.c_str(), kv.second);
    }
}

void FMODAudioDevice::start() {
    if (mAudioThread) {
        stop();
    }

    if (!fmodClass) {
        fmodClass = vm.findClass("org/fmod/FMODAudioDevice").get();
        bd_dump_fmod_methods(fmodClass);
    }

    mRunning.store(true);

    mAudioThread = std::make_shared<jnivm::android::os::HandlerThread>(std::make_shared<FakeJni::JString>("Audio"));
    
    mAudioThread->start();

    auto mLooper = mAudioThread->getLooper();
    auto mHandler = std::make_shared<jnivm::android::os::Handler>(mLooper);

    mHandler->post(jnivm::java::lang::LambdaRunnable::Create([this]() {
            this->runAudio();
        }));
    
}

void FMODAudioDevice::stop() {
    if (mAudioThread) {
        mRunning.store(false); // Break out of the audio loop
        mAudioThread->quit(); // Quit the Looper
        mAudioThread->join(); // Join the thread
        mAudioThread = nullptr;
    }
}

void FMODAudioDevice::close() {
    stop();
}

void FMODAudioDevice::runAudio() {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        verbose("FMODAudioDevice", "SDL_Init(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
        return;
    }

    // Step 2: Gather info from FMOD and set up the audio device
    int sampleRate = local_fmodGetInfo(0); 
    int numChannels = local_fmodGetInfo(4); 
    int bufferLengthSamples = local_fmodGetInfo(1);
    int numBuffers = local_fmodGetInfo(2); 

    if (sampleRate == 0) {
        verbose("FMODAudioDevice", "fmodGetInfo returned 0 for sample rate. Aborting.");
        return;
    }

    int sampleSize = 2; // FMOD typically uses 16-bit audio (2 bytes)
    int frameSize = numChannels * sampleSize;
    int bufferSize = bufferLengthSamples * frameSize;
    
    mAudioBuffer.resize(bufferSize);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = sampleRate;
    want.format = AUDIO_S16LSB; 
    want.channels = numChannels;
    want.samples = bufferLengthSamples; 
    want.callback = nullptr; 

    mAudioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (mAudioDevice == 0) {
        verbose("FMODAudioDevice", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return;
    }
    bd_sys_volume_poll();
    BD_LOG("AUDIO", "SDL Audio device opened. Rate: %d, Channels: %d, sysvol=%d%% (%s)",
           have.freq, have.channels, bd_sys_volume_percent(), bd_sys_volume_backend_name());

    // Publish for anyone else who needs an output device -- SDL only ever hands
    // out one, and Wwise's OpenSL sink (thunks/opensles/opensles.cpp) has to
    // reuse this one. Both engines now feed the mixer in audio_bus.cpp rather
    // than SDL's FIFO, so neither has to be muted for the other to be heard.
    bd_audio_bus_publish(mAudioDevice, have.freq, have.channels, 16);
    bd_audio_bus_set_owner(BD_AUDIO_OWNER_FMOD);
    bd_audio_bus_set_capacity(BD_AUDIO_OWNER_FMOD,
                              (size_t)have.freq * (size_t)have.channels); // ~1 s

    SDL_PauseAudioDevice(mAudioDevice, 0);

    const size_t buffer_samples = mAudioBuffer.size() / sizeof(int16_t);
    const size_t low_water = std::max<size_t>(buffer_samples / 2, 1);

    while (mRunning.load()) {
        // Unity's mixer only advances while fmodProcess() is called, and on
        // Oddmar that mixer carries the movie soundtrack (the VideoPlayer runs
        // with audioOutputMode = AudioSource, so the AAC track never reaches
        // AudioTrack -- there is no AudioTrack implementation here at all).
        // It therefore has to run for the whole session, not just while FMOD
        // happens to own the device: skipping it is exactly what made the
        // opening movie silent.
        if (bd_audio_bus_available(BD_AUDIO_OWNER_FMOD) < low_water) {
            local_fmodProcess();
            if (buffer_samples)
                bd_audio_bus_push(BD_AUDIO_OWNER_FMOD,
                                  reinterpret_cast<const int16_t*>(mAudioBuffer.data()),
                                  buffer_samples);
        }
        // Volume and mixing live in the bus so both engines are scaled once.
        bd_audio_bus_pump(mAudioDevice);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    verbose("FMODAudioDevice", "Exiting audio loop.");
    
    bd_audio_bus_unpublish(mAudioDevice);
    SDL_CloseAudioDevice(mAudioDevice);
    mAudioDevice = 0;
}

// Resolve one of org/fmod/FMODAudioDevice's registered natives and hand back its
// raw entry point.
//
// A jmethodID in jnivm is a bare Method*, and RegisterNatives() results are not
// stable: UnregisterNatives() erases registered Methods from Class::methods,
// which drops the last shared_ptr and frees them. The allocator then reuses the
// block for the next same-sized Method, so a cached id does not fail loudly --
// it silently starts naming a *different* native function. On Oddmar a cached
// fmodGetInfo id began naming fmodProcessMicData (libunity+0xafb738), which fed
// the infoId straight into JNIEnv::GetDirectBufferAddress and segfaulted inside
// UnpackJObject<ByteBuffer>'s dynamic_cast. libjnivm now pins registered natives
// (see vm.cpp), and this helper re-resolves on every call and re-checks the
// name/signature so that even a future invalidation cannot be mistaken for the
// method we asked for.
//
// The three fmod entry points never read their `thiz` argument (fmodGetInfo:
// `mov w19, w2` then a jump table over w2; fmodProcess/fmodProcessMicData:
// `mov x1, x2`), so passing the raw class pointer is equivalent and avoids
// building a throwaway jclass handle per audio callback.
static jnivm::Method* bd_fmod_native(const char* sig, const char* name,
                                     bool logMiss) {
    try {
        auto proxy = fmodClass->getMethod(sig, name);
        if (!proxy) return nullptr;
        auto* mid = (jnivm::Method*)proxy;
        if (!mid || !mid->native || mid->name != name || mid->signature != sig) {
            if (logMiss) {
                BD_LOG("AUDIO", "resolve %s %s -> rejected Method=%p name=%s sig=%s native=%p",
                       name, sig, (void*)mid,
                       mid ? mid->name.c_str() : "(null)",
                       mid ? mid->signature.c_str() : "(null)",
                       mid ? mid->native : nullptr);
            }
            return nullptr;
        }
        if (logMiss) {
            BD_LOG("AUDIO", "resolve %s %s -> Method=%p native=%p",
                   name, sig, (void*)mid, mid->native);
        }
        return mid;
    } catch (const std::exception& e) {
        BD_LOG("AUDIO", "resolve %s %s threw: %s", name, sig, e.what());
        return nullptr;
    } catch (...) {
        BD_LOG("AUDIO", "resolve %s %s threw", name, sig);
        return nullptr;
    }
}

int FMODAudioDevice::local_fmodGetInfo(int info_id) {
    FakeJni::LocalFrame frame(vm);
    auto* mid = bd_fmod_native("(I)I", "fmodGetInfo", true);
    if (!mid) {
        BD_LOG("AUDIO", "local_fmodGetInfo(id=%d): no entry point, returning 0", info_id);
        return 0;
    }
    using Fn = jint (*)(JNIEnv*, jobject, jint);
    int r = (int)((Fn)mid->native)(&frame.getJniEnv(), (jobject)fmodClass, (jint)info_id);
    BD_LOG("AUDIO", "local_fmodGetInfo(id=%d) native=%p -> %d", info_id, mid->native, r);
    return r;
}

int FMODAudioDevice::local_fmodProcess()
{
    FakeJni::LocalFrame frame(vm);

    // The direct ByteBuffer must be handed out as a *global* reference.
    //
    // jnivm owns returned object references through the ENV's local frame:
    // NewDirectByteBuffer() files the new ByteBuffer into
    // env->localframe.front(), and PopLocalFrame() clears that frame, dropping
    // the last shared_ptr. Holding the raw jobject in a function-local static
    // (the previous shape of this code) therefore caches a pointer to a
    // destroyed object: the first call is fine, every call after the frame pops
    // passes a dangling handle to org/fmod/FMODAudioDevice.fmodProcess(), whose
    // native implementation in libunity answers with
    // JNIEnv::GetDirectBufferAddress() and then memcpy()s through whatever
    // pointer that returns.
    //
    // NewGlobalRef pins the object in the VM's global list instead, so it
    // survives every local frame. The handle is rebuilt if runAudio() ever
    // reallocates mAudioBuffer and moves its data pointer.
    static jobject buffer = nullptr;
    static void* cachedData = nullptr;
    static size_t cachedSize = 0;
    if (buffer == nullptr || cachedData != mAudioBuffer.data() || cachedSize != mAudioBuffer.size()) {
        auto local = frame.getJniEnv().NewDirectByteBuffer(mAudioBuffer.data(), (jlong)mAudioBuffer.size());
        buffer = frame.getJniEnv().NewGlobalRef(local);
        cachedData = mAudioBuffer.data();
        cachedSize = mAudioBuffer.size();
        BD_LOG("AUDIO", "fmodProcess direct buffer -> data=%p size=%zu handle=%p",
                cachedData, cachedSize, (void*)buffer);
    }

    auto* mid = bd_fmod_native("(Ljava/nio/ByteBuffer;)I", "fmodProcess", false);
    if (!mid) {
        BD_LOG("AUDIO", "local_fmodProcess: no entry point, returning 0");
        return 0;
    }
    using Fn = jint (*)(JNIEnv*, jobject, jobject);
    return (int)((Fn)mid->native)(&frame.getJniEnv(), (jobject)fmodClass, buffer);
}

int FMODAudioDevice::startAudioRecord(int v1, int v2, int v3) {
    verbose("FMODAudioDevice", "startAudioRecord called but not implemented.");
    return 0;
}

void FMODAudioDevice::stopAudioRecord() {
    verbose("FMODAudioDevice", "stopAudioRecord called but not implemented.");
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::org::fmod::FMODAudioDevice)
    {FakeJni::Constructor<FMODAudioDevice>{}},
    {FakeJni::Function<&FMODAudioDevice::start>{}, "start", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&FMODAudioDevice::stop>{}, "stop", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&FMODAudioDevice::close>{}, "close", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&FMODAudioDevice::startAudioRecord>{}, "startAudioRecord", FakeJni::JMethodID::PUBLIC},
    {FakeJni::Function<&FMODAudioDevice::stopAudioRecord>{}, "stopAudioRecord", FakeJni::JMethodID::PUBLIC},
END_NATIVE_DESCRIPTOR
