#include "egl_sdl.h"
#include "SDL2/SDL.h"
#include "process_memory.h"
#include "plugin_present.h"
#include "glad_egl.h"
#include "gles2.h"
#include "logging.h"
#include "platform.h"
#include "so_util.h"
#include "thunk_gen.h"
#include <algorithm>
#include <errno.h>
#include <inttypes.h>
#include <memory>
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <toml++/toml.hpp>
#include <unistd.h>
#include <vector>
extern toml::table config;

// Opaque EGL objects must be distinct, aligned, and readable. 0xDEAD collided
// with dlopen_impl's fake handle and is unmapped (first page), so Unity treating
// a display/context as a pointer SIGSEGVs. Offset 0x10 is a common field load.
struct FakeEglObject {
    FakeEglObject* self;
    FakeEglObject* p08;
    FakeEglObject* p10;
    uintptr_t pad[13];
};

static FakeEglObject g_fake_display;
static FakeEglObject g_fake_context;
static FakeEglObject g_fake_window_surface;
static FakeEglObject g_fake_pbuffer_surface;
static FakeEglObject g_fake_config;

static FakeEglObject* fake_egl_touch(FakeEglObject* o)
{
    if (!o->self) {
        o->self = o;
        o->p08 = o;
        o->p10 = o;
    }
    return o;
}

static EGLDisplay fake_egl_display()
{
    return (EGLDisplay)fake_egl_touch(&g_fake_display);
}
static EGLContext fake_egl_context()
{
    return (EGLContext)fake_egl_touch(&g_fake_context);
}
static EGLSurface fake_egl_window_surface()
{
    return (EGLSurface)fake_egl_touch(&g_fake_window_surface);
}
static EGLSurface fake_egl_pbuffer_surface()
{
    return (EGLSurface)fake_egl_touch(&g_fake_pbuffer_surface);
}
static EGLConfig fake_egl_config()
{
    return (EGLConfig)fake_egl_touch(&g_fake_config);
}

SDL_Window* sdl_win;
SDL_GLContext sdl_ctx;
EGLDisplay egl_display;
EGLContext egl_context;
EGLSurface egl_surface;

static uint32_t bd_fb_channel(uint8_t value, const fb_bitfield& field)
{
    if (!field.length)
        return 0;
    const uint32_t max_value = field.length >= 32
        ? 0xffffffffu : ((1u << field.length) - 1u);
    return ((static_cast<uint32_t>(value) * max_value + 127u) / 255u)
        << field.offset;
}

static bool bd_cpu_present_frame()
{
    static int fb_fd = -1;
    static uint8_t* fb_map = nullptr;
    static size_t fb_map_size = 0;
    static fb_fix_screeninfo finfo = {};
    static fb_var_screeninfo vinfo = {};
    static std::vector<uint8_t> rgba;
    static bool failed = false;

    if (failed)
        return false;
    if (!fb_map) {
        fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
        if (fb_fd < 0 || ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) != 0 ||
            ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) != 0 ||
            vinfo.bits_per_pixel != 32) {
            BD_LOG("EGL_SDL", "CPU present init failed fd=%d bpp=%u errno=%d",
                   fb_fd, vinfo.bits_per_pixel, errno);
            if (fb_fd >= 0)
                close(fb_fd);
            fb_fd = -1;
            failed = true;
            return false;
        }
        fb_map_size = finfo.smem_len;
        void* mapped = mmap(nullptr, fb_map_size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fb_fd, 0);
        if (mapped == MAP_FAILED) {
            BD_LOG("EGL_SDL", "CPU present mmap failed size=%zu errno=%d",
                   fb_map_size, errno);
            close(fb_fd);
            fb_fd = -1;
            failed = true;
            return false;
        }
        fb_map = static_cast<uint8_t*>(mapped);
        BD_LOG("EGL_SDL", "CPU present active fb=%ux%u virtual=%ux%u yoffset=%u stride=%u format=%u:%u/%u:%u/%u:%u/%u:%u",
               vinfo.xres, vinfo.yres, vinfo.xres_virtual, vinfo.yres_virtual,
               vinfo.yoffset, finfo.line_length,
               vinfo.red.offset, vinfo.red.length,
               vinfo.green.offset, vinfo.green.length,
               vinfo.blue.offset, vinfo.blue.length,
               vinfo.transp.offset, vinfo.transp.length);
    }

    int drawable_w = 0;
    int drawable_h = 0;
    SDL_GL_GetDrawableSize(sdl_win, &drawable_w, &drawable_h);
    const int copy_w = std::min(drawable_w, static_cast<int>(vinfo.xres));
    const int copy_h = std::min(drawable_h, static_cast<int>(vinfo.yres));
    if (copy_w <= 0 || copy_h <= 0 || !glad_glReadPixels)
        return false;
    rgba.resize(static_cast<size_t>(drawable_w) * drawable_h * 4u);
    glad_glFinish();
    glad_glReadPixels(0, 0, drawable_w, drawable_h, GL_RGBA,
                      GL_UNSIGNED_BYTE, rgba.data());

    static std::vector<uint32_t> packed;
    packed.resize(static_cast<size_t>(copy_w) * copy_h);
    for (int y = 0; y < copy_h; ++y) {
        const uint8_t* src = rgba.data() +
            (static_cast<size_t>(drawable_h - 1 - y) * drawable_w * 4u);
        uint32_t* dst = packed.data() + static_cast<size_t>(y) * copy_w;
        for (int x = 0; x < copy_w; ++x) {
            const uint8_t* pixel = src + static_cast<size_t>(x) * 4u;
            dst[x] = bd_fb_channel(pixel[0], vinfo.red) |
                     bd_fb_channel(pixel[1], vinfo.green) |
                     bd_fb_channel(pixel[2], vinfo.blue) |
                     bd_fb_channel(pixel[3], vinfo.transp);
        }
    }
    // This Allwinner driver can report yoffset=480 while the display engine
    // continues scanning page 0 after its RCQ state gets out of sync.  Mirror
    // the frame into every complete virtual page so the visible page updates
    // without issuing the FBIOPAN_DISPLAY ioctl that deadlocks.
    const uint32_t page_count = std::max(1u, vinfo.yres_virtual / vinfo.yres);
    for (uint32_t page = 0; page < page_count; ++page) {
        const size_t page_offset = static_cast<size_t>(page) * vinfo.yres *
            finfo.line_length + static_cast<size_t>(vinfo.xoffset) * 4u;
        for (int y = 0; y < copy_h; ++y) {
            const size_t dst_offset = page_offset +
                static_cast<size_t>(y) * finfo.line_length;
            if (dst_offset + static_cast<size_t>(copy_w) * 4u > fb_map_size)
                break;
            memcpy(fb_map + dst_offset,
                   packed.data() + static_cast<size_t>(y) * copy_w,
                   static_cast<size_t>(copy_w) * 4u);
        }
    }
    __sync_synchronize();
    return true;
}

