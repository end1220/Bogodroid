#include "startup_splash.h"
#include "startup_trace.h"
#include "egl_sdl.h"
#include "glad.h"
#include "startup_video.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>
#include <memory>
#include <cstring>
#include <dlfcn.h>
#include <link.h>

extern SDL_Window* sdl_win;
extern SDL_GLContext sdl_ctx;
extern EGLDisplay eglGetDisplay_impl(NativeDisplayType);

namespace {
std::string image_path;
std::atomic<bool> scene_ready{false};
bool visible = false;
GLuint texture = 0, program = 0, vao = 0;
int image_w = 0, image_h = 0;
int frame_count = 1, frame_ms = 0;
Uint32 animation_started = 0;
bool video_mode = false;
Uint32 minimum_visible_ms = 0;

bool can_dismiss() {
    return scene_ready.load(std::memory_order_acquire) &&
        SDL_GetTicks() - animation_started >= minimum_visible_ms;
}
float height_fraction = 1.0f;
std::unique_ptr<StartupVideo> video;
std::thread animation_thread;
std::atomic<bool> animate{false};
bool background = false;
SDL_GLContext splash_context = nullptr;
EGLDisplay real_display = nullptr;
EGLSurface boot_surface = nullptr;
using MakeCurrent = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
MakeCurrent real_make_current = nullptr;
using DestroySurface = EGLBoolean (*)(EGLDisplay, EGLSurface);
DestroySurface real_destroy_surface = nullptr;

void* host_egl_proc(const char* name) {
    if (void* proc = SDL_GL_GetProcAddress(name)) return proc;
    // SDL's mali backend on Trimui resolves GLES symbols only. Reuse the
    // already loaded host EGL DSO; loading a soname via LD_LIBRARY_PATH could
    // select an unrelated Mesa library alongside the vendor context.
    static void* host = []() -> void* {
        void* handle = nullptr;
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
            const char* path = info->dlpi_name;
            const char* base = std::strrchr(path, '/');
            base = base ? base + 1 : path;
            if (std::strncmp(base, "libEGL.so", 9) != 0 ||
                (base[9] && base[9] != '.')) return 0;
            void* loaded = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
            if (!loaded) return 0;
            *static_cast<void**>(data) = loaded;
            BD_LOG("SPLASH", "host EGL fallback using loaded %s", path);
            return 1;
        }, &handle);
        return handle;
    }();
    return host ? dlsym(host, name) : nullptr;
}

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glad_glCreateShader(type);
    glad_glShaderSource(shader, 1, &source, nullptr);
    glad_glCompileShader(shader);
    GLint ok = 0;
    glad_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        GLsizei len = 0;
        glad_glGetShaderInfoLog(shader, sizeof(log) - 1, &len, log);
        BD_LOG("SPLASH", "shader compile failed type=0x%x: %s", type, log);
        glad_glDeleteShader(shader); return 0;
    }
    return shader;
}

void release() {
    if (texture) glad_glDeleteTextures(1, &texture);
    if (program) glad_glDeleteProgram(program);
    if (vao) glad_glDeleteVertexArrays(1, &vao);
    texture = program = vao = 0;
    visible = false;
}

