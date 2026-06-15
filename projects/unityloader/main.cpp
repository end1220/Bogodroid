// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#include "globals.h"
#include <cstdlib>
#include <execinfo.h>
#include <iostream>

#include "toml++/toml.hpp"
toml::table config;
#include "config.h"

#include "io_util.h"
#include "javastubs/binding.h"
#include "monocompat/monobridge.h"
#include "monocompat/il2cpp_log_shim.h"
#include "platform.h"
#include "so_util.h"
#include <baron/baron.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <unistd.h>

#include "anative_activity.h"
#include "ndk.h"

#include "logging.h"

#include "debug_utils.h"
#include "egl_sdl.h"
#include "glad.h"
#include "glad_egl.h"
#include "gles2.h"
#include "input_backend.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_hints.h>
#include <cstring>
#include <dirent.h>
#include <ctime>
#include <cstdio>

static struct timespec g_bd_start_ts;
static int g_bd_start_inited = 0;

static void bd_time_init() {
    clock_gettime(CLOCK_MONOTONIC, &g_bd_start_ts);
    g_bd_start_inited = 1;
}

static long bd_time_ms() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - g_bd_start_ts.tv_sec) * 1000L +
              (now.tv_nsec - g_bd_start_ts.tv_nsec) / 1000000L;
    return ms;
}

#define BD_TIME(tag) do { \
    if (g_bd_start_inited) \
        BD_LOG("TIME", "+%ld ms %s", bd_time_ms(), tag); \
    } while(0)

thread_local int tls0[2 << 12] = {};
int foo() { return tls0[0]++; }

using namespace FakeJni;

// bool il2cpp_gc_is_incremental_stub()
// {
//     return false;
// }
// #include "thunk_gen.h"
// DynLibFunction symtable_il2cppfake[] = {
//     NO_THUNK("il2cpp_gc_is_incremental",(uintptr_t)&il2cpp_gc_is_incremental_stub),
//     NULL
// };

extern DynLibFunction symtable_monobridge[];
extern DynLibFunction symtable_il2cpp_log[];
extern DynLibFunction symtable_libc[];
extern DynLibFunction symtable_ndk[];
extern DynLibFunction symtable_gles2[];
extern DynLibFunction symtable_egl_sdl[];
extern DynLibFunction symtable_zlib[];
extern DynLibFunction symtable_opensles[];

DynLibFunction* so_static_patches[32] = {
    NULL,
};

DynLibFunction* so_dynamic_libraries[32] = {
    symtable_libc,
    symtable_ndk,
    symtable_egl_sdl,
    symtable_gles2,
    symtable_zlib,
#ifdef BD_ENABLE_OPENSLES_SHIM
    symtable_opensles,
#endif
    NULL
};

so_module* loaded_modules[128] = {
    NULL
};

extern SDL_Window* sdl_win;
extern SDL_GLContext sdl_ctx;
extern EGLDisplay egl_display;
extern EGLContext egl_context;
extern EGLSurface egl_surface;

bool llcompat = true;

Baron::Jvm vm;

#pragma GCC push_options
#pragma GCC optimize("O0")
void gdb_break_here()
{
}
#pragma GCC pop_options
/**
 * @brief Checks if a string ends with a given suffix.
 */
static int ends_with(const char* str, const char* suffix)
{
    if (!str || !suffix)
        return 0;
    size_t len_str = strlen(str);
    size_t len_suffix = strlen(suffix);
    if (len_suffix > len_str)
        return 0;
    return strncmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
}



extern "C" void bd_flush_prefs_impl();