static void bd_log_sdl_display_mode(const char* label, int display_index)
{
    SDL_DisplayMode mode = {};
    if (SDL_GetCurrentDisplayMode(display_index, &mode) == 0) {
        BD_LOG("EGL_SDL", "%s display=%d current_mode=%dx%d@%d fmt=0x%x",
               label, display_index, mode.w, mode.h, mode.refresh_rate, mode.format);
    } else {
        BD_LOG("EGL_SDL", "%s display=%d current_mode unavailable: %s",
               label, display_index, SDL_GetError());
    }
}

namespace jnivm::android::view {
class Choreographer;

class Choreographer {
public:
    static std::shared_ptr<Choreographer> getInstance();
    void signalVSync();
};
}

static int egl_ext_blocked(const char* sym)
{
    // Mali / SDL 2.0.10 没有这些；转发给真 libEGL 会在 Anbernic 上崩。
    static const char* blocked[] = {
        "eglQueryDevicesEXT",
        "eglQueryDeviceStringEXT",
        "eglQueryDeviceAttribEXT",
        "eglQueryDisplayAttribEXT",
        "eglQueryDeviceBinaryEXT",
        "eglQueryDmaBufFormatsEXT",
        "eglQueryDmaBufModifiersEXT",
        "eglGetPlatformDisplayEXT",
        "eglGetPlatformDisplay",
        NULL,
    };
    for (int i = 0; blocked[i]; i++) {
        if (strcmp(sym, blocked[i]) == 0)
            return 1;
    }
    return 0;
}

static SDL_Window* bd_create_sdl_window(int w, int h)
{
    struct Attempt {
        Uint32 flags;
        const char* label;
    };
    static const Attempt attempts[] = {
        { (Uint32)(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN), "FULLSCREEN" },
        { (Uint32)(SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP), "FULLSCREEN_DESKTOP" },
        { (Uint32)SDL_WINDOW_OPENGL, "windowed" },
    };
    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); i++) {
        SDL_Window* win = SDL_CreateWindow("Teapot",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            w, h, attempts[i].flags);
        if (win) {
            BD_LOG("EGL_SDL", "SDL_CreateWindow %s ok flags=0x%x",
                   attempts[i].label, attempts[i].flags);
            return win;
        }
        BD_LOG("EGL_SDL", "SDL_CreateWindow %s failed: %s",
               attempts[i].label, SDL_GetError());
    }
    return NULL;
}

// mali-fbdev + SDL 2.0.12: if the GLES context is already current on another
// thread (Unity gfx-job / render thread) or SDL's TLS is stale, the first
// SDL_GL_MakeCurrent returns EGL_BAD_ACCESS. Clearing this thread's binding
// forces the second call to actually run. Same idea as the non-FAKE_EGL
// KMSDRM repair below.
static int bd_sdl_gl_make_current(SDL_GLContext ctx)
{
    if (!sdl_win)
        return -1;
    if (SDL_GL_MakeCurrent(sdl_win, ctx) == 0)
        return 0;
    BD_LOG("EGL_SDL", "SDL_GL_MakeCurrent failed: %s (tid=%lu) — unbind+retry",
           SDL_GetError(), (unsigned long)SDL_ThreadID());
    SDL_GL_MakeCurrent(sdl_win, NULL);
    if (SDL_GL_MakeCurrent(sdl_win, ctx) == 0) {
        BD_LOG("EGL_SDL", "SDL_GL_MakeCurrent rebound ok tid=%lu",
               (unsigned long)SDL_ThreadID());
        return 0;
    }
    BD_LOG("EGL_SDL", "SDL_GL_MakeCurrent retry failed: %s tid=%lu",
           SDL_GetError(), (unsigned long)SDL_ThreadID());
    return -1;
}