bool start_background() {
#ifdef FAKE_EGL
    auto current_display = reinterpret_cast<EGLDisplay (*)()>(
        host_egl_proc("eglGetCurrentDisplay"));
    auto create_pbuffer = reinterpret_cast<EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint*)>(
        host_egl_proc("eglCreatePbufferSurface"));
    auto choose = reinterpret_cast<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>(
        host_egl_proc("eglChooseConfig"));
    auto query_context = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLContext, EGLint, EGLint*)>(
        host_egl_proc("eglQueryContext"));
    real_make_current = reinterpret_cast<MakeCurrent>(host_egl_proc("eglMakeCurrent"));
    real_destroy_surface = reinterpret_cast<DestroySurface>(host_egl_proc("eglDestroySurface"));
    auto get_error = reinterpret_cast<EGLint (*)()>(host_egl_proc("eglGetError"));
    auto get_config = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLConfig, EGLint, EGLint*)>(
        host_egl_proc("eglGetConfigAttrib"));
    auto failed = [&](const char* stage) {
        BD_LOG("SPLASH", "animation setup failed stage=%s eglerr=0x%x SDL=%s",
               stage, get_error ? get_error() : 0, SDL_GetError());
        return false;
    };
    if (!current_display || !create_pbuffer || !choose || !query_context ||
        !real_make_current || !real_destroy_surface)
        return failed("EGL entry points");
    real_display = current_display();
    EGLint id = 0, count = 0;
    EGLConfig config = nullptr;
    if (!query_context(real_display, static_cast<EGLContext>(sdl_ctx), EGL_CONFIG_ID, &id))
        return failed("query window context config");
    const EGLint attributes[] = {EGL_CONFIG_ID, id, EGL_NONE};
    if (!choose(real_display, attributes, &config, 1, &count) || count != 1)
        return failed("choose window context config");
    EGLint surfaces = 0;
    if (get_config) get_config(real_display, config, EGL_SURFACE_TYPE, &surfaces);
    BD_LOG("SPLASH", "animation config id=%d surfaceTypes=0x%x pbuffer=%d",
           id, surfaces, (surfaces & EGL_PBUFFER_BIT) ? 1 : 0);
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(sdl_win, &w, &h);
    const EGLint size[] = {EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE};
    boot_surface = create_pbuffer(real_display, config, size);
    if (!boot_surface) return failed("create guest pbuffer");
    splash_context = sdl_ctx;
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GLContext guest = SDL_GL_CreateContext(sdl_win);
    if (!guest) {
        failed("create guest context");
        real_destroy_surface(real_display, boot_surface);
        boot_surface = nullptr;
        SDL_GL_MakeCurrent(sdl_win, splash_context);
        return false;
    }
    sdl_ctx = guest;
    SDL_GL_MakeCurrent(sdl_win, nullptr);
    background = true;
    animate.store(true, std::memory_order_release);
    animation_thread = std::thread([] {
        if (SDL_GL_MakeCurrent(sdl_win, splash_context) != 0) {
            BD_LOG("SPLASH", "animation context failed: %s", SDL_GetError());
            animate.store(false, std::memory_order_release);
            return;
        }
        // Refresh independently of Unity's blocking startup calls. The guest
        // owns a different context and pbuffer until its first scene is ready.
        int previous = -1;
        while (animate.load(std::memory_order_acquire)) {
            const auto tick_started = std::chrono::steady_clock::now();
            bd_startup_draw();
            SDL_GL_SwapWindow(sdl_win);
            const int current = frame_count > 1 && frame_ms > 0
                ? int((SDL_GetTicks() - animation_started) / frame_ms) % frame_count : 0;
            if (current != previous) {
                previous = current;
                BD_LOG("SPLASH", "animation frame=%d elapsed=%ums", current,
                       SDL_GetTicks() - animation_started);
            }
            // Include decoding, uploads and any swap/vsync wait in the tick
            // budget. Sleeping another 16ms after that work limits a slower
            // device to 18-28 presents/s even for a 31.2fps startup video.
            std::this_thread::sleep_until(tick_started + std::chrono::milliseconds(16));
        }
        release();
        video.reset();
        SDL_GL_MakeCurrent(sdl_win, nullptr);
    });
    BD_LOG("SPLASH", "independent animation started video=%d frames=%d interval=%dms height=%.3f",
           video_mode, frame_count, frame_ms, height_fraction);
    return true;
#else
    return false;
#endif
}
}

int bd_startup_set_image(const char* path) {
    if (visible || !path || !*path) return 0;
    image_path = path;
    frame_count = 1;
    frame_ms = 0;
    video_mode = false;
    minimum_visible_ms = 0;
    height_fraction = 1.0f;
    return 1;
}

int bd_startup_set_animation(const char* path, int count, int interval_ms) {
    if (!bd_startup_set_image(path)) return 0;
    frame_count = count > 0 ? count : 1;
    frame_ms = interval_ms > 0 ? interval_ms : 0;
    return 1;
}

int bd_startup_set_video(const char* path, float fraction) {
    if (!(fraction > 0 && fraction <= 1) || !bd_startup_set_image(path)) return 0;
    video_mode = true;
    minimum_visible_ms = 3000;
    height_fraction = fraction;
    return 1;
}

void bd_startup_scene_ready() { scene_ready.store(true, std::memory_order_release); }

void bd_startup_wait_before_resume() {
    if (!visible || !minimum_visible_ms) return;
    const bool fallback = !background;
    SDL_GLContext previous = SDL_GL_GetCurrentContext();
    SDL_Window* previous_window = SDL_GL_GetCurrentWindow();
    if (fallback && SDL_GL_MakeCurrent(sdl_win, sdl_ctx) != 0) return;
    bd_startup::mark("waiting for splash minimum before Unity resume");
    while (SDL_GetTicks() - animation_started < minimum_visible_ms) {
        if (fallback) {
            bd_startup_draw();
            SDL_GL_SwapWindow(sdl_win);
        }
        SDL_Delay(1);
    }
    if (fallback) SDL_GL_MakeCurrent(previous_window ? previous_window : sdl_win, previous);
    bd_startup::mark("splash minimum complete; Unity may resume");
}

