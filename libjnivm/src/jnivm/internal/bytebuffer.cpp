#include <jnivm/class.h>
#include "bytebuffer.hpp"
#include "log.h"
#include <jnivm/bytebuffer.h>
#include <jnivm/jnitypes.h>

using namespace jnivm;

// Dumps the caller's frame chain. Declared in platform/common/debug_utils.h and
// defined in platform/common/debug_utils.cpp (which forwards to the same helper
// thunks/libc/misc.cpp uses for its SEGV report). Both are linked into
// unityloader, so an extern declaration is enough here and keeps libjnivm free
// of a dependency on the platform headers.
extern "C" void bd_dump_crash_backtrace(const char* tag);

// Handle tracing for the direct-buffer path, off by default.
//
// GetDirectBufferAddress() unpacks its argument with dynamic_cast, which reads
// the object's vtable. A *stale* handle (an object whose local frame was
// popped) therefore does not fail cleanly -- it dereferences a clobbered
// vtable and dies inside __dynamic_cast. Unity's native audio write path
// (libunity+0xafb760) calls this on every fmodProcess() poll and is the first
// thing to notice, so being able to see the exact handle flow matters.
//
// Set BD_DBUF_TRACE=1 to log every buffer this file hands out and every handle
// it is asked to unpack.
static bool bd_dbuf_trace() {
    static const bool on = [] {
        const char* value = std::getenv("BD_DBUF_TRACE");
        return value && *value && *value != '0';
    }();
    return on;
}

jobject jnivm::NewDirectByteBuffer(JNIEnv *env, void *buffer, jlong capacity) {
    auto res = JNITypes<std::shared_ptr<ByteBuffer>>::ToJNIType(ENV::FromJNIEnv(env), std::make_shared<ByteBuffer>(buffer, capacity));
    if (bd_dbuf_trace()) {
        LOG("BD-DBUF", "NewDirectByteBuffer data=%p capacity=%lld -> handle=%p",
            buffer, (long long)capacity, (void*)res);
    }
    return res;
}
// The JNI spec allows a null jobject here: callers use
// GetDirectBufferCapacity() == -1 as the documented probe for "this is not a
// direct buffer", and the address call is expected to just hand back NULL.
// Unpacking a null handle yields a null shared_ptr, and the old code went
// straight to ->buffer on it, so every such probe read address 0x58 (the
// ByteBuffer::buffer field offset) and took the whole process down with a
// SEGV_MAPERR. Guard the null, and also swallow the type mismatch that
// UnpackJObject raises when the handle is a real object but not a ByteBuffer --
// there is no way to report that through this API, and Unity probes it on
// arbitrary handles.
void *jnivm::GetDirectBufferAddress(JNIEnv *env, jobject bytebuffer) {
    if (bd_dbuf_trace()) {
        LOG("BD-DBUF", "GetDirectBufferAddress handle=%p from %p",
            (void*)bytebuffer, __builtin_return_address(0));
    }
    // A handle below the smallest plausible mapping is never an object. Report
    // the caller's return address so the offending call site can be identified
    // without a debugger: the loader's images live at fixed addresses
    // (unityloader 0x4000000000, libmain 0x3200000000, libil2cpp 0x3600000000,
    // libunity 0x3800000000, libAkSoundEngine 0x3a00000000), so the logged
    // address minus the owning base is directly feedable to addr2line.
    if (bytebuffer != nullptr && (uintptr_t)bytebuffer < 0x1000) {
        LOG("BD-DBUF", "GetDirectBufferAddress got non-pointer handle %p from %p",
            (void*)bytebuffer, __builtin_return_address(0));
        bd_dump_crash_backtrace("BD-DBUF");
    }
    if (bytebuffer == nullptr) {
        return nullptr;
    }
    try {
        auto buf = JNITypes<std::shared_ptr<ByteBuffer>>::JNICast(ENV::FromJNIEnv(env), bytebuffer);
        return buf ? buf->buffer : nullptr;
    } catch (...) {
        return nullptr;
    }
}
jlong jnivm::GetDirectBufferCapacity(JNIEnv *env, jobject bytebuffer) {
    if (bd_dbuf_trace()) {
        LOG("BD-DBUF", "GetDirectBufferCapacity handle=%p from %p",
            (void*)bytebuffer, __builtin_return_address(0));
    }
    if (bytebuffer == nullptr) {
        return -1;
    }
    try {
        auto buf = JNITypes<std::shared_ptr<ByteBuffer>>::JNICast(ENV::FromJNIEnv(env), bytebuffer);
        return buf ? buf->capacity : -1;
    } catch (...) {
        return -1;
    }
}