static SDL_GLContext bd_create_gles_context(SDL_Window* win)
{
    // Anbernic Mali-G31 已验证 GLES 3.2。只允许 3.2 / 3.1，禁止落到 GLES2，
    // 否则 Unity 会按 ES2 选 renderer，和真机能力、包体设定都不一致。
    static const int versions[][2] = { { 3, 2 }, { 3, 1 } };
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        int maj = versions[i][0];
        int min = versions[i][1];
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, maj);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, min);
        SDL_GLContext ctx = SDL_GL_CreateContext(win);
        if (ctx) {
            BD_LOG("EGL_SDL", "GLES context %d.%d ok", maj, min);
            return ctx;
        }
        BD_LOG("EGL_SDL", "GLES %d.%d failed: %s", maj, min, SDL_GetError());
    }
    return NULL;
}

void* getProc(const char* sym)
{
    if (!sym)
        return NULL;
    if (egl_ext_blocked(sym)) {
        BD_LOG("EGL_SDL", "getProc block %s (Mali/SDL 2.0.10)", sym);
        return NULL;
    }

    void* proc = SDL_GL_GetProcAddress(sym);
    if (proc)
        return proc;

#ifdef FAKE_EGL
    // 窗口已由系统 SDL 创建。不要再 dlopen 真 libEGL 去服务 Unity 的 Android display。
    return NULL;
#else
    static void* libEGL_handle = dlopen("libEGL.so", RTLD_NOW);
    if (!libEGL_handle)
        return NULL;
    return dlsym(libEGL_handle, sym);
#endif
}

EGLBoolean eglSwapBuffers_impl(EGLDisplay display,
    EGLSurface surface)
{
    bd_sdl_gl_make_current(sdl_ctx);

    const char* cpu_present_value = getenv("BD_EGL_CPU_PRESENT");
    const bool cpu_present = cpu_present_value && *cpu_present_value &&
        strcmp(cpu_present_value, "0") != 0;

    // Optional CPU framebuffer present + plugin present callbacks. Games can
    // raise BD_EGL_CPU_PRESENT around bootstrap transitions that wedge mali
    // fbdev (sunxi_fb_pan_display). Plugins register via
    // register_present_callback (ABI v3) to poll AsyncOperations on this thread.
    static bool transition_pause_active = false;
    static Uint32 transition_pause_started = 0;
    static Uint32 transition_pause_ms = 0;
    static uint32_t transition_skipped = 0;
    const char* pause_requested = getenv("BD_EGL_SWAP_PAUSE");
    const bool pause_enabled = pause_requested && *pause_requested &&
        strcmp(pause_requested, "0") != 0;
    bool skip_swap = cpu_present;
    if (cpu_present) {
        bd_cpu_present_frame();
        bd_plugin_run_present_callbacks();
        // Keep the readback/copy fallback near 60 FPS so input, audio and the
        // Dropbeak service are not starved by an unbounded render loop.
        SDL_Delay(16);
    } else if (pause_enabled) {
        if (!transition_pause_active) {
            const char* duration = getenv("BD_EGL_SWAP_PAUSE_MS");
            long parsed = duration ? strtol(duration, nullptr, 10) : 3000;
            if (parsed < 1 || parsed > 15000)
                parsed = 3000;
            transition_pause_ms = static_cast<Uint32>(parsed);
            transition_pause_started = SDL_GetTicks();
            transition_skipped = 0;
            transition_pause_active = true;
            BD_LOG("EGL_SDL", "pausing SDL swaps for PlatformLoader (max=%u ms)",
                   transition_pause_ms);
        }
        const Uint32 elapsed = SDL_GetTicks() - transition_pause_started;
        if (elapsed < transition_pause_ms) {
            ++transition_skipped;
            skip_swap = true;
            // Drain each frame's GPU work without presenting it.  Otherwise
            // the unthrottled transition can queue hundreds of default-FBO
            // frames and make the first resumed fbdev flip wait forever.
            if (glad_glFinish)
                glad_glFinish();
            SDL_Delay(16);
        } else {
            setenv("BD_EGL_SWAP_PAUSE", "0", 1);
            BD_LOG("EGL_SDL", "resuming SDL swaps after %u ms (%u skipped)",
                   elapsed, transition_skipped);
            transition_pause_active = false;
        }
    } else {
        transition_pause_active = false;
    }

    if (!skip_swap)
        SDL_GL_SwapWindow(sdl_win);

    // Process RSS sample for OOM diagnosis (gameBase load etc.). Default 2s;
    // set debug.mem_log_interval_ms=0 or BD_MEM_LOG_MS=0 to disable.
    {
        static int mem_interval_ms = -1;
        if (mem_interval_ms < 0) {
            const char* env = getenv("BD_MEM_LOG_MS");
            if (env && *env)
                mem_interval_ms = atoi(env);
            else
                mem_interval_ms = config["debug"]["mem_log_interval_ms"]
                    .value_or<int>(2000);
        }
        bd_log_process_memory_throttled("eglSwapBuffers", mem_interval_ms);
    }

    auto choreographer = jnivm::android::view::Choreographer::getInstance();
    if (choreographer) {
        choreographer->signalVSync();
    }
    return EGL_TRUE;
}