void bd_startup_show() {
    if (image_path.empty()) return;
    bd_startup::Scope elapsed("startup image prepare/show");
    SDL_Surface* rgba = nullptr;
    if (video_mode) {
        // Load SDL/driver TLS before FFmpeg's optional dependency tree. Loading
        // libGL after those DSOs can exhaust the dynamic static-TLS reserve.
        eglGetDisplay_impl(0);
        video = std::make_unique<StartupVideo>();
        if (!video->open(image_path.c_str())) {
            BD_LOG("SPLASH", "video prepare failed: %s", image_path.c_str());
            video.reset();
            return;
        }
    } else {
        SDL_Surface* loaded = SDL_LoadBMP(image_path.c_str());
        if (!loaded) {
            BD_LOG("SPLASH", "load failed %s: %s", image_path.c_str(), SDL_GetError());
            return;
        }
        rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(loaded);
        if (!rgba) return;
    }
    // Create the window before loading Android libraries. Animated boot uses
    // isolated contexts, then hands this same window to Unity without a wait.
    if (!video_mode) eglGetDisplay_impl(0);
    const char* vs = "#version 300 es\n"
        "out vec2 uv; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "uv=vec2(p.x,1.0-p.y);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    const char* fs = "#version 300 es\nprecision mediump float;"
        "in vec2 uv; uniform sampler2D image; uniform float frameOffset;"
        "uniform float frameScale; out vec4 color;"
        "void main(){color=texture(image,vec2(frameOffset+uv.x*frameScale,uv.y));}";
    GLuint vertex = compile(GL_VERTEX_SHADER, vs), fragment = compile(GL_FRAGMENT_SHADER, fs);
    if (vertex && fragment) {
        program = glad_glCreateProgram();
        glad_glAttachShader(program, vertex);
        glad_glAttachShader(program, fragment);
        glad_glLinkProgram(program);
        GLint linked = 0;
        glad_glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512] = {};
            GLsizei len = 0;
            glad_glGetProgramInfoLog(program, sizeof(log) - 1, &len, log);
            BD_LOG("SPLASH", "program link failed: %s", log);
            glad_glDeleteProgram(program); program = 0;
        }
    }
    if (vertex) glad_glDeleteShader(vertex);
    if (fragment) glad_glDeleteShader(fragment);
    if (program) {
        GLint unpack = 4, binding = 0;
        glad_glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        glad_glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glad_glGenTextures(1, &texture);
        glad_glBindTexture(GL_TEXTURE_2D, texture);
        glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glad_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        image_w = video_mode ? video->width() : rgba->w;
        image_h = video_mode ? video->height() : rgba->h;
        animation_started = SDL_GetTicks();
        glad_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glad_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_w, image_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE,
                         video_mode ? video->pixels() : rgba->pixels);
        glad_glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glad_glBindTexture(GL_TEXTURE_2D, binding);
        glad_glGenVertexArrays(1, &vao);
        visible = true;
        // Fill the presentation buffers so boot cannot flip back to black.
        for (int i = 0; i < 3; ++i) {
            const bool drawn = bd_startup_draw();
            if (i == 0) BD_LOG("SPLASH", "initial draw=%d glerr=0x%x", drawn,
                               glad_glGetError());
            if (glad_glFinish) glad_glFinish();
            SDL_GL_SwapWindow(sdl_win);
            if (i == 0) animation_started = SDL_GetTicks();
        }
        bd_startup::mark("startup image visible");
        BD_LOG("SPLASH", "shown %s %dx%d", image_path.c_str(), image_w, image_h);
        if ((video_mode || (frame_count > 1 && frame_ms > 0)) && !start_background())
            BD_LOG("SPLASH", "independent animation unavailable; using game swaps");
    }
    if (rgba) SDL_FreeSurface(rgba);
    if (!visible) video.reset();
    // Unity can take this context on its render thread.
    SDL_GL_MakeCurrent(sdl_win, nullptr);
}

