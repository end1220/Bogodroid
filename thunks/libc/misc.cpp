#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>

#include "platform.h"
#include "logging.h"
#include "so_util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <elf.h>
#include <execinfo.h>
#include <inttypes.h>
#include <link.h>
#include <stdbool.h>
#include <atomic>




#include "bionic_file.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <cstring>

extern "C" ABI_ATTR int login_tty_impl(int fd)
{
    return -1;
}

extern "C" long syscall_impl(long number,
    long arg1, long arg2, long arg3,
    long arg4, long arg5, long arg6)
{
    if (number == 0xb2) { // __NR_gettid for aarch64
        // A simple passthrough for gettid
        return syscall(SYS_gettid);
    }

    if (number == 0x62) { // __NR_futex for aarch64
        // Cast arguments to their expected types for futex
        int* uaddr = (int*)arg1;
        int op = (int)arg2;
        unsigned int val = (unsigned int)arg3;
        const struct timespec* timeout = (const struct timespec*)arg4;
        int* uaddr2 = (int*)arg5;
        unsigned int val3 = (unsigned int)arg6;

        return syscall(SYS_futex, uaddr, op, val, timeout, uaddr2, val3);
    }

    {
        verbose("SYSCALLS","Unimplemented syscall: %ld\n", number);
    }

    return syscall(number, arg1, arg2, arg3, arg4, arg5, arg6);
}

extern "C" ABI_ATTR void abort_impl(void)
{
    fatal_error("Guest called abort!\n");
    abort();
    // exit(-1);
}

extern "C" ABI_ATTR void* dlopen_impl(const char* filename, int flags)
{
    BD_DEBUG("DLOPEN", "%s", filename ? filename : "(null)");

    if (filename == NULL)
        return NULL;

    const char* base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    // ── Libraries this host deliberately does not provide ──
    //
    // Everything unknown falls through to the 0xDEAD sentinel below, meaning
    // "not a real module, but not a hard failure either" so that a later dlsym
    // still reaches the global thunk tables. That is right for shims we do
    // provide (OpenSL ES) and wrong for APIs we do not, because some callers
    // only test whether the handle is non-NULL.
    //
    //   Wwise's Android sink factory (libAkSoundEngine.so+0x2278f8) decides
    //   between AAudio and OpenSL ES by evaluating `obj[0x228+8] != NULL` --
    //   the cached dlopen handle. Handed 0xDEAD it concludes AAudio is usable,
    //   commits to that sink, and then every one of the ~27 AAudioStream*
    //   lookups misses. The engine does *not* retry OpenSL: it drops to
    //   "Hardware audio subsystem stopped responding. Silent mode is enabled."
    //   Returning NULL is both truthful and the thing that triggers the
    //   fallback to OpenSL ES, which thunks/opensles bridges onto SDL.
    if (strcmp(base, "libaaudio.so") == 0) {
        // A/B escape hatch: BD_AAUDIO_SENTINEL=1 restores the old 0xDEAD
        // behaviour so the two can be compared without a rebuild.
        if (getenv("BD_AAUDIO_SENTINEL") != NULL) {
            BD_LOG("DLOPEN", "libaaudio.so -> sentinel (BD_AAUDIO_SENTINEL set; old behaviour)");
        } else {
            BD_LOG("DLOPEN", "libaaudio.so -> NULL (host has no AAudio; caller must fall back to OpenSL ES)");
            return NULL;
        }
    }

    // OpenSL ES, by contrast, is a shim we do serve -- hand back the sentinel and
    // let dlsym_impl decide who is allowed to see the symbols.
    if (strstr(base, "OpenSL") != NULL)
        BD_LOG("DLOPEN", "%s -> sentinel (OpenSL ES is a host shim; dlsym decides who gets it)", base);
#ifdef FAKE_EGL
    // Distinct from EGL display/context sentinels. Unity dlopens libEGL.so after
    // we already created the SDL GLES context; handing back 0xDEAD collided with
    // the old fake EGL handles.
    if (strcmp(base, "libEGL.so") == 0) {
        static char kFakeLibEGL;
        BD_LOG("DLOPEN", "libEGL.so -> fake handle (do not load system EGL)");
        return &kFakeLibEGL;
    }
    if (strcmp(base, "libGLESv2.so") == 0 || strcmp(base, "libGLESv3.so") == 0) {
        static char kFakeLibGLES;
        BD_LOG("DLOPEN", "%s -> fake handle", base);
        return &kFakeLibGLES;
    }
#endif

    char resolved1[PATH_MAX];
    char resolved2[PATH_MAX];
    realpath(filename, resolved1);

    so_module* head = so_get_head();
    while(head)
    {
        realpath(head->path, resolved2);
        if (strcmp(resolved1, resolved2) == 0)
            return head;
        head = head->next;
    }

    // [BD] Fallback: match on basename.
    //
    // IL2CPP resolves [DllImport("Foo")] with a bare soname -- dlopen("libFoo.so")
    // -- and expects the platform linker to search the app's native library
    // directory for it. realpath() cannot resolve that name from the game's
    // working directory, so the exact-path loop above never hits and we would
    // report failure for a library that is in fact already mapped. Comparing
    // basenames lets libunity/libil2cpp (which the loader always preloads) and
    // any eagerly loaded plugin answer those requests.
    {
        so_module* byName = so_get_head();
        while (byName)
        {
            if (byName->path) {
                const char* otherSlash = strrchr(byName->path, '/');
                const char* otherBase = otherSlash ? otherSlash + 1 : byName->path;
                if (strcmp(base, otherBase) == 0) {
                    BD_DEBUG("DLOPEN", "%s -> preloaded module by basename", base);
                    return byName;
                }
            }
            byName = byName->next;
        }
    }

    return (void*)0xDEAD;
}