// Just return the current display
EGLDisplay eglGetDisplay_impl(NativeDisplayType native_display)
{
    if (egl_display)
        return egl_display;

    // Initialize SDL with video, audio, joystick, and controller support
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fatal_error("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        // return -1;
    }
    int requested_w = config["device"]["displayWidth"].value_or<int>(640);
    int requested_h = config["device"]["displayHeight"].value_or<int>(480);
    if (requested_w <= 0) requested_w = 640;
    if (requested_h <= 0) requested_h = 480;

    const char* video_driver = SDL_GetCurrentVideoDriver();
    BD_LOG("EGL_SDL", "SDL video_driver=%s requested=%dx%d",
           video_driver ? video_driver : "(null)", requested_w, requested_h);
    bd_log_sdl_display_mode("before window", 0);

    // Request GLES before CreateWindow; retry versions at CreateContext.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    sdl_win = bd_create_sdl_window(requested_w, requested_h);
    if (sdl_win == NULL) {
        fatal_error("Failed to create SDL Window: %s\n", SDL_GetError());
    }
    int window_w = 0, window_h = 0;
    SDL_GetWindowSize(sdl_win, &window_w, &window_h);
    int display_index = SDL_GetWindowDisplayIndex(sdl_win);
    if (display_index < 0) display_index = 0;
    bd_log_sdl_display_mode("after window", display_index);
    BD_LOG("EGL_SDL", "SDL window requested=%dx%d window=%dx%d flags=0x%x",
           requested_w, requested_h, window_w, window_h, SDL_GetWindowFlags(sdl_win));

    sdl_ctx = bd_create_gles_context(sdl_win);
    if (sdl_ctx == NULL) {
        fatal_error("Failed to create OpenGL Context: %s\n", SDL_GetError());
    }
    SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
    // mali-fbdev + SDL 2.0.12: interval 1 can block forever on later swaps.
    if (SDL_GL_SetSwapInterval(0) != 0) {
        BD_LOG("EGL_SDL", "SDL_GL_SetSwapInterval(0) failed: %s", SDL_GetError());
    } else {
        BD_LOG("EGL_SDL", "SDL_GL_SetSwapInterval(0) ok (no vsync wait)");
    }
    int drawable_w = 0, drawable_h = 0;
    SDL_GL_GetDrawableSize(sdl_win, &drawable_w, &drawable_h);
    BD_LOG("EGL_SDL", "SDL drawable=%dx%d logical=%dx%d",
           drawable_w, drawable_h, requested_w, requested_h);

    //

#ifdef FAKE_EGL
    // FAKE 下 getProc("eglGetCurrentDisplay") 会是 NULL。SDL 已持有真实 context。
    egl_display = fake_egl_display();
    egl_context = fake_egl_context();
    egl_surface = fake_egl_window_surface();
    load_gles2_funcs();
#else
    egl_display = ((EGLDisplay (*)())getProc("eglGetCurrentDisplay"))();
    egl_context = ((EGLDisplay (*)())getProc("eglGetCurrentContext"))();
    egl_surface = ((EGLSurface (*)(EGLint))getProc("eglGetCurrentSurface"))(EGL_DRAW);

    load_egl_funcs();
    load_gles2_funcs();
#endif

    // Print OpenGL information
    const char* glVersion = (const char*)glad_glGetString(GL_VERSION);
    const char* glVendor = (const char*)glad_glGetString(GL_VENDOR);
    const char* glRenderer = (const char*)glad_glGetString(GL_RENDERER);
    const char* glExtensions = (const char*)glad_glGetString(GL_EXTENSIONS);

    if (glVersion) {
        printf("OpenGL Version: %s\n", glVersion);
    } else {
        fatal_error("Failed to retrieve OpenGL version.\n");
    }

    if (glVendor) {
        printf("OpenGL Vendor: %s\n", glVendor);
    } else {
        fatal_error("Failed to retrieve OpenGL vendor.\n");
    }

    if (glRenderer) {
        printf("OpenGL Renderer: %s\n", glRenderer);
    } else {
        fatal_error("Failed to retrieve OpenGL renderer.\n");
    }

    if (glExtensions) {
        // ~3KB list — debug-only via BOOT_LOG; short Vendor/Renderer above stays.
        BOOT_LOG("OpenGL Extensions: %s\n", glExtensions);
    } else {
        fatal_error("Failed to retrieve OpenGL extensions.\n");
    }

    // Just for good measure
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);
    SDL_GL_SwapWindow(sdl_win);

    return egl_display;
}