// Generic config-driven IL2CPP value patcher. Reads [[il2cpp_patch]] from the
// game toml; after il2cpp_init, resolves each (class, method) by name via the
// exported il2cpp_* API and detours it to scale a float field of a pointer arg.
// No game-specific names here — targets live in config. mult==1.0 installs no
// hook. See IL2CPP_VALUE_PATCH_DESIGN.md.
//
// Limitation: the C pass-through forwards x0-x7 and an int/ptr return only;
// methods taking/returning floats in d0-d7 would need an asm trampoline.
namespace il2cpp_patch {
    typedef void* (*p_domain_get)();
    typedef void** (*p_domain_get_assemblies)(void* domain, size_t* size);
    typedef void* (*p_assembly_get_image)(void* assembly);
    typedef const char* (*p_image_get_name)(void* image);
    typedef void* (*p_class_from_name)(void* image, const char* ns, const char* name);
    typedef void* (*p_class_get_method_from_name)(void* klass, const char* name, int argc);

    static p_domain_get                 il2cpp_domain_get;
    static p_domain_get_assemblies      il2cpp_domain_get_assemblies;
    static p_assembly_get_image         il2cpp_assembly_get_image;
    static p_image_get_name             il2cpp_image_get_name;
    static p_class_from_name            il2cpp_class_from_name;
    static p_class_get_method_from_name il2cpp_class_get_method_from_name;

    typedef void* (*p_il2cpp_init)(const char*);

    struct Patch {
        std::string cls, method;
        int   argc  = 0;   // param count (excludes hidden this/MethodInfo*)
        int   arg   = 0;   // which param holds the pointer (0-based)
        int   field = -1;  // float field offset inside that pointer
        float mult  = 1.0f;
        uintptr_t orig = 0;
    };

    static const int MAXP = 8;
    static Patch        g_p[MAXP];
    static int          g_n = 0;
    static so_module*   g_mod = nullptr;
    static p_il2cpp_init g_orig_init = nullptr;
    static bool         g_installed = false;