extern "C" ABI_ATTR char* dlerror_impl(void)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int dlclose_impl(void* handle)
{
    /* ... */
    return 0;
}

extern "C" ABI_ATTR int dladdr_impl(const void* addr, Dl_info* info)
{
    /* THIS IS TERRIBLE LOL */
    WARN_STUB
    return 0;
}

// ───────── OpenSL ES: hand it to Wwise, and only to Wwise ─────────
//
// thunks/opensles/opensles.cpp bridges the OpenSL ES buffer-queue pipeline onto
// SDL. Two very different consumers probe for it, and both do it the same way:
// dlopen("libOpenSLES.so") followed by dlsym(slCreateEngine).
//
//   * libAkSoundEngine.so (Wwise) genuinely needs it. Its Android sink
//     (AkSink_OpenSL) is what feeds the hardware watchdog; without OpenSL,
//     AK::SoundEngine::Init still returns success and every counter looks
//     healthy, but the watchdog fires a few seconds in and the engine drops to
//     "Hardware audio subsystem stopped responding. Silent mode is enabled."
//     Result: a silent game that appears to have working audio.
//
//   * libunity.so also probes OpenSL for its built-in FMOD, but this is a trap.
//     When FMOD gets slCreateEngine it selects OpenSL as its output, fails to
//     make progress against the shim, and gives up *without* falling back. That
//     costs us the path that already works: the Unity audio device via
//     org.fmod.FMODAudioDevice -> javastubs/fakefmod.cpp -> the shared mixer
//     (platform/common/audio_bus.cpp).
//     So a globally-visible shim would break working audio to fix silent audio.
//
// so_resolve_link() is a flat global lookup with no notion of the caller, so the
// split has to be made here, on the caller's identity. Note the static UND
// OpenSL references inside libAkSoundEngine.so need no handling: relocations are
// resolved with the requesting module's own identity, so only the module that
// carries them can reach them.
static bool bd_is_opensl_symbol(const char* name)
{
    if (name == NULL) return false;
    if (strcmp(name, "slCreateEngine") == 0) return true;
    return strncmp(name, "SL_IID_", 7) == 0;
}

static bool bd_caller_is_ak_sound_engine(void* return_address)
{
    so_module* m = so_module_containing((uintptr_t)return_address);
    if (m == NULL || m->path == NULL) return false;
    const char* base = strrchr(m->path, '/');
    base = base ? base + 1 : m->path;
    return strncmp(base, "libAkSoundEngine", 16) == 0;
}

extern "C" ABI_ATTR void* dlsym_impl(void* handle, const char* name)
{
    // AAudio is probed the same silent way -- one dlopen plus a dlsym per entry
    // point -- and nothing on this host implements it. Log a bounded sample so
    // the attempt leaves *some* trace in a LOG-only build; otherwise the whole
    // path is invisible (dlopen logs at BD_DEBUG, which is compiled out here)
    // and AAudio looks indistinguishable from "never tried".
    if (name != NULL && strncmp(name, "AAudio", 6) == 0) {
        static std::atomic<uint32_t> aaudio_lookups{0};
        uint32_t n = ++aaudio_lookups;
        if (n <= 8 || (n % 100) == 0)
            BD_LOG("DLSYM", "AAudio %s -> no host implementation (lookup #%u)", name, (unsigned)n);
    }

    if (bd_is_opensl_symbol(name)) {
        // Read the caller's return address *here*, while we are still the callee.
        void* ra = __builtin_return_address(0);
        if (!bd_caller_is_ak_sound_engine(ra)) {
            BD_LOG("DLSYM", "OpenSL %s withheld from %p (caller is not libAkSoundEngine; FMOD stays on AudioTrack->SDL)",
                   name, ra);
            return NULL;
        }
        BD_LOG("DLSYM", "OpenSL %s granted to libAkSoundEngine (caller %p)", name, ra);
    }

    void* result = (void*)so_resolve_link((so_module*)handle, name);
    if (name && strncmp(name, "AMedia", 6) == 0)
        BD_LOG("DLSYM", "%s -> %p", name, result);
    return result;
}

extern "C" ABI_ATTR const void*
memchr_impl(const void* __s, int __c, size_t __n)
{
    return __builtin_memchr(__s, __c, __n);
}

extern "C" ABI_ATTR int sigsetmask_impl(int mask)
{
    WARN_STUB
    return -1;
}

extern "C" ABI_ATTR char* tempnam_impl(const char* dir, const char* pfx)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char* tmpnam_impl(char* s)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR char* mktemp_impl(char* _template)
{
    WARN_STUB
    return NULL;
}

extern "C" ABI_ATTR int* __errno_impl(void)
{
    return __errno_location();
}

// Android's logging entry points.
//
// These used to go through warning(), which only exists when BD_ENABLE_TRACE is
// on. The build that ships is LOG-only, TRACE off -- so all three compiled to
// ((void)0) and every line Unity said, including its managed-exception
// reporting, was dropped on the floor. Route them through BD_LOG instead, under
// the ANDROID category, so they land in the layer that actually ships on the
// handheld. BD_ANDROID_LOG=0 silences them for a throughput run, the same way
// BD_MEM_LOG_MS controls the RSS sampler.
static bool bd_android_log_enabled()
{
    static const bool enabled = [] {
        const char* value = getenv("BD_ANDROID_LOG");
        return !(value && *value && strcmp(value, "0") == 0);
    }();
    return enabled;
}

static void bd_android_log_out(const char* tag, const char* text)
{
    if (!bd_android_log_enabled())
        return;
    BD_LOG("ANDROID", "%s: %s", tag ? tag : "?", text ? text : "");
}