// Do not actually initialize, just return the EGL version number.
EGLBoolean eglInitialize_impl(EGLDisplay display, int* major, int* minor)
{
    verbose("EGL_SDL", "eglInitialize\n");
#ifdef FAKE_EGL
    if (major != NULL)
        *major = 1;
    if (minor != NULL)
        *minor = 4;
    return EGL_TRUE;
#endif

    if (!egl_display)
        eglGetDisplay_impl(NULL);

    int temp_major = 0, temp_minor = 0;
    const char* versionString = ((const char* (*)(EGLDisplay, EGLint))getProc("eglQueryString"))(display, EGL_VERSION);
    if (!versionString) {
        fatal_error("Failed to retrieve EGL version string.\n");
        return EGL_FALSE;
    }

    if (sscanf(versionString, "%d.%d", &temp_major, &temp_minor) != 2) {
        fatal_error("Failed to parse EGL version string: %s\n", versionString);
        return EGL_FALSE;
    }

    if (major != NULL)
        *major = temp_major;
    if (minor != NULL)
        *minor = temp_minor;

    return EGL_TRUE;
}

// Do not actually search for configs. Just always return the config that the current context uses
EGLBoolean eglChooseConfig_impl(EGLDisplay display, const EGLint* attribList, EGLConfig* configs, EGLint configSize, EGLint* numConfigs)
{
    verbose("EGL_SDL", "eglChooseConfig\n");
#ifdef FAKE_EGL
    if (numConfigs)
        *numConfigs = 1;
    if (configs && configSize > 0)
        configs[0] = fake_egl_config();
    return EGL_TRUE;
#endif

    // Inline fetching of eglGetCurrentContext
    EGLContext context = egl_context;
    if (context == EGL_NO_CONTEXT) {
        fatal_error("Failed to get current EGLContext.\n");
        return EGL_FALSE;
    }

    EGLint configID;
    if (!((EGLBoolean (*)(EGLDisplay, EGLContext, EGLint, EGLint*))getProc("eglQueryContext"))(display, context, EGL_CONFIG_ID, &configID)) {
        fatal_error("Failed to query EGL_CONFIG_ID.\n");
        return EGL_FALSE;
    }

    EGLint totalConfigs;
    if (!((EGLBoolean (*)(EGLDisplay, EGLConfig*, EGLint, EGLint*))getProc("eglGetConfigs"))(display, NULL, 0, &totalConfigs)) {
        fatal_error("Failed to get the number of EGLConfigs.\n");
        return EGL_FALSE;
    }

    EGLConfig* allConfigs = (EGLConfig*)malloc(totalConfigs * sizeof(EGLConfig));
    if (!((EGLBoolean (*)(EGLDisplay, EGLConfig*, EGLint, EGLint*))getProc("eglGetConfigs"))(display, allConfigs, totalConfigs, &totalConfigs)) {
        fatal_error("Failed to retrieve EGLConfigs.\n");
        free(allConfigs);
        return EGL_FALSE;
    }

    // eglGetConfigAttrib to find the matching config
    EGLConfig matchingConfig = NULL;
    for (EGLint i = 0; i < totalConfigs; i++) {
        EGLint id;
        if (((EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*))getProc("eglGetConfigAttrib"))(display, allConfigs[i], EGL_CONFIG_ID, &id) && id == configID) {
            matchingConfig = allConfigs[i];
            break;
        }
    }
    free(allConfigs);

    if (!matchingConfig) {
        fatal_error("Failed to find a matching EGLConfig.\n");
        return EGL_FALSE;
    }

    // Populate the results
    if (configs && configSize > 0) {
        configs[0] = matchingConfig;
    }
    if (numConfigs) {
        *numConfigs = 1; // Always return exactly 1 config
    }

    return EGL_TRUE;
}

EGLSurface eglCreateWindowSurface_impl(EGLDisplay display, EGLConfig config, NativeWindowType native_window, EGLint const* attrib_list)
{
    verbose("EGL_SDL", "eglCreateWindowSurface\n");
#ifdef FAKE_EGL
    return fake_egl_window_surface();
#endif
    return egl_surface;
}