    typedef uintptr_t (*orig8_t)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,
                                 uintptr_t,uintptr_t,uintptr_t,uintptr_t);

    // One hook per slot, statically bound to g_p[K]. Scales the configured float
    // field of the configured arg, then tail-calls the original with all 8 int
    // regs (extra ones are harmless; a void/int/ptr return is forwarded via x0).
    #define IL2CPP_HOOK(K) \
    static uintptr_t hook##K(uintptr_t x0,uintptr_t x1,uintptr_t x2,uintptr_t x3, \
                             uintptr_t x4,uintptr_t x5,uintptr_t x6,uintptr_t x7){ \
        Patch* p = &g_p[K]; \
        uintptr_t* a[8] = {&x0,&x1,&x2,&x3,&x4,&x5,&x6,&x7}; \
        uintptr_t obj = *a[1 + p->arg]; \
        if (p->field >= 0 && obj) { \
            float* f = (float*)(obj + p->field); \
            *f = *f * p->mult; \
        } \
        return ((orig8_t)p->orig)(x0,x1,x2,x3,x4,x5,x6,x7); \
    }
    IL2CPP_HOOK(0) IL2CPP_HOOK(1) IL2CPP_HOOK(2) IL2CPP_HOOK(3)
    IL2CPP_HOOK(4) IL2CPP_HOOK(5) IL2CPP_HOOK(6) IL2CPP_HOOK(7)
    static void* g_hookfn[MAXP] = { (void*)hook0,(void*)hook1,(void*)hook2,(void*)hook3,
                                    (void*)hook4,(void*)hook5,(void*)hook6,(void*)hook7 };

    static void* find_image(const char* want) {
        void* dom = il2cpp_domain_get();
        size_t n = 0;
        void** as = il2cpp_domain_get_assemblies(dom, &n);
        for (size_t i = 0; i < n; i++) {
            void* img = il2cpp_assembly_get_image(as[i]);
            const char* nm = il2cpp_image_get_name(img);
            if (nm && strstr(nm, want)) return img;
        }
        return nullptr;
    }

    static void install_once() {
        if (g_installed) return;
        g_installed = true;
        void* img = find_image("Assembly-CSharp");
        if (!img) { BD_LOG("DMG", "Assembly-CSharp image not found"); return; }
        for (int i = 0; i < g_n; i++) {
            Patch* p = &g_p[i];
            void* klass = il2cpp_class_from_name(img, "", p->cls.c_str());
            if (!klass) { BD_LOG("DMG", "class %s not found", p->cls.c_str()); continue; }
            void* m = il2cpp_class_get_method_from_name(klass, p->method.c_str(), p->argc);
            if (!m) { BD_LOG("DMG", "method %s.%s(%d) not found", p->cls.c_str(), p->method.c_str(), p->argc); continue; }
            uintptr_t ptr = *(uintptr_t*)m;       // MethodInfo.methodPointer @ offset 0
            uintptr_t orig = 0;
            hook_address_detour(g_mod, ptr, (uintptr_t)g_hookfn[i], &orig);
            if (!orig) { BD_LOG("DMG", "detour FAILED for %s.%s", p->cls.c_str(), p->method.c_str()); continue; }
            p->orig = orig;
            BD_LOG("DMG", "patch ACTIVE: %s.%s arg%d+0x%x x%.2f @ %p",
                   p->cls.c_str(), p->method.c_str(), p->arg, p->field, (double)p->mult, (void*)ptr);
        }
    }

    static void* il2cpp_init_hook(const char* name) {
        void* dom = g_orig_init(name);          // runtime now initialized
        install_once();
        return dom;
    }

    // Parse [[il2cpp_patch]] from config. Skips entries with mult==1.0 (no-op).
    static void load_config() {
        auto arr = config["il2cpp_patch"].as_array();
        if (!arr) return;
        for (auto&& node : *arr) {
            auto t = node.as_table();
            if (!t) continue;
            Patch p;
            p.cls    = (*t)["class"].value<std::string>().value_or("");
            p.method = (*t)["method"].value<std::string>().value_or("");
            p.argc   = (int)(*t)["argc"].value<int64_t>().value_or(0);
            p.arg    = (int)(*t)["arg"].value<int64_t>().value_or(0);
            p.field  = (int)(*t)["field"].value<int64_t>().value_or(-1);
            p.mult   = (float)(*t)["mult"].value<double>().value_or(1.0);
            if (p.cls.empty() || p.method.empty() || p.field < 0) continue;
            if (p.mult == 1.0f) continue;        // "正常" = no change, don't hook
            if (g_n < MAXP) g_p[g_n++] = p;
        }
    }

    // Call right after libil2cpp.so is loaded (before il2cpp_init runs).
    static void init(so_module* lil2cpp) {
        g_mod = lil2cpp;
        load_config();
        if (g_n == 0) return;                    // nothing to patch → fully inert
        il2cpp_domain_get                 = (p_domain_get)so_symbol(lil2cpp, "il2cpp_domain_get");
        il2cpp_domain_get_assemblies      = (p_domain_get_assemblies)so_symbol(lil2cpp, "il2cpp_domain_get_assemblies");
        il2cpp_assembly_get_image         = (p_assembly_get_image)so_symbol(lil2cpp, "il2cpp_assembly_get_image");
        il2cpp_image_get_name             = (p_image_get_name)so_symbol(lil2cpp, "il2cpp_image_get_name");
        il2cpp_class_from_name            = (p_class_from_name)so_symbol(lil2cpp, "il2cpp_class_from_name");
        il2cpp_class_get_method_from_name = (p_class_get_method_from_name)so_symbol(lil2cpp, "il2cpp_class_get_method_from_name");
        if (!il2cpp_domain_get || !il2cpp_class_from_name || !il2cpp_class_get_method_from_name) {
            BD_LOG("DMG", "missing il2cpp API exports, patches disabled");
            return;
        }
        uintptr_t init_addr = so_symbol(lil2cpp, "il2cpp_init");
        if (!init_addr) { BD_LOG("DMG", "il2cpp_init not found"); return; }
        uintptr_t orig = 0;
        hook_address_detour(lil2cpp, init_addr, (uintptr_t)&il2cpp_init_hook, &orig);
        if (!orig) { BD_LOG("DMG", "failed to hook il2cpp_init"); return; }
        g_orig_init = (p_il2cpp_init)orig;
        BD_LOG("DMG", "%d patch(es) armed; resolving on runtime init", g_n);
    }
}