bool bd_startup_draw() {
    if (!visible) return false;
    if (!background && can_dismiss()) {
        release();
        video.reset();
        bd_startup::mark("startup image dismissed; game frame ready");
        return false;
    }
    GLint prog, active, tex, sampler, vertex_array, draw_fbo, view[4];
    GLfloat clear[4];
    GLboolean mask[4];
    const GLenum toggles[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
    GLboolean enabled[5];
    glad_glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glad_glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glad_glActiveTexture(GL_TEXTURE0);
    glad_glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glad_glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
    glad_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
    glad_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
    glad_glGetIntegerv(GL_VIEWPORT, view);
    glad_glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glad_glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    for (int i=0;i<5;++i) { enabled[i]=glad_glIsEnabled(toggles[i]); glad_glDisable(toggles[i]); }
    glad_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glad_glColorMask(true,true,true,true);
    glad_glClearColor(0,0,0,1);
    glad_glClear(GL_COLOR_BUFFER_BIT);
    int w=0,h=0;
    SDL_GL_GetDrawableSize(sdl_win,&w,&h);
    const float source_w = static_cast<float>(image_w) /
        static_cast<float>(frame_count > 0 ? frame_count : 1);
    const float display_h = float(h) * height_fraction;
    const float fit = std::min(float(w)/source_w, display_h/image_h);
    const int iw = int(source_w*fit), ih = int(image_h*fit);
    glad_glViewport((w-iw)/2,(h-ih)/2,iw,ih);
    glad_glBindTexture(GL_TEXTURE_2D,texture);
    if (video && video->update(SDL_GetTicks() - animation_started)) {
        GLint unpack, row_length, skip_pixels, skip_rows, buffer;
        glad_glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
        glad_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
        glad_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        glad_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glad_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
        glad_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glad_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glad_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glad_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glad_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glad_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image_w, image_h,
                             GL_RGBA, GL_UNSIGNED_BYTE, video->pixels());
        glad_glPixelStorei(GL_UNPACK_ALIGNMENT, unpack);
        glad_glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
        glad_glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
        glad_glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
        glad_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    }
    glad_glBindSampler(0,0);
    glad_glUseProgram(program);
    glad_glUniform1i(glad_glGetUniformLocation(program,"image"),0);
    const int frame = frame_ms && frame_count > 1
        ? static_cast<int>((SDL_GetTicks() - animation_started) /
                           static_cast<Uint32>(frame_ms)) % frame_count : 0;
    glad_glUniform1f(glad_glGetUniformLocation(program,"frameOffset"),
                    frame_count > 1 ? static_cast<float>(frame) / frame_count : 0.0f);
    glad_glUniform1f(glad_glGetUniformLocation(program,"frameScale"),
                    frame_count > 1 ? 1.0f / frame_count : 1.0f);
    glad_glBindVertexArray(vao);
    glad_glDrawArrays(GL_TRIANGLES,0,3);
    static bool logged_draw = false;
    if (!logged_draw) {
        logged_draw = true;
        BD_LOG("SPLASH", "draw texture=%u program=%u viewport=%dx%d glerr=0x%x",
               texture, program, iw, ih, glad_glGetError());
    }
    glad_glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fbo);
    glad_glViewport(view[0],view[1],view[2],view[3]);
    glad_glClearColor(clear[0],clear[1],clear[2],clear[3]);
    glad_glColorMask(mask[0],mask[1],mask[2],mask[3]);
    for(int i=0;i<5;++i) { if(enabled[i])glad_glEnable(toggles[i]); else glad_glDisable(toggles[i]); }
    glad_glUseProgram(prog);
    glad_glBindVertexArray(vertex_array);
    glad_glBindTexture(GL_TEXTURE_2D,tex);
    glad_glBindSampler(0,sampler);
    glad_glActiveTexture(active);
    return true;
}

// SDL-style result, or -2 when ordinary SDL context handling should apply.
int bd_startup_make_current(void* context) {
    if (!background) return -2;
    const EGLSurface surface = context ? boot_surface : nullptr;
    return real_make_current(real_display, surface, surface,
        static_cast<EGLContext>(context)) ? 0 : -1;
}

bool bd_startup_background_present() {
    if (!background) return false;
    if (!can_dismiss() && animate.load(std::memory_order_acquire))
        return true;
    animate.store(false, std::memory_order_release);
    animation_thread.join();
    background = false;
    // A failed worker bind leaves its objects owned by the splash context.
    // Deleting that context frees them; never delete its names in Unity's
    // independent namespace (the numeric IDs can overlap).
    texture = program = vao = 0;
    visible = false;
    video.reset();
    // Discard the scene frame rendered into the pbuffer. Its next frame uses
    // the same game context on the window; the last splash stays visible until
    // that frame is swapped. No Unity objects or GL bindings are recreated.
    real_make_current(real_display, nullptr, nullptr, nullptr);
    SDL_GL_MakeCurrent(sdl_win, nullptr);
    if (SDL_GL_MakeCurrent(sdl_win, sdl_ctx) != 0)
        BD_LOG("SPLASH", "game window context bind failed: %s", SDL_GetError());
    real_destroy_surface(real_display, boot_surface);
    boot_surface = nullptr;
    SDL_GL_DeleteContext(splash_context);
    splash_context = nullptr;
    bd_startup::mark("startup animation finished; game context owns window");
    return true;
}