EGLBoolean eglQuerySurface_impl(EGLDisplay display, EGLSurface surface, EGLint attribute, EGLint* value)
{
    verbose("EGL_SDL", "eglQuerySurface\n");
    // Return logical [device] size, not physical — fixes 16:9-on-4:3 stretch.
    if (attribute == EGL_WIDTH) {
        *value = config["device"]["displayWidth"].value_or<int>(640);
        static bool logged = false;
        if (!logged) {
            logged = true;
            BD_LOG("EGL_SDL", "eglQuerySurface(EGL_WIDTH) -> %d", *value);
        }
        return EGL_TRUE;
    }
    if (attribute == EGL_HEIGHT) {
        *value = config["device"]["displayHeight"].value_or<int>(480);
        static bool logged = false;
        if (!logged) {
            logged = true;
            BD_LOG("EGL_SDL", "eglQuerySurface(EGL_HEIGHT) -> %d", *value);
        }
        return EGL_TRUE;
    }
#ifdef FAKE_EGL
    if (attribute == EGL_WIDTH)
        *value = 640;
    if (attribute == EGL_HEIGHT)
        *value = 480;
    return EGL_TRUE;
#endif
    return ((EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint*))getProc("eglQuerySurface"))(display, surface, attribute, value);
}

EGLContext eglCreateContext_impl(EGLDisplay display,
    EGLConfig config,
    EGLContext share_context,
    EGLint const* attrib_list)
{
    verbose("EGL_SDL", "eglCreateContext\n");
#ifdef FAKE_EGL
    return fake_egl_context();
#endif
    return egl_context;
}

EGLBoolean eglDestroyContext_impl(EGLDisplay display,
    EGLContext context)
{
    return EGL_TRUE;
}

EGLBoolean eglDestroySurface_impl(EGLDisplay display,
    EGLSurface surface)
{
    return EGL_TRUE;
}

EGLBoolean eglMakeCurrent_impl(EGLDisplay display,
    EGLSurface draw,
    EGLSurface read,
    EGLContext context)
{
    verbose("EGL_SDL", "eglMakeCurrent\n");
#ifdef FAKE_EGL
    if (!sdl_win || !sdl_ctx) {
        BD_LOG("EGL_SDL", "eglMakeCurrent: no SDL window/context");
        return EGL_FALSE;
    }
    if (context == (EGLContext)0) {
        if (bd_sdl_gl_make_current(NULL) != 0)
            BD_LOG("EGL_SDL", "eglMakeCurrent unbind failed");
        return EGL_TRUE;
    }
    if (bd_sdl_gl_make_current(sdl_ctx) != 0)
        return EGL_FALSE;
    return EGL_TRUE;
#else
    static auto cached_eglMakeCurrent = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))getProc("eglMakeCurrent");
    static auto p_glGetString = (const unsigned char* (*)(unsigned int))getProc("glGetString");

    // Forward the game's raw EGL handles. On WAYLAND this binds the SDL-owned
    // context on Unity's render thread and renders fine — unchanged path.
    EGLBoolean r = cached_eglMakeCurrent(display, draw, read, context);

    // KMSDRM/Mali repair. Unity does its GL (shader compile, draw) on a separate
    // render thread, which calls eglMakeCurrent to take the context. The forward
    // above passes the surface captured at init (eglCreateWindowSurface_impl
    // returns egl_surface). On KMSDRM that surface is NOT config-compatible with
    // the context on a 2nd thread → eglMakeCurrent fails with EGL_BAD_MATCH
    // (0x300d) → the render thread ends up with no live GL context → every
    // glCreateShader returns 0 → black screen. (eglGetError_impl always returns
    // SUCCESS, so Unity never notices the failure.) SDL owns its own self-
    // consistent surface+context pair that always binds, so on detecting "still
    // no live context after the forward" force a clean rebind through SDL (clear
    // SDL's thread-local current first so the second call really executes).
    // Guard: on wayland the forward already produced a live context, so glGetString
    // is non-null and this block never runs there — zero behaviour change on
    // working devices; only the broken KMSDRM render thread is repaired.
    if (context != (EGLContext)0 && p_glGetString && p_glGetString(0x1F02 /*GL_VERSION*/) == NULL) {
        SDL_GL_MakeCurrent(sdl_win, NULL);
        SDL_GL_MakeCurrent(sdl_win, sdl_ctx);
        if (p_glGetString(0x1F02) != NULL)
            return EGL_TRUE;
    }
    return r;
#endif
}

EGLint eglGetError_impl()
{
    return EGL_SUCCESS; // TRULY AWFUL
}