int main(int argc, char* argv[])
{
    // Unbuffered stderr — glibc full-buffers (4KB) when stderr is a file,
    // and SIGKILL on lid-close would drop the tail of log.txt.
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    bd_time_init();
    BD_TIME("main() start");
    print_backtrace_on_segfault(); // Registers a signal handler to print backtrace on segfaults
    exit_on_signals(); // Exits when CTRL-C is presset (or SIGINT or SIGTERM is received)

    if (argc < 2) {
        fatal_error("Usage: %s <config file>\n", argv[0]);
        return -1;
    }

    // Init config, GLES pointers, JNI VN and bindings
    init_config(argv[1]);
    // sdl_initialize_gles();
    InitJNIBinding(&vm);

    JClass* unityClass = vm.findClass("com/unity3d/player/UnityPlayer").get();

    auto unityActivity = std::make_shared<jnivm::com::unity3d::player::UnityPlayerActivity>();
    auto unityPlayer = std::make_shared<jnivm::com::unity3d::player::UnityPlayer>();
    auto unityPlayerObj = std::dynamic_pointer_cast<jnivm::Object>(unityPlayer);
    jnivm::com::unity3d::player::UnityPlayer::currentActivity = unityActivity;
    auto& backend = InputBackend::instance();

    int module_count = 0;

    // There is a weird incompatibility with Unity's incremental GC. Luckily there's a commandline parameter to overwrite it.
    putenv("GC_DISABLE_INCREMENTAL=1");

    BOOT_LOG("Loading libc++\n");
    so_module lcpp = {};
    uintptr_t addr_lcpp = 0x3100000000;
    const char* path_lcpp = "lib/arm64-v8a/libc++_shared.so";
    if (!load_so_from_file(&lcpp, path_lcpp, addr_lcpp)) {
        BOOT_LOG("No libhelp found\n");
    }
    loaded_modules[module_count++] = &lcpp;

    so_module lbootstrap = {};
    uintptr_t addr_lbootstrap = 0x4200000000;
    const char* path_lbootstrap = "lib/arm64-v8a/libBootstrap.so";

    so_module ldobby = {};
    uintptr_t addr_ldobby = 0x4250000000;
    const char* path_ldobby = "lib/arm64-v8a/libdobby.so";
    if (llcompat) {
        BOOT_LOG("LemonLoader Compat active, loading libBootstrap.so and libdobby.so\n");
        if (!load_so_from_file(&ldobby, path_ldobby, addr_ldobby)) {
            BOOT_LOG("No libdobby found\n");
        }
        loaded_modules[module_count++] = &ldobby;

        if (!load_so_from_file(&lbootstrap, path_lbootstrap, addr_lbootstrap)) {
            BOOT_LOG("No libbootstrap found\n");
        }
        loaded_modules[module_count++] = &lbootstrap;

        so_module* mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "lib/arm64-v8a/libcrypto.so", 0x4251000000)) {
            BOOT_LOG("  Loaded: libcrypto.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "lib/arm64-v8a/libssl.so", 0x4252000000)) {
            BOOT_LOG("  Loaded: libssl.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/host/fxr/8.0.6/libhostfxr.so", 0x4253000000)) {
            BOOT_LOG("  Loaded: libhostfxr.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/shared/Microsoft.NETCore.App/8.0.6/libhostpolicy.so", 0x4254000000)) {
            BOOT_LOG("  Loaded: libhostpolicy.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/shared/Microsoft.NETCore.App/8.0.6/libcoreclr.so", 0x4255000000)) {
            BOOT_LOG("  Loaded: libcoreclr.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/shared/Microsoft.NETCore.App/8.0.6/libSystem.Native.so", 0x4256000000)) {
            BOOT_LOG("  Loaded: libSystem.Native.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/shared/Microsoft.NETCore.App/8.0.6/libSystem.Globalization.Native.so", 0x4257000000)) {
            BOOT_LOG("  Loaded: libSystem.Globalization.Native.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/shared/Microsoft.NETCore.App/8.0.6/libSystem.IO.Compression.Native.so", 0x4258000000)) {
            BOOT_LOG("  Loaded: libSystem.IO.Compression.Native.so\n");
            loaded_modules[module_count++] = mod;
        }

        mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, "assets/dotnet/shared/Microsoft.NETCore.App/8.0.6/libSystem.Security.Cryptography.Native.OpenSsl.so", 0x4259000000)) {
            BOOT_LOG("  Loaded: libSystem.Security.Cryptography.Native.OpenSsl.so\n");
            loaded_modules[module_count++] = mod;
        }

        auto lemonBootJNI_OnLoad = (jint (*)(JavaVM* vm, void* reserved))(so_symbol(&lbootstrap, "JNI_OnLoad"));
        if (lemonBootJNI_OnLoad) {
            BOOT_LOG("calling JNI_OnLoad from libBootstrap.so\n");
            lemonBootJNI_OnLoad(&vm, nullptr);
        }
    }

    BD_TIME("before loading libmain.so");
    BOOT_LOG("Loading libmain\n");
    so_module lmain = {};
    uintptr_t addr_lmain = 0x3200000000;
    const char* path_lmain = "lib/arm64-v8a/libmain.so";
    if (!load_so_from_file(&lmain, path_lmain, addr_lmain)) {
        return 1;
    }
    loaded_modules[module_count++] = &lmain;
    BD_TIME("after loading libmain.so");

    BD_TIME("before loading libil2cpp.so");
    BOOT_LOG("Loading libil2cpp\n");
    so_module lil2cpp = {};
    so_module lposix = {};
    so_module lmnative = {};
    uintptr_t addr_lil2cpp = 0x3600000000;
    const char* path_lil2cpp = "lib/arm64-v8a/libil2cpp.so";
    if (!load_so_from_file(&lil2cpp, path_lil2cpp, addr_lil2cpp)) {
        BOOT_LOG("il2cpp not found, trying libmono\n");
        const char* path_mono = "lib/arm64-v8a/libmonobdwgc-2.0.so";
        if (!load_so_from_file(&lil2cpp, path_mono, addr_lil2cpp)) {
            return 1;
        }
        BOOT_LOG("Loading libMonoPosixHelper.so\n");
        uintptr_t addr_lposix = 0x3700000000;
        const char* path_lposix = "lib/arm64-v8a/libMonoPosixHelper.so";
        if (!load_so_from_file(&lposix, path_lposix, addr_lposix)) {
            return 1;
        }
        loaded_modules[module_count++] = &lposix;

        BOOT_LOG("Loading libmono-native.so\n");
        uintptr_t addr_lmnative = 0x3750000000;
        const char* path_lmnative = "lib/arm64-v8a/libmono-native.so";
        if (!load_so_from_file(&lmnative, path_lmnative, addr_lmnative)) {
            return 1;
        }
        loaded_modules[module_count++] = &lmnative;

        so_dynamic_libraries[4] = symtable_monobridge;
        so_dynamic_libraries[5] = NULL;
        monobridge_init(&lil2cpp);
    }
    loaded_modules[module_count++] = &lil2cpp;
    BD_TIME("after loading libil2cpp.so");

    // Arm the generic il2cpp value-patch framework (reads [[il2cpp_patch]] from
    // config; resolves + hooks once il2cpp_init runs). Inert if no patches.
    il2cpp_patch::init(&lil2cpp);