extern "C" ABI_ATTR int __android_log_write_impl(int prio, const char* tag, const char* text)
{
    bd_android_log_out(tag, text);
    return 1;
}

extern "C" ABI_ATTR int __android_log_print_impl(int prio, const char* tag, const char* fmt, ...)
{
    char andlog[2048] = {};
    va_list va;
    va_start(va, fmt);
    int r = vsnprintf(andlog, sizeof(andlog) - 1, fmt, va);
    va_end(va);
    bd_android_log_out(tag, andlog);
    return r;
}

extern "C" ABI_ATTR int __android_log_vprint_impl(int prio, const char* tag, const char* fmt, va_list va)
{
    char andlog[2048] = {};
    int r = vsnprintf(andlog, sizeof(andlog) - 1, fmt, va);
    bd_android_log_out(tag, andlog);
    return r;
}

extern "C" ABI_ATTR const char* __strchr_chk(const char* __s, int __ch, size_t __n) { return strchr(__s, __ch); }
extern "C" ABI_ATTR const char* __strrchr_chk(const char* __s, int __ch, size_t __n) { return strrchr(__s, __ch); }
extern "C" ABI_ATTR size_t __strlen_chk(const char* __s, size_t __n) { return strnlen(__s, __n); }

// Resolve one address to "module+offset" for the backtrace dumps below
// (Bionic's abort hook and the SIGSEGV handler in platform/common/debug_utils.cpp).
//
// dladdr() only knows about libraries the dynamic loader mapped; the game's .so
// files are mapped by hand (so_util) and are invisible to it, so fall back to
// so_util's own module list. An offset is all that is needed: the symbols stay
// in the local unstripped build, and addr2line/llvm-symbolizer turn
// "libunity.so+0xcbe1bc" back into a function name offline.
static void bd_describe_address(void* address, char* out, size_t out_size)
{
    const uintptr_t addr = (uintptr_t)address;

    Dl_info info;
    if (dladdr(address, &info) && info.dli_fname && info.dli_fbase &&
        (uintptr_t)info.dli_fbase <= addr) {
        snprintf(out, out_size, "%s+0x%zx", info.dli_fname,
                 (size_t)(addr - (uintptr_t)info.dli_fbase));
        return;
    }

    so_module* best = nullptr;
    for (so_module* mod = so_get_head(); mod; mod = mod->next) {
        if (mod->base && mod->base <= addr && (!best || mod->base > best->base))
            best = mod;
    }
    if (best)
        snprintf(out, out_size, "%s+0x%zx",
                 best->path ? best->path : "(game module)",
                 (size_t)(addr - best->base));
    else
        snprintf(out, out_size, "0x%" PRIxPTR, addr);
}

// Bionic's abort-message hook - the last thing a C++ runtime says before it
// aborts. Unity's own libc++_shared answers a virtual call that lands on a pure
// virtual slot here ("Pure virtual function called!"), and by the time the
// tombstone is written the interesting frame is gone: the dump then reads
// "abort somewhere", with the loader's abort hook as the topmost symbol and no
// hint of which interface or which caller. Unwind while it is still on the
// stack instead, and print module+offset per frame.
// Upper bound of a module's mapping, used to decide whether a stray stack word
// can possibly be a return address into it.
static uintptr_t bd_module_end(const so_module* mod)
{
    uintptr_t end = mod->base;
    if (mod->text_base) {
        const uintptr_t e = mod->text_base + mod->text_size;
        if (e > end) end = e;
    }
    if (mod->patch_base) {
        const uintptr_t e = mod->patch_base + mod->patch_size;
        if (e > end) end = e;
    }
    if (mod->cave_base) {
        const uintptr_t e = mod->cave_base + mod->cave_size;
        if (e > end) end = e;
    }
    for (int i = 0; i < mod->n_data; ++i) {
        const uintptr_t e = mod->data_base[i] + mod->data_size[i];
        if (e > end) end = e;
    }
    return end;
}

static so_module* bd_find_module(uintptr_t addr)
{
    so_module* best = nullptr;
    for (so_module* mod = so_get_head(); mod; mod = mod->next) {
        if (!mod->base || mod->base > addr) continue;
        if (addr >= bd_module_end(mod)) continue;
        if (!best || mod->base > best->base) best = mod;
    }
    return best;
}

// Is *insn the call-site encoding that could branch to `scan`? On AArch64 a
// return address is preceded by BL (direct) or BLR (indirect virtual call).
// Requiring this filters out the majority of stack noise (saved vtable/data
// pointers that happen to land inside a mapping).
static bool bd_is_call_site(const uint32_t* insn)
{
    const uint32_t v = *insn;
    if ((v & 0xFC000000u) == 0x94000000u) return true;  // bl  #imm26
    if ((v & 0xFFFFFC1Fu) == 0xD63F0000u) return true;  // blr xn
    if ((v & 0xFE000000u) == 0x14000000u) return true;  // b   #imm26 (tail call)
    return false;
}