EGLBoolean eglGetConfigAttrib_impl(EGLDisplay display,
    EGLConfig config,
    EGLint attribute,
    EGLint* value)
{
#ifdef FAKE_EGL
    if (!value)
        return EGL_FALSE;
    switch (attribute) {
    case EGL_BUFFER_SIZE:
        *value = 32;
        break;
    case EGL_RED_SIZE:
    case EGL_GREEN_SIZE:
    case EGL_BLUE_SIZE:
    case EGL_ALPHA_SIZE:
        *value = 8;
        break;
    case EGL_DEPTH_SIZE:
        *value = 24;
        break;
    case EGL_STENCIL_SIZE:
        *value = 8;
        break;
    case EGL_SURFACE_TYPE:
        *value = EGL_WINDOW_BIT;
        break;
    case EGL_RENDERABLE_TYPE:
    case EGL_CONFORMANT:
        *value = EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT;
        break;
    case EGL_CONFIG_ID:
        *value = 1;
        break;
    case EGL_NATIVE_VISUAL_ID:
    case EGL_NATIVE_VISUAL_TYPE:
    case EGL_SAMPLE_BUFFERS:
    case EGL_SAMPLES:
    case EGL_LEVEL:
    case EGL_LUMINANCE_SIZE:
    case EGL_ALPHA_MASK_SIZE:
    case EGL_MIN_SWAP_INTERVAL:
        *value = 0;
        break;
    case EGL_MAX_SWAP_INTERVAL:
        *value = 1;
        break;
    case EGL_NATIVE_RENDERABLE:
    case EGL_BIND_TO_TEXTURE_RGB:
    case EGL_BIND_TO_TEXTURE_RGBA:
        *value = EGL_FALSE;
        break;
    case EGL_COLOR_BUFFER_TYPE:
        *value = EGL_RGB_BUFFER;
        break;
    case EGL_CONFIG_CAVEAT:
    case EGL_TRANSPARENT_TYPE:
        *value = EGL_NONE;
        break;
    case EGL_MAX_PBUFFER_WIDTH:
    case EGL_MAX_PBUFFER_HEIGHT:
        *value = 4096;
        break;
    case EGL_MAX_PBUFFER_PIXELS:
        *value = 4096 * 4096;
        break;
    case EGL_COVERAGE_BUFFERS_NV:
    case EGL_COVERAGE_SAMPLES_NV:
    case EGL_DEPTH_ENCODING_NV:
        *value = 0;
        break;
    case EGL_RECORDABLE_ANDROID:
    case EGL_FRAMEBUFFER_TARGET_ANDROID:
        *value = EGL_TRUE;
        break;
    default:
        BD_LOG("EGL_SDL", "eglGetConfigAttrib unhandled 0x%x -> 0", attribute);
        *value = 0;
        break;
    }
    return EGL_TRUE;
#else
    return ((EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*))getProc("eglGetConfigAttrib"))(display, config, attribute, value);
#endif
}

char const* eglQueryString_impl(EGLDisplay display,
    EGLint name)
{
    verbose("EGL_SDL", "eglQueryString %d\n", name);
#ifdef FAKE_EGL
    switch (name) {
    case EGL_VERSION:
        return "1.4";
    case EGL_VENDOR:
        return "ARM";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    case EGL_EXTENSIONS:
        // Mali/GLES-safe short list. No device / platform-display enumeration.
        return "EGL_KHR_image EGL_KHR_gl_texture_2D_image EGL_KHR_fence_sync";
    default:
        BD_LOG("EGL_SDL", "eglQueryString unhandled %d", name);
        return "";
    }
#else
    return ((char const* (*)(EGLDisplay, EGLint))getProc("eglQueryString"))(display, name);
#endif
}

EGLDisplay eglGetCurrentDisplay_impl()
{
    return egl_display;
}

EGLContext eglGetCurrentContext_impl()
{
    return egl_context;
}

EGLSurface eglGetCurrentSurface_impl()
{
    return egl_surface;
}

