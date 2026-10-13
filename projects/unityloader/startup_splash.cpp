#include "startup_splash.h"
#include "startup_trace.h"
#include "egl_sdl.h"
#include "glad.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <string>

extern SDL_Window* sdl_win;
extern SDL_GLContext sdl_ctx;
extern EGLDisplay eglGetDisplay_impl(NativeDisplayType);

namespace {
std::string image_path;
std::atomic<bool> scene_ready{false};
bool visible = false;
GLuint texture = 0, program = 0, vao = 0;
int image_w = 0, image_h = 0;

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
}

int bd_startup_set_image(const char* path) {
    if (visible || !path || !*path) return 0;
    image_path = path;
    return 1;
}

void bd_startup_scene_ready() { scene_ready.store(true, std::memory_order_release); }

void bd_startup_show() {
    if (image_path.empty()) return;
    bd_startup::Scope elapsed("startup image prepare/show");
    SDL_Surface* loaded = SDL_LoadBMP(image_path.c_str());
    if (!loaded) {
        BD_LOG("SPLASH", "load failed %s: %s", image_path.c_str(), SDL_GetError());
        return;
    }
    SDL_Surface* rgba = SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (!rgba) return;
    // Create the same window/context Unity will later reuse, before loading its
    // Android libraries. No second window and no fixed splash wait.
    eglGetDisplay_impl(0);
    const char* vs = "#version 300 es\n"
        "out vec2 uv; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "uv=vec2(p.x,1.0-p.y);gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    const char* fs = "#version 300 es\nprecision mediump float;"
        "in vec2 uv; uniform sampler2D image; out vec4 color;"
        "void main(){color=texture(image,uv);}";
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
        image_w = rgba->w; image_h = rgba->h;
        glad_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glad_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_w, image_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
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
        }
        bd_startup::mark("startup image visible");
        BD_LOG("SPLASH", "shown %s %dx%d", image_path.c_str(), image_w, image_h);
    }
    SDL_FreeSurface(rgba);
    // Unity can take this context on its render thread.
    SDL_GL_MakeCurrent(sdl_win, nullptr);
}

bool bd_startup_draw() {
    if (!visible) return false;
    if (scene_ready.load(std::memory_order_acquire)) {
        release();
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
    const float fit = std::min(float(w)/image_w,float(h)/image_h);
    const int iw = int(image_w*fit), ih = int(image_h*fit);
    glad_glViewport((w-iw)/2,(h-ih)/2,iw,ih);
    glad_glBindTexture(GL_TEXTURE_2D,texture);
    glad_glBindSampler(0,0);
    glad_glUseProgram(program);
    glad_glUniform1i(glad_glGetUniformLocation(program,"image"),0);
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