// The unwinder gives up one frame short of the interesting one: the pure virtual
// call is the frame *above* libc++'s __cxa_pure_virtual, and it is that caller
// (a virtual dispatch on a destroyed object inside libunity) we need to name.
//
// The C++ backtrace() walk stops there because libunity's own frames carry no
// unwind info our runtime can use, so rebuild the chain by hand: first walk the
// aarch64 frame-pointer chain (Unity is built with frame pointers on), then fall
// back to a conservative stack scan for words that look like return addresses.
static void bd_scan_caller_frames(const char* tag)
{
    uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
    char where[512];

    fprintf(stderr, "%s: -- fp chain from 0x%zx --\n", tag, (size_t)fp);
    int shown = 0;
    for (int i = 0; i < 64 && fp && (fp & 7) == 0; ++i) {
        const uintptr_t* frame = (const uintptr_t*)fp;
        const uintptr_t next = frame[0];
        const uintptr_t ret = frame[1];
        if (ret) {
            bd_describe_address((void*)ret, where, sizeof(where));
            fprintf(stderr, "%s:   fp#%02d %s\n", tag, i, where);
            ++shown;
        }
        if (next <= fp || next - fp > 1u << 20) break;
        fp = next;
        if (shown >= 40) break;
    }

    // Conservative scan: any word in the first 48 KB of stack that points into a
    // loaded module and sits right after a call instruction is a candidate.
    fprintf(stderr, "%s: -- stack scan --\n", tag);
    const uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    shown = 0;
    for (uintptr_t p = sp; p < sp + (48u << 10) && shown < 40; p += sizeof(uintptr_t)) {
        const uintptr_t value = *(const uintptr_t*)p;
        so_module* mod = bd_find_module(value);
        if (!mod || value < 8 || (value & 3)) continue;
        if (!bd_find_module(value - 4)) continue;  // call site must be in a module too
        if (!bd_is_call_site((const uint32_t*)(value - 4))) continue;
        bd_describe_address((void*)value, where, sizeof(where));
        fprintf(stderr, "%s:   @%04zx %s\n", tag, (size_t)(p - sp), where);
        ++shown;
    }
    fflush(stderr);
}

static void bd_dump_backtrace(const char* tag)
{
    void* frames[48];
    const int count = backtrace(frames, 48);
    fprintf(stderr, "%s: tid=%d frames=%d\n", tag, (int)syscall(__NR_gettid), count);
    for (int i = 0; i < count; ++i) {
        char where[512];
        bd_describe_address(frames[i], where, sizeof(where));
        fprintf(stderr, "%s:   #%02d %s\n", tag, i, where);
    }
    bd_scan_caller_frames(tag);
    fflush(stderr);
}

// Shared entry point for platform/common/debug_utils.cpp's SIGSEGV handler
// (declared in debug_utils.h; kept out of the include graph on purpose -- this
// file only needs the extern "C" name to match).
//
// Unity installs its own handler for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT from
// libunity's JNI_OnLoad and chains into the one it replaced, which is ours. By
// the time we run, the stack belongs to Unity's handler, so backtrace(3)'s
// frame-pointer walk cannot be trusted there either -- exactly the situation
// bd_scan_caller_frames() was written for.
extern "C" void bd_dump_crash_backtrace(const char* tag)
{
    bd_dump_backtrace(tag);
}

extern "C" void bd_describe_crash_address(const char* tag, const char* label,
                                          unsigned long long addr)
{
    char where[512];
    bd_describe_address((void*)(uintptr_t)addr, where, sizeof(where));
    fprintf(stderr, "%s:   %s %s\n", tag, label, where);
    fflush(stderr);
}

extern "C" ABI_ATTR void android_set_abort_message_impl(const char* msg)
{
    bd_dump_backtrace("BD-ABORT");
    fatal_error("%s", msg);
    //   abort();
}

extern "C" ABI_ATTR int __system_property_get_impl(const char* name, char* value)
{
    if (!value) return 0;
    // Unity VideoPlayer gates compressed AssetBundle clips on API 29+ and
    // reads this property (not only Build.VERSION.SDK_INT).
    static const struct { const char* key; const char* val; } kProps[] = {
        {"ro.build.version.sdk", "29"},
        {"ro.build.version.release", "10"},
        {"ro.product.model", "h700"},
        {"ro.product.manufacturer", "Allwinner"},
        {"ro.hardware", "sun50iw9"},
        {"ro.product.cpu.abi", "arm64-v8a"},
        {"ro.product.cpu.abilist", "arm64-v8a"},
        {"ro.product.cpu.abilist64", "arm64-v8a"},
        {"ro.product.cpu.abilist32", ""},
    };
    if (name) {
        for (const auto& prop : kProps) {
            if (strcmp(name, prop.key) == 0) {
                size_t len = strlen(prop.val);
                memcpy(value, prop.val, len + 1);
                BD_LOG("PROP", "%s=%s", name, prop.val);
                return static_cast<int>(len);
            }
        }
        BD_LOG("PROP", "%s=(empty)", name);
    }
    value[0] = 0;
    return 0;
}

// libunity sometimes passes NULL from internal failures straight into libc
// string ops, which then segfault. These wrappers return safe sentinels.
extern "C" ABI_ATTR size_t strlen_safe_impl(const char* s)
{
    if (!s) {
        static std::atomic<int> n{0};
        int cur = ++n;
        if (cur <= 5) {
            BD_DEBUG("STRGUARD", "strlen(NULL) #%d (returning 0)", cur);
        }
        return 0;
    }
    return strlen(s);
}

extern "C" ABI_ATTR char* strchr_safe_impl(const char* s, int c)
{
    if (!s) return nullptr;
    return (char*)strchr(s, c);
}

extern "C" ABI_ATTR char* strrchr_safe_impl(const char* s, int c)
{
    if (!s) return nullptr;
    return (char*)strrchr(s, c);
}

extern "C" ABI_ATTR int strcmp_safe_impl(const char* a, const char* b)
{
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strcmp(a, b);
}