EGLBoolean eglSwapInterval_impl(EGLDisplay display,
    EGLint interval)
{
    BD_LOG("EGL_SDL", "eglSwapInterval requested=%d -> force 0 (mali-fbdev)", interval);
    if (SDL_GL_SetSwapInterval(0) != 0) {
        BD_LOG("EGL_SDL", "SDL_GL_SetSwapInterval(0) failed: %s", SDL_GetError());
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

EGLBoolean eglTerminate_impl(EGLDisplay display)
{
    verbose("EGL_SDL", "eglTerminate\n");
    return EGL_TRUE;
}

EGLBoolean eglSurfaceAttrib_impl(EGLDisplay display,
    EGLSurface surface,
    EGLint attribute,
    EGLint value)
{
    verbose("EGL_SDL", "eglSurfaceAttrib 0x%x=%d\n", attribute, value);
    return EGL_TRUE;
}

EGLSurface eglCreatePbufferSurface_impl(EGLDisplay display,
    EGLConfig config,
    EGLint const* attrib_list)
{
    verbose("EGL_SDL", "eglCreatePbufferSurface\n");
#ifdef FAKE_EGL
    return fake_egl_pbuffer_surface();
#else
    return egl_surface;
#endif
}

EGLBoolean eglQueryContext_impl(EGLDisplay display,
    EGLContext context,
    EGLint attribute,
    EGLint* value)
{
#ifdef FAKE_EGL
    if (!value)
        return EGL_FALSE;
    switch (attribute) {
    case EGL_CONFIG_ID:
        *value = 1;
        break;
    case EGL_CONTEXT_CLIENT_TYPE:
        *value = EGL_OPENGL_ES_API;
        break;
    case EGL_CONTEXT_CLIENT_VERSION:
        *value = 3;
        break;
    case EGL_RENDER_BUFFER:
        *value = EGL_BACK_BUFFER;
        break;
    default:
        BD_LOG("EGL_SDL", "eglQueryContext unhandled 0x%x", attribute);
        *value = 0;
        break;
    }
    return EGL_TRUE;
#else
    return ((EGLBoolean (*)(EGLDisplay, EGLContext, EGLint, EGLint*))getProc("eglQueryContext"))(display, context, attribute, value);
#endif
}

EGLBoolean eglGetConfigs_impl(EGLDisplay display,
    EGLConfig* configs,
    EGLint config_size,
    EGLint* num_config)
{
#ifdef FAKE_EGL
    if (num_config)
        *num_config = 1;
    if (configs && config_size > 0)
        configs[0] = fake_egl_config();
    return EGL_TRUE;
#else
    return ((EGLBoolean (*)(EGLDisplay, EGLConfig*, EGLint, EGLint*))getProc("eglGetConfigs"))(display, configs, config_size, num_config);
#endif
}

EGLBoolean eglBindAPI_impl(EGLenum api)
{
#ifdef FAKE_EGL
    return (api == EGL_OPENGL_ES_API) ? EGL_TRUE : EGL_FALSE;
#else
    auto fn = (EGLBoolean (*)(EGLenum))getProc("eglBindAPI");
    return fn ? fn(api) : EGL_TRUE;
#endif
}

EGLBoolean eglReleaseThread_impl(void)
{
    return EGL_TRUE;
}

EGLBoolean eglWaitGL_impl(void)
{
    return EGL_TRUE;
}

EGLBoolean eglWaitNative_impl(EGLint engine)
{
    (void)engine;
    return EGL_TRUE;
}

EGLBoolean eglWaitClient_impl(void)
{
    return EGL_TRUE;
}

// Actually implemented in egl.cpp
ABI_ATTR __eglMustCastToProperFunctionPointerType EGLAPIENTRY eglGetProcAddress_impl(const char* procname);

DynLibFunction symtable_egl_sdl[] = {
    NO_THUNK("eglSwapBuffers", (uintptr_t)&eglSwapBuffers_impl),
    NO_THUNK("eglGetDisplay", (uintptr_t)&eglGetDisplay_impl),
    NO_THUNK("eglInitialize", (uintptr_t)&eglInitialize_impl),
    NO_THUNK("eglChooseConfig", (uintptr_t)&eglChooseConfig_impl),
    NO_THUNK("eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface_impl),
    NO_THUNK("eglQuerySurface", (uintptr_t)&eglQuerySurface_impl),
    NO_THUNK("eglCreateContext", (uintptr_t)&eglCreateContext_impl),
    NO_THUNK("eglMakeCurrent", (uintptr_t)&eglMakeCurrent_impl),
    NO_THUNK("eglGetError", (uintptr_t)&eglGetError_impl),
    NO_THUNK("eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib_impl),
    NO_THUNK("eglDestroyContext", (uintptr_t)&eglDestroyContext_impl),
    NO_THUNK("eglDestroySurface", (uintptr_t)&eglDestroySurface_impl),
    NO_THUNK("eglQueryString", (uintptr_t)&eglQueryString_impl),
    NO_THUNK("eglGetCurrentDisplay", (uintptr_t)&eglGetCurrentDisplay_impl),
    NO_THUNK("eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext_impl),
    NO_THUNK("eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface_impl),
    NO_THUNK("eglSwapInterval", (uintptr_t)&eglSwapInterval_impl),
    NO_THUNK("eglGetProcAddress", (uintptr_t)&eglGetProcAddress_impl),
    NO_THUNK("eglTerminate", (uintptr_t)&eglTerminate_impl),
    NO_THUNK("eglSurfaceAttrib", (uintptr_t)&eglSurfaceAttrib_impl),
    NO_THUNK("eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface_impl),
    NO_THUNK("eglQueryContext", (uintptr_t)&eglQueryContext_impl),
    NO_THUNK("eglGetConfigs", (uintptr_t)&eglGetConfigs_impl),
    NO_THUNK("eglBindAPI", (uintptr_t)&eglBindAPI_impl),
    NO_THUNK("eglReleaseThread", (uintptr_t)&eglReleaseThread_impl),
    NO_THUNK("eglWaitGL", (uintptr_t)&eglWaitGL_impl),
    NO_THUNK("eglWaitNative", (uintptr_t)&eglWaitNative_impl),
    NO_THUNK("eglWaitClient", (uintptr_t)&eglWaitClient_impl),
    { NULL, (uintptr_t)NULL }
};

// Internal use, do not put these in the symtable

void sdl_initialize_gles()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fatal_error("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
    }
    sdl_win = SDL_CreateWindow("Loader", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1, SDL_WINDOW_HIDDEN | SDL_WINDOW_OPENGL);
    if (sdl_win == NULL) {
        fatal_error("Failed to create SDL Window: %s\n", SDL_GetError());
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    sdl_ctx = bd_create_gles_context(sdl_win);
    if (sdl_ctx == NULL) {
        fatal_error("Failed to create OpenGL Context: %s\n", SDL_GetError());
    }

    // SDL_GL_DeleteContext(sdl_ctx);
    // SDL_DestroyWindow(sdl_win);
    // SDL_Quit();
}