#ifdef IL2CPP_TRACE
    if(config["debug"]["log_il2cpp"].value_or<bool>(false))
        il2cpp_log_shim_init(&lil2cpp, symtable_monobridge);
#endif

    BD_TIME("before loading libunity.so");
    BOOT_LOG("Loading libunity\n");
    so_module lunity = {};
    uintptr_t addr_lunity = 0x3800000000;
    const char* path_lunity = "lib/arm64-v8a/libunity.so";
    if (!load_so_from_file(&lunity, path_lunity, addr_lunity)) {
        return 1;
    }
    loaded_modules[module_count++] = &lunity;
    BD_TIME("after loading libunity.so");

    BOOT_LOG("Loading libburst\n");
    so_module lburst = {};
    uintptr_t addr_lburst = 0x4000000000;
    const char* path_lburst = "lib/arm64-v8a/lib_burst_generated.so";
    if (!load_so_from_file(&lburst, path_lburst, addr_lburst)) {
        BOOT_LOG("No libburst found\n");
    } else
        loaded_modules[module_count++] = &lburst;

    BOOT_LOG("Loading libUnityHelp\n");
    so_module lhelpers = {};
    uintptr_t addr_lhelpers = 0x4200000000;
    const char* path_lhelpers = "lib/arm64-v8a/libUnityHelpers_Android.so";
    if (!load_so_from_file(&lhelpers, path_lhelpers, addr_lhelpers)) {
        BOOT_LOG("No libhelp found\n");
    }
    loaded_modules[module_count++] = &lhelpers;

    const char* directory = "assets/bin/Data/Managed/";
    DIR* d = opendir(directory);
    if (!d) {
        perror(directory);
        return -1;
    }

    BOOT_LOG("Loading assemblies from: %s\n", directory);
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (!ends_with(entry->d_name, ".dll.so")) {
            continue;
        }

        char path[512];
        // Assumes the directory path has a trailing slash
        snprintf(path, sizeof(path), "%s%s", directory, entry->d_name);

        so_module* mod = (so_module*)calloc(1, sizeof(so_module));
        if (mod && load_so_from_file(mod, path, (uintptr_t)NULL)) {
            BOOT_LOG("  Loaded: %s\n", entry->d_name);
            loaded_modules[module_count++] = mod;
        } else {
            warning("  Failed to load: %s\n", path);
            free(mod); // It is safe to call free() on a NULL pointer
        }
    }
    closedir(d);

    auto mainJNI_OnLoad = (jint (*)(JavaVM* vm, void* reserved))(so_symbol(&lmain, "JNI_OnLoad"));
    if (mainJNI_OnLoad) {
        BD_TIME("before libmain JNI_OnLoad");
        BOOT_LOG("calling JNI_OnLoad from libmain.so\n");
        mainJNI_OnLoad(&vm, nullptr);
        BD_TIME("after libmain JNI_OnLoad");
    }

    JClass* nativeLoaderClass = vm.findClass("com/unity3d/player/NativeLoader").get();
    LocalFrame frame(vm);
    auto mainLoad = nativeLoaderClass->getMethod("(Ljava/lang/String;)Z", "load");

    BOOT_LOG("calling com/unity3d/player/NativeLoader/load from libmain.so\n");
    jvalue ret = mainLoad.invoke(frame.getJniEnv(), nativeLoaderClass, (JString) "lib/arm64-v8a");
    if (!ret.z) {
        BOOT_LOG("libmain.so:load returned false, game could not be loaded\n");
        return 1;
    }

    auto il2cppJNI_OnLoad = (jint (*)(JavaVM* vm, void* reserved))(so_symbol(&lil2cpp, "JNI_OnLoad"));
    if (il2cppJNI_OnLoad) {
        BD_TIME("before libil2cpp JNI_OnLoad");
        BOOT_LOG("calling JNI_OnLoad from libil2cpp.so (%p)\n", (void*)il2cppJNI_OnLoad);
        il2cppJNI_OnLoad(&vm, nullptr);
        BD_TIME("after libil2cpp JNI_OnLoad");
    }

    auto unityJNI_OnLoad = (jint (*)(JavaVM* vm, void* reserved))(so_symbol(&lunity, "JNI_OnLoad"));
    if (unityJNI_OnLoad) {
        BD_TIME("before libunity JNI_OnLoad");
        BOOT_LOG("calling JNI_OnLoad from libunity.so (%p)\n", (void*)unityJNI_OnLoad);
        unityJNI_OnLoad(&vm, nullptr);
        BD_TIME("after libunity JNI_OnLoad");
    }

    backend.setKeyCallback([unityActivity](std::shared_ptr<jnivm::android::view::KeyEvent> event) {
        unityActivity->injectEvent(event);
    });

    backend.setMotionCallback([unityActivity](std::shared_ptr<jnivm::android::view::MotionEvent> event) {
        unityActivity->injectEvent(event);
    });

    auto unityInitJni = unityClass->getMethod("(Landroid/content/Context;)V", "initJni");
    BOOT_LOG("calling initJni from libunity.so\n");
    auto activity = std::make_shared<jnivm::android::app::Activity>();
    LocalFrame frame2(vm);
    unityInitJni.invoke(frame2.getJniEnv(), unityPlayerObj.get(), activity);

    // In another thread, start the event loop
    std::thread([&backend]() {
        backend.runEventLoop();
    }).detach();

    // vm.printStatistics();
    // return 0;

    auto unityNRecreateGfxState = unityClass->getMethod("(ILandroid/view/Surface;)V", "nativeRecreateGfxState");
    BOOT_LOG("calling nativeRecreateGfxState from libunity.so\n");
    auto surface = std::make_shared<jnivm::android::view::Surface>();
    LocalFrame frame3(vm);
    auto ret2 = unityNRecreateGfxState.invoke(frame3.getJniEnv(), unityPlayerObj.get(), 0, surface);

    auto unityNRestartACtivityIndicator = unityClass->getMethod("()V", "nativeRestartActivityIndicator");
    if (unityNRestartACtivityIndicator) {
        BOOT_LOG("calling nativeRestartActivityIndicator from libunity.so\n");
        unityNRestartACtivityIndicator.invoke(frame3.getJniEnv(), unityPlayerObj.get());
    }

    auto unityNSendSurfaceChangedEvent = unityClass->getMethod("()V", "nativeSendSurfaceChangedEvent");
    BOOT_LOG("calling nativeSendSurfaceChangedEvent from libunity.so\n");
    unityNSendSurfaceChangedEvent.invoke(frame3.getJniEnv(), unityPlayerObj.get());

    gdb_break_here();

    auto unityNResume = unityClass->getMethod("()V", "nativeResume");
    BOOT_LOG("calling nativeResume from libunity.so\n");
    unityNResume.invoke(frame3.getJniEnv(), unityPlayerObj.get());

    auto unityNFocusChanged = unityClass->getMethod("(Z)V", "nativeFocusChanged");
    BOOT_LOG("calling nativeFocusChanged from libunity.so\n");
    unityNFocusChanged.invoke(frame3.getJniEnv(), unityPlayerObj.get(), true);

    auto unityNRender = unityClass->getMethod("()Z", "nativeRender");
    BD_TIME("before first nativeRender");
    BOOT_LOG("calling nativeRender from libunity.so\n");
    auto ret3 = unityNRender.invoke(frame3.getJniEnv(), unityPlayerObj.get());
    BD_TIME("after first nativeRender");

    BOOT_LOG("NativeRender returned %d, Entering loop...\n", ret3.z);

    // nativeRender() returns false after Application.Quit() — must stop rendering
    // or Unity crashes mid-shutdown. OnApplicationQuit has already saved PlayerPrefs;
    // we only flush our own SharedPreferences and fast-exit (skip dtor cascade).
    while (true) {
        auto ret4 = unityNRender.invoke(frame3.getJniEnv(), unityPlayerObj.get());
        if (!ret4.z) {
            BD_LOG("EXIT", "nativeRender returned false - Unity requested quit");
            bd_flush_prefs_impl();
            _exit(0);
        }
    }

    BOOT_LOG("Exit.\n");
    return 0;
}