extern "C" ABI_ATTR int strncmp_safe_impl(const char* a, const char* b, size_t n)
{
    if (n == 0) return 0;
    if (!a && !b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strncmp(a, b, n);
}

extern "C" ABI_ATTR char* strstr_safe_impl(const char* haystack, const char* needle)
{
    if (!haystack || !needle) return nullptr;
    return (char*)strstr(haystack, needle);
}

extern "C" ABI_ATTR const void* __system_property_find_impl(const char* name)
{
    WARN_STUB;
    return nullptr;
}

extern "C" ABI_ATTR int __system_property_read_impl(const void* pi, char* name, char* value)
{
    WARN_STUB;
    if (name) name[0] = 0;
    if (value) value[0] = 0;
    return 0;
}

extern "C" ABI_ATTR void syslog_impl(int priority, const char* format, ...)
{
    WARN_STUB;
}

ABI_ATTR int open_impl(const char *filename, int flags, mode_t mode);
extern "C" ABI_ATTR int __open_2_impl(const char* pathname, int flags)
{
    return open_impl(pathname, flags, NULL);
}
char* clean_jar_path(const char* path);

// Taken from https://github.com/libhybris/libhybris/blob/master/hybris/common/hooks.c
extern "C" char* bd_redirect_system_fonts(const char* path); // from fcntl.cpp

ABI_ATTR int scandirat_impl(int fd, const char* dir,
    struct bionic_dirent*** namelist,
    int (*filter)(const struct bionic_dirent*),
    int (*compar)(const struct bionic_dirent**,
        const struct bionic_dirent**))
{
    char* fonts_redirect = bd_redirect_system_fonts(dir);
    char* clean_path = fonts_redirect ? fonts_redirect : clean_jar_path(dir);
    struct dirent** namelist_r;
    struct bionic_dirent** result;
    struct bionic_dirent* filter_r;

    int i = 0;
    size_t nItems = 0;

    int res = scandirat(fd, clean_path, &namelist_r, NULL, NULL);

    if (res > 0 && namelist_r != NULL) {
        result = (bionic_dirent**)malloc(res * sizeof(struct bionic_dirent));
        if (!result)
            return -1;

        for (i = 0; i < res; i++) {
            filter_r = (bionic_dirent*)malloc(sizeof(struct bionic_dirent));
            if (!filter_r) {
                while (i-- > 0)
                    free(result[i]);
                free(result);
                return -1;
            }

            filter_r->d_ino = namelist_r[i]->d_ino;
            filter_r->d_off = namelist_r[i]->d_off;
            filter_r->d_reclen = namelist_r[i]->d_reclen;
            filter_r->d_type = namelist_r[i]->d_type;

            strcpy(filter_r->d_name, namelist_r[i]->d_name);
            filter_r->d_name[sizeof(namelist_r[i]->d_name) - 1] = '\0';

            if (filter != NULL && !(*filter)(filter_r)) { // apply filter
                free(filter_r);
                continue;
            }

            result[nItems++] = filter_r;
        }

        if (nItems && compar != NULL) // sort
            qsort(result, nItems, sizeof(struct bionic_dirent*), (__compar_fn_t)compar);

        *namelist = result;
    } else {
        return res;
    }

    return nItems;
}

ABI_ATTR int scandir_impl(const char* dir,
    struct bionic_dirent*** namelist,
    int (*filter)(const struct bionic_dirent*),
    int (*compar)(const struct bionic_dirent**,
        const struct bionic_dirent**))
{
    return scandirat_impl(AT_FDCWD, dir, namelist, filter, compar);
}

ABI_ATTR int prctl_impl(int op, int arg1, int arg2, int arg3)
{
    return 0;
}

#ifndef LOG_DLPI
#define LOG_DLPI 0
#endif

#define DLPI_LOG(fmt, ...)                                      \
    do {                                                        \
        if (LOG_DLPI)                                           \
            fprintf(stderr, "[dlpi] " fmt "\n", ##__VA_ARGS__); \
    } while (0)

static const char* phdr_type_name(ElfW(Word) t)
{
    switch (t) {
    case PT_NULL:
        return "PT_NULL";
    case PT_LOAD:
        return "PT_LOAD";
    case PT_DYNAMIC:
        return "PT_DYNAMIC";
    case PT_INTERP:
        return "PT_INTERP";
    case PT_NOTE:
        return "PT_NOTE";
    case PT_SHLIB:
        return "PT_SHLIB";
    case PT_PHDR:
        return "PT_PHDR";
    case PT_TLS:
        return "PT_TLS";
    case 0x6474e550u:
        return "PT_GNU_EH_FRAME";
    case 0x6474e551u:
        return "PT_GNU_STACK";
    case 0x6474e552u:
        return "PT_GNU_RELRO";
    default:
        return "PT_<other>";
    }
}

// Thread-local scratch to hold a normalized PHDR view per thread during callback
static thread_local ElfW(Phdr) tl_phdr_scratch[1024];
static inline struct dl_phdr_info make_dl_phdr_info(const struct so_module* m, bool is_main_exe)
{
    struct dl_phdr_info info;
    memset(&info, 0, sizeof(info));

    // This is the correct base address where the library was loaded.
    ElfW(Addr) load_bias = (ElfW(Addr))m->base;

    const char* name = is_main_exe ? "" : (m->soname ? m->soname : "");
    DLPI_LOG("module=%p name=\"%s\" is_main=%d", (void*)m, name, is_main_exe);

    if (!m->ehdr || !m->phdr) {
        DLPI_LOG("ERROR: missing EHDR/PHDR pointers");
        return info;
    }

    ElfW(Half) phnum = m->ehdr->e_phnum;
    if (phnum == 0 || phnum > (sizeof(tl_phdr_scratch) / sizeof(tl_phdr_scratch[0]))) {
        DLPI_LOG("ERROR: phnum=%u out of bounds for scratch buffer", (unsigned)phnum);
        return info;
    }

    for (ElfW(Half) i = 0; i < phnum; i++) {
        tl_phdr_scratch[i] = m->phdr[i]; // Make a copy
        if (tl_phdr_scratch[i].p_vaddr >= load_bias) {
            tl_phdr_scratch[i].p_vaddr -= load_bias;
        }
    }

    // Fill the info struct according to the API contract
    info.dlpi_addr = load_bias;
    info.dlpi_phdr = tl_phdr_scratch; // Point to our corrected, relative headers
    info.dlpi_phnum = phnum;
    info.dlpi_name = name;

    DLPI_LOG("REPORTING: dlpi_addr(load_bias)=0x%" PRIxPTR, (uintptr_t)info.dlpi_addr);

    // Now, log the values as the unwinder will see and calculate them
    bool saw_eh = false;
    for (ElfW(Half) i = 0; i < phnum; i++) {
        const ElfW(Phdr)* ph = &info.dlpi_phdr[i];
        // This calculation should now yield the correct runtime address
        ElfW(Addr) runtime_start = info.dlpi_addr + ph->p_vaddr;
        DLPI_LOG("PHDR[%u]: type=%s p_vaddr(rel)=0x%" PRIxPTR " -> runtime_addr=0x%" PRIxPTR,
            (unsigned)i, phdr_type_name(ph->p_type), (uintptr_t)ph->p_vaddr, (uintptr_t)runtime_start);
        if (ph->p_type == 0x6474e550u) { // PT_GNU_EH_FRAME
            saw_eh = true;
            DLPI_LOG("--> PT_GNU_EH_FRAME found, runtime location will be 0x%" PRIxPTR, (uintptr_t)runtime_start);
        }
    }
    if (!saw_eh)
        DLPI_LOG("INFO: PT_GNU_EH_FRAME not present");

    return info;
}

struct hybrid_state {
    // The original callback and data from the unwinder
    int (*original_callback)(struct dl_phdr_info* info, size_t size, void* data);
    void* original_data;

    // A list of our custom modules
    const struct so_module* guest_modules_head;
};

// This is a new callback that we will pass to the REAL dl_iterate_phdr
static int hybrid_callback(struct dl_phdr_info* info, size_t size, void* data)
{
    struct hybrid_state* state = (struct hybrid_state*)data;

    // Pass the host module info to the unwinder's original callback
    return state->original_callback(info, size, state->original_data);
}

extern "C" ABI_ATTR int dl_iterate_phdr_impl(
    int (*callback)(struct dl_phdr_info* info, size_t size, void* data),
    void* data)
{
    if (!callback)
        return -1;

    DLPI_LOG("dl_iterate_phdr_impl start");

    struct hybrid_state state;
    state.original_callback = callback;
    state.original_data = data;

    DLPI_LOG("dl_iterate_phdr_impl call real dl_iterate_phdr");
    int ret = dl_iterate_phdr(hybrid_callback, &state);
    DLPI_LOG("dl_iterate_phdr_impl real dl_iterate_phdr returns %d",ret);

    // If the original callback asked to stop, we must respect that.
    if (ret != 0) {
        DLPI_LOG("dl_iterate_phdr_impl end early from real function");
        return ret;
    }

    const struct so_module* head = so_get_head();
    const struct so_module* m = head;
    bool is_first = false;

    for (; m != NULL; m = m->next, is_first = false) {
        struct dl_phdr_info info = make_dl_phdr_info(m, is_first);
        // Optional fields like dlpi_adds/subs/tls_* can remain zeroed; callers size-check via 'size'.
        DLPI_LOG("dl_iterate_phdr_impl call callback");
        ret = callback(&info, sizeof(info), data);
        DLPI_LOG("dl_iterate_phdr_impl callback returned %d", ret);
        if (ret != 0)
            break; // stop early if callback asks to stop
    }
    DLPI_LOG("dl_iterate_phdr_impl end");
    return ret; // 0 if all callbacks returned 0, or the callback's nonzero value
}


// Skip C++ dtors / atexit / Mali release on shutdown — the cleanup cascade
// alloc-spikes and OOMs 1GB devices. Flush prefs first (small sync write).
namespace jnivm { namespace android { namespace content { class SharedPreferences; } } }
extern "C" void bd_flush_prefs_impl();

extern "C" ABI_ATTR void exit_impl(int status)
{
    BD_LOG("EXIT", "guest exit() intercepted -> flush + _exit");
    bd_flush_prefs_impl();
    _exit(status);
}

extern "C" ABI_ATTR void __assert_impl(const char *expression, const char *file, int line) {
    fatal_error("Guest assertion failed: %s, file %s, line %d\n", expression, file, line);
    abort();
}

extern ABI_ATTR long sysconf_impl(int name)
{
switch (name) {
    case 0x0000: return sysconf(_SC_ARG_MAX);
    case 0x0001: return sysconf(_SC_BC_BASE_MAX);
    case 0x0002: return sysconf(_SC_BC_DIM_MAX);
    case 0x0003: return sysconf(_SC_BC_SCALE_MAX);
    case 0x0004: return sysconf(_SC_BC_STRING_MAX);
    case 0x0005: return sysconf(_SC_CHILD_MAX);
    case 0x0006: return sysconf(_SC_CLK_TCK);
    case 0x0007: return sysconf(_SC_COLL_WEIGHTS_MAX);
    case 0x0008: return sysconf(_SC_EXPR_NEST_MAX);
    case 0x0009: return sysconf(_SC_LINE_MAX);
    case 0x000a: return sysconf(_SC_NGROUPS_MAX);
    case 0x000b: return sysconf(_SC_OPEN_MAX);
    case 0x000c: return sysconf(_SC_PASS_MAX);
    case 0x000d: return sysconf(_SC_2_C_BIND);
    case 0x000e: return sysconf(_SC_2_C_DEV);
    case 0x000f: return sysconf(_SC_2_C_VERSION);
    case 0x0010: return sysconf(_SC_2_CHAR_TERM);
    case 0x0011: return sysconf(_SC_2_FORT_DEV);
    case 0x0012: return sysconf(_SC_2_FORT_RUN);
    case 0x0013: return sysconf(_SC_2_LOCALEDEF);
    case 0x0014: return sysconf(_SC_2_SW_DEV);
    case 0x0015: return sysconf(_SC_2_UPE);
    case 0x0016: return sysconf(_SC_2_VERSION);
    case 0x0017: return sysconf(_SC_JOB_CONTROL);
    case 0x0018: return sysconf(_SC_SAVED_IDS);
    case 0x0019: return sysconf(_SC_VERSION);
    case 0x001a: return sysconf(_SC_RE_DUP_MAX);
    case 0x001b: return sysconf(_SC_STREAM_MAX);
    case 0x001c: return sysconf(_SC_TZNAME_MAX);
    case 0x001d: return sysconf(_SC_XOPEN_CRYPT);
    case 0x001e: return sysconf(_SC_XOPEN_ENH_I18N);
    case 0x001f: return sysconf(_SC_XOPEN_SHM);
    case 0x0020: return sysconf(_SC_XOPEN_VERSION);
    case 0x0021: return sysconf(_SC_XOPEN_XCU_VERSION);
    case 0x0022: return sysconf(_SC_XOPEN_REALTIME);
    case 0x0023: return sysconf(_SC_XOPEN_REALTIME_THREADS);
    case 0x0024: return sysconf(_SC_XOPEN_LEGACY);
    case 0x0025: return sysconf(_SC_ATEXIT_MAX);
    case 0x0026: return sysconf(_SC_IOV_MAX);
    case 0x0027: return sysconf(_SC_PAGESIZE);
    case 0x0028: return sysconf(_SC_PAGE_SIZE);
    case 0x0029: return sysconf(_SC_XOPEN_UNIX);
    case 0x002a: return sysconf(_SC_XBS5_ILP32_OFF32);
    case 0x002b: return sysconf(_SC_XBS5_ILP32_OFFBIG);
    case 0x002c: return sysconf(_SC_XBS5_LP64_OFF64);
    case 0x002d: return sysconf(_SC_XBS5_LPBIG_OFFBIG);
    case 0x002e: return sysconf(_SC_AIO_LISTIO_MAX);
    case 0x002f: return sysconf(_SC_AIO_MAX);
    case 0x0030: return sysconf(_SC_AIO_PRIO_DELTA_MAX);
    case 0x0031: return sysconf(_SC_DELAYTIMER_MAX);
    case 0x0032: return sysconf(_SC_MQ_OPEN_MAX);
    case 0x0033: return sysconf(_SC_MQ_PRIO_MAX);
    case 0x0034: return sysconf(_SC_RTSIG_MAX);
    case 0x0035: return sysconf(_SC_SEM_NSEMS_MAX);
    case 0x0036: return sysconf(_SC_SEM_VALUE_MAX);
    case 0x0037: return sysconf(_SC_SIGQUEUE_MAX);
    case 0x0038: return sysconf(_SC_TIMER_MAX);
    case 0x0039: return sysconf(_SC_ASYNCHRONOUS_IO);
    case 0x003a: return sysconf(_SC_FSYNC);
    case 0x003b: return sysconf(_SC_MAPPED_FILES);
    case 0x003c: return sysconf(_SC_MEMLOCK);
    case 0x003d: return sysconf(_SC_MEMLOCK_RANGE);
    case 0x003e: return sysconf(_SC_MEMORY_PROTECTION);
    case 0x003f: return sysconf(_SC_MESSAGE_PASSING);
    case 0x0040: return sysconf(_SC_PRIORITIZED_IO);
    case 0x0041: return sysconf(_SC_PRIORITY_SCHEDULING);
    case 0x0042: return sysconf(_SC_REALTIME_SIGNALS);
    case 0x0043: return sysconf(_SC_SEMAPHORES);
    case 0x0044: return sysconf(_SC_SHARED_MEMORY_OBJECTS);
    case 0x0045: return sysconf(_SC_SYNCHRONIZED_IO);
    case 0x0046: return sysconf(_SC_TIMERS);
    case 0x0047: return sysconf(_SC_GETGR_R_SIZE_MAX);
    case 0x0048: return sysconf(_SC_GETPW_R_SIZE_MAX);
    case 0x0049: return sysconf(_SC_LOGIN_NAME_MAX);
    case 0x004a: return sysconf(_SC_THREAD_DESTRUCTOR_ITERATIONS);
    case 0x004b: return sysconf(_SC_THREAD_KEYS_MAX);
    case 0x004c: return sysconf(_SC_THREAD_STACK_MIN);
    case 0x004d: return sysconf(_SC_THREAD_THREADS_MAX);
    case 0x004e: return sysconf(_SC_TTY_NAME_MAX);
    case 0x004f: return sysconf(_SC_THREADS);
    case 0x0050: return sysconf(_SC_THREAD_ATTR_STACKADDR);
    case 0x0051: return sysconf(_SC_THREAD_ATTR_STACKSIZE);
    case 0x0052: return sysconf(_SC_THREAD_PRIORITY_SCHEDULING);
    case 0x0053: return sysconf(_SC_THREAD_PRIO_INHERIT);
    case 0x0054: return sysconf(_SC_THREAD_PRIO_PROTECT);
    case 0x0055: return sysconf(_SC_THREAD_SAFE_FUNCTIONS);
    case 0x0060: return sysconf(_SC_NPROCESSORS_CONF);
    case 0x0061: return sysconf(_SC_NPROCESSORS_ONLN);
    case 0x0062: return sysconf(_SC_PHYS_PAGES);
    case 0x0063: return sysconf(_SC_AVPHYS_PAGES);
    case 0x0064: return sysconf(_SC_MONOTONIC_CLOCK);
    case 0x0065: return sysconf(_SC_2_PBS);
    case 0x0066: return sysconf(_SC_2_PBS_ACCOUNTING);
    case 0x0067: return sysconf(_SC_2_PBS_CHECKPOINT);
    case 0x0068: return sysconf(_SC_2_PBS_LOCATE);
    case 0x0069: return sysconf(_SC_2_PBS_MESSAGE);
    case 0x006a: return sysconf(_SC_2_PBS_TRACK);
    case 0x006b: return sysconf(_SC_ADVISORY_INFO);
    case 0x006c: return sysconf(_SC_BARRIERS);
    case 0x006d: return sysconf(_SC_CLOCK_SELECTION);
    case 0x006e: return sysconf(_SC_CPUTIME);
    case 0x006f: return sysconf(_SC_HOST_NAME_MAX);
    case 0x0070: return sysconf(_SC_IPV6);
    case 0x0071: return sysconf(_SC_RAW_SOCKETS);
    case 0x0072: return sysconf(_SC_READER_WRITER_LOCKS);
    case 0x0073: return sysconf(_SC_REGEXP);
    case 0x0074: return sysconf(_SC_SHELL);
    case 0x0075: return sysconf(_SC_SPAWN);
    case 0x0076: return sysconf(_SC_SPIN_LOCKS);
    case 0x0077: return sysconf(_SC_SPORADIC_SERVER);
    case 0x0078: return sysconf(_SC_SS_REPL_MAX);
    case 0x0079: return sysconf(_SC_SYMLOOP_MAX);
    case 0x007a: return sysconf(_SC_THREAD_CPUTIME);
    case 0x007b: return sysconf(_SC_THREAD_PROCESS_SHARED);
    case 0x007c: return sysconf(_SC_THREAD_ROBUST_PRIO_INHERIT);
    case 0x007d: return sysconf(_SC_THREAD_ROBUST_PRIO_PROTECT);
    case 0x007e: return sysconf(_SC_THREAD_SPORADIC_SERVER);
    case 0x007f: return sysconf(_SC_TIMEOUTS);
    case 0x0080: return sysconf(_SC_TRACE);
    case 0x0081: return sysconf(_SC_TRACE_EVENT_FILTER);
    case 0x0082: return sysconf(_SC_TRACE_EVENT_NAME_MAX);
    case 0x0083: return sysconf(_SC_TRACE_INHERIT);
    case 0x0084: return sysconf(_SC_TRACE_LOG);
    case 0x0085: return sysconf(_SC_TRACE_NAME_MAX);
    case 0x0086: return sysconf(_SC_TRACE_SYS_MAX);
    case 0x0087: return sysconf(_SC_TRACE_USER_EVENT_MAX);
    case 0x0088: return sysconf(_SC_TYPED_MEMORY_OBJECTS);
    case 0x0089: return sysconf(_SC_V7_ILP32_OFF32);
    case 0x008a: return sysconf(_SC_V7_ILP32_OFFBIG);
    case 0x008b: return sysconf(_SC_V7_LP64_OFF64);
    case 0x008c: return sysconf(_SC_V7_LPBIG_OFFBIG);
    case 0x008d: return sysconf(_SC_XOPEN_STREAMS);
    //case 0x008e: return sysconf(_SC_XOPEN_UUCP); TODO: Not supported on Linux
    case 0x008f: return sysconf(_SC_LEVEL1_ICACHE_SIZE);
    case 0x0090: return sysconf(_SC_LEVEL1_ICACHE_ASSOC);
    case 0x0091: return sysconf(_SC_LEVEL1_ICACHE_LINESIZE);
    case 0x0092: return sysconf(_SC_LEVEL1_DCACHE_SIZE);
    case 0x0093: return sysconf(_SC_LEVEL1_DCACHE_ASSOC);
    case 0x0094: return sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    case 0x0095: return sysconf(_SC_LEVEL2_CACHE_SIZE);
    case 0x0096: return sysconf(_SC_LEVEL2_CACHE_ASSOC);
    case 0x0097: return sysconf(_SC_LEVEL2_CACHE_LINESIZE);
    case 0x0098: return sysconf(_SC_LEVEL3_CACHE_SIZE);
    case 0x0099: return sysconf(_SC_LEVEL3_CACHE_ASSOC);
    case 0x009a: return sysconf(_SC_LEVEL3_CACHE_LINESIZE);
    case 0x009b: return sysconf(_SC_LEVEL4_CACHE_SIZE);
    case 0x009c: return sysconf(_SC_LEVEL4_CACHE_ASSOC);
    case 0x009d: return sysconf(_SC_LEVEL4_CACHE_LINESIZE);
    //case 0x009e: return sysconf(_SC_NSIG); TODO: Not supported on Linux
    default: {
        long result = sysconf(name);
        BD_DEBUG("SYSCONF", "unmapped query %d -> host result %ld", name, result);
        return result;
    }
}
}

extern ABI_ATTR int strerror_r_impl(int errnum, char *buf, size_t buflen)
{
    char* ret = strerror_r(errnum, buf, buflen);
    if(ret == 0)
        return -1;
    else
       return errnum;
}
 #include <fnmatch.h>
ABI_ATTR int fnmatch_impl(const char *pattern, const char *string, int flags)
{
    return fnmatch(pattern, string, flags);
}