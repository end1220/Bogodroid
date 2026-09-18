// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#include <SDL2/SDL.h>
#include "glad.h"

#include "platform.h"
#include "so_util.h"
#include "thunk_gen.h"
#include "thunk_gen_dyn.h"
#include "logging.h"

#include "gles2_funcs.hpp"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include "bd_video.h"
#include "device_display.h"
#include <toml++/toml.hpp>
extern toml::table config;

static int symtable_gles2_index = 0;
DynLibFunction symtable_gles2[4096] = {};

#define PTR_RESOLVE(x) resolve_thunked<&glad_##x>(#x, symtable_gles2_index, symtable_gles2, SDL_GL_GetProcAddress)

// Texture-size cap. Unity ships textures sized for high-end Android phones
// that OOM on low-end Mali handhelds. wsm.toml [gpu] caps the longest side
// per format family and box-downsamples uploads:
//   textureMaxDim       — fallback for families without their own key (0 = off)
//   textureMaxDimRGBA8  — RGBA8 / RGB8 / SRGB8 / SRGB8_ALPHA8
//   textureMaxDimETC2   — ETC2 family
//   textureMaxDimASTC   — ASTC LDR family
// Strips (LUTs) and RTs matching the display size are skipped; HDR float /
// depth / stencil pass through unchanged. ETC2/ASTC caps are forced to 0:
// glCompressedTexSubImage2D is not intercepted, so shrinking compressed
// storage desyncs the upload dimensions and crashes the driver.
struct BD_TexCaps { int rgba8; int etc2; int astc; };

static const BD_TexCaps& bd_get_tex_caps()
{
    static BD_TexCaps caps;
    static bool init = false;
    if (!init) {
        int fallback = config["gpu"]["textureMaxDim"].value_or<int>(0);
        caps.rgba8 = config["gpu"]["textureMaxDimRGBA8"].value_or(fallback);
        caps.etc2  = config["gpu"]["textureMaxDimETC2"].value_or(fallback);
        caps.astc  = config["gpu"]["textureMaxDimASTC"].value_or(fallback);
        if (caps.etc2 > 0) {
            BD_LOG("CAP", "textureMaxDimETC2 = %d unsupported (compressed upload not intercepted), forcing 0", caps.etc2);
            caps.etc2 = 0;
        }
        if (caps.astc > 0) {
            BD_LOG("CAP", "textureMaxDimASTC = %d unsupported (compressed upload not intercepted), forcing 0", caps.astc);
            caps.astc = 0;
        }
        BD_LOG("CAP", "textureMaxDim RGBA8=%d ETC2=%d ASTC=%d (0 = disabled)",
               caps.rgba8, caps.etc2, caps.astc);
        init = true;
    }
    return caps;
}

static int bd_cap_for_format(GLenum sized_internalformat)
{
    const BD_TexCaps& caps = bd_get_tex_caps();
    switch (sized_internalformat) {
        case 0x8058 /*GL_RGBA8*/:
        case 0x8051 /*GL_RGB8*/:
        case 0x8C41 /*GL_SRGB8*/:
        case 0x8C43 /*GL_SRGB8_ALPHA8*/:
            return caps.rgba8;
        case 0x9274: case 0x9275: case 0x9276: case 0x9277:
        case 0x9278: case 0x9279:    // ETC2 family
            return caps.etc2;
        case 0x93B0: case 0x93B1: case 0x93B2: case 0x93B3:
        case 0x93B4: case 0x93B5: case 0x93B6: case 0x93B7:
        case 0x93B8: case 0x93B9: case 0x93BA: case 0x93BB:
        case 0x93BC: case 0x93BD:    // ASTC LDR
            return caps.astc;
        default:
            return 0;
    }
}

static int bd_bpp_from_format_type(GLenum format, GLenum type)
{
    int comps = 0;
    switch (format) {
        case 0x1908 /*GL_RGBA*/:           comps = 4; break;
        case 0x1907 /*GL_RGB*/:            comps = 3; break;
        case 0x80E1 /*GL_BGRA*/:           comps = 4; break;
        case 0x1909 /*GL_LUMINANCE*/:      comps = 1; break;
        case 0x190A /*GL_LUMINANCE_ALPHA*/:comps = 2; break;
        case 0x1906 /*GL_ALPHA*/:          comps = 1; break;
        case 0x1903 /*GL_RED*/:            comps = 1; break;
        case 0x8227 /*GL_RG*/:             comps = 2; break;
        default: return 0;
    }
    int bpc = 0;
    switch (type) {
        case 0x1401 /*GL_UNSIGNED_BYTE*/:  bpc = 1; break;
        case 0x1403 /*GL_UNSIGNED_SHORT*/: bpc = 2; break;
        case 0x1405 /*GL_UNSIGNED_INT*/:   bpc = 4; break;
        case 0x1406 /*GL_FLOAT*/:          bpc = 4; break;
        case 0x140B /*GL_HALF_FLOAT*/:     bpc = 2; break;
        default: return 0;
    }
    return comps * bpc;
}

static void bd_box_downsample(const uint8_t* src, int src_w, int src_h,
                               uint8_t* dst, int dst_w, int dst_h, int bpp)
{
    for (int dy = 0; dy < dst_h; dy++) {
        int sy0 = dy * src_h / dst_h;
        int sy1 = (dy + 1) * src_h / dst_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx0 = dx * src_w / dst_w;
            int sx1 = (dx + 1) * src_w / dst_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            int sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int sy = sy0; sy < sy1 && sy < src_h; sy++) {
                for (int sx = sx0; sx < sx1 && sx < src_w; sx++) {
                    const uint8_t* sp = src + (sy * src_w + sx) * bpp;
                    for (int c = 0; c < bpp; c++) sum[c] += sp[c];
                    n++;
                }
            }
            uint8_t* dp = dst + (dy * dst_w + dx) * bpp;
            if (n > 0) {
                for (int c = 0; c < bpp; c++) dp[c] = (uint8_t)(sum[c] / n);
            } else {
                for (int c = 0; c < bpp; c++) dp[c] = 0;
            }
        }
    }
}

struct BD_ScaleInfo {
    int orig_w, orig_h;
    int new_w, new_h;
    float scale;
};
static std::unordered_map<unsigned, BD_ScaleInfo> g_tex_scales;
static unsigned g_bound_tex_2d = 0;
// GL enums we need without dragging a second copy of the ES headers in.
static const GLenum BD_GL_TEXTURE_2D = 0x0DE1;
static const GLenum BD_GL_TEXTURE_EXTERNAL_OES = 0x8D65;
static const GLenum BD_GL_RGBA = 0x1908;
static const GLenum BD_GL_UNSIGNED_BYTE = 0x1401;
static const GLenum BD_GL_TEXTURE_MIN_FILTER = 0x2801;
static const GLenum BD_GL_TEXTURE_MAG_FILTER = 0x2800;
static const GLenum BD_GL_TEXTURE_WRAP_S = 0x2802;
static const GLenum BD_GL_TEXTURE_WRAP_T = 0x2803;
static const GLenum BD_GL_LINEAR = 0x2601;
static const GLenum BD_GL_CLAMP_TO_EDGE = 0x812F;
static const GLenum BD_GL_RED = 0x1903;
static const GLenum BD_GL_R8 = 0x8229;
static const GLenum BD_GL_READ_FRAMEBUFFER = 0x8CA8;
static const GLenum BD_GL_DRAW_FRAMEBUFFER = 0x8CA9;
static const GLenum BD_GL_COLOR_ATTACHMENT0 = 0x8CE0;
static const GLenum BD_GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

static uint64_t g_video_uploads = 0;
static uint64_t g_video_redirects = 0;
// Binds of the guest's video texture name to the 2D target that had to be
// swapped for the backing texture (see bd_video_bind_swap).
static uint64_t g_video_swaps = 0;
// Size the video texture was last allocated at, and the accumulated upload cost.
// Re-allocating per frame is not needed while the clip's size is unchanged, and
// the cost has to be visible in the log: on the handheld the upload runs on the
// guest's video thread, so it is on the critical path of every video frame.
static int g_video_tex_w = 0;
static int g_video_tex_h = 0;
static uint32_t g_upload_ms = 0;
static uint32_t g_yuv_blit_ms = 0;

struct BD_YuvGpu {
    GLuint textures[3]{};
    GLuint framebuffer{};
    GLuint program{};
    GLuint vertex_array{};
    GLint sampler[3]{-1, -1, -1};
    GLint flip{-1};
    GLint bt709{-1};
    GLint full_range{-1};
    int width{};
    int height{};
    bool failed{};
};
static BD_YuvGpu g_yuv_gpu;

// True once a decoded frame has been uploaded into the backing texture
// (bd_video_present). The UI geometry census (BD_VIDEO_TRACE_UI=2) starts from
// that moment rather than from a recognised video program, because a program
// served by Unity's shader cache never goes through glShaderSource.
static bool g_video_frame_uploaded = false;

// Attribute GL errors to the loader's own calls. Unity reports the errors it
// sees ("OPENGL NATIVE PLUG-IN ERROR: GL_INVALID_ENUM") without saying which
// call produced them, and this code sits exactly on the video path that is
// under suspicion, so drain the error queue after each redirect/upload and log
// the first few occurrences with the call that caused them.
void log_gl_error(const char* what)
{
    if (!glad_glGetError)
        return;
    static int logged = 0;
    GLenum error = glad_glGetError();
    if (error == 0 || logged >= 12)
        return;
    ++logged;
    BD_LOG("VIDEO", "GL error 0x%x after %s", (unsigned)error, what);
}

// Unity's video blit samples a GL_TEXTURE_EXTERNAL_OES texture, but on this
// device nothing ever queues a buffer into it (no BufferQueue, no gralloc - see
// javastubs/bd_video.h). Two observations drove this design:
//
//   * Unity binds its video texture once and keeps the binding (the external
//     bind happens a handful of times per clip, not per frame), so the frame
//     upload cannot live on the bind path;
//   * the GL texture name Unity binds (176) is not the handle it passed to
//     SurfaceTexture (22), so the bridge cannot match on the id either.
//
// So: every external bind during playback is redirected to one loader-owned
// GL_TEXTURE_2D, and the upload happens from SurfaceTexture.updateTexImage(),
// which is the guest's per-frame "new frame is ready" moment and runs with the
// GL context current. bd_glShaderSource rewrote the blit shader's
// samplerExternalOES to sampler2D, so that is the texture the shader reads.

// Creates the loader-owned GL_TEXTURE_2D that stands in for Unity's external
// texture. Must run with the GL context current. This can happen either from the
// first external bind or from an earlier updateTexImage(), so both paths call it.
static GLuint bd_ensure_backing_texture()
{
    GLuint backing = bd_video::backing_texture();
    if (backing != 0)
        return backing;
    if (!glad_glGenTextures || !glad_glBindTexture || !glad_glTexParameteri)
        return 0;
    GLint previous_texture = 0;
    const unsigned previous_shadow = g_bound_tex_2d;
    if (glad_glGetIntegerv)
        glad_glGetIntegerv(0x8069 /*TEXTURE_BINDING_2D*/, &previous_texture);
    glad_glGenTextures(1, &backing);
    glad_glBindTexture(BD_GL_TEXTURE_2D, backing);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_MIN_FILTER, BD_GL_LINEAR);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_MAG_FILTER, BD_GL_LINEAR);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_WRAP_S, BD_GL_CLAMP_TO_EDGE);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_WRAP_T, BD_GL_CLAMP_TO_EDGE);
    bd_video::set_backing_texture(backing);
    // Texture creation is private loader work, not a guest bind.
    glad_glBindTexture(BD_GL_TEXTURE_2D, (GLuint)previous_texture);
    g_bound_tex_2d = previous_shadow;
    // New texture: nothing is allocated yet, so the next upload must be a full
    // glTexImage2D, not a glTexSubImage2D into nothing.
    g_video_tex_w = 0;
    g_video_tex_h = 0;
    return backing;
}

static bool bd_redirect_video_bind(GLuint texture)
{
    const GLuint backing = bd_ensure_backing_texture();
    if (backing == 0)
        return false;
    glad_glBindTexture(BD_GL_TEXTURE_2D, backing);
    g_bound_tex_2d = backing;
    if (++g_video_redirects <= 5 || (g_video_redirects % 600) == 0)
        BD_LOG("VIDEO",
               "redirect external bind %u -> GL_TEXTURE_2D %u (redirect #%llu)",
               texture, backing, (unsigned long long)g_video_redirects);
    log_gl_error("redirect bind");
    return true;
}

static GLuint bd_compile_yuv_shader(GLenum type, const char* source)
{
    if (!glad_glCreateShader || !glad_glShaderSource || !glad_glCompileShader ||
        !glad_glGetShaderiv || !glad_glGetShaderInfoLog)
        return 0;
    const GLuint shader = glad_glCreateShader(type);
    if (!shader)
        return 0;
    glad_glShaderSource(shader, 1, &source, nullptr);
    glad_glCompileShader(shader);
    GLint ok = 0;
    glad_glGetShaderiv(shader, 0x8B81 /*GL_COMPILE_STATUS*/, &ok);
    if (!ok) {
        char message[1024]{};
        GLsizei length = 0;
        glad_glGetShaderInfoLog(shader, sizeof(message) - 1, &length, message);
        BD_LOG("VIDEO", "YUV shader compile failed: %s", message);
        if (glad_glDeleteShader)
            glad_glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool bd_init_yuv_gpu()
{
    if (g_yuv_gpu.program)
        return true;
    if (g_yuv_gpu.failed)
        return false;
    if (!glad_glCreateProgram || !glad_glAttachShader || !glad_glLinkProgram ||
        !glad_glGetProgramiv || !glad_glGetProgramInfoLog ||
        !glad_glGenTextures || !glad_glGenFramebuffers ||
        !glad_glGetUniformLocation) {
        g_yuv_gpu.failed = true;
        return false;
    }

    static const char* vertex_source =
        "#version 300 es\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "  v_uv = p;\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char* fragment_source =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 v_uv;\n"
        "layout(location=0) out vec4 out_color;\n"
        "uniform sampler2D tex_y;\n"
        "uniform sampler2D tex_u;\n"
        "uniform sampler2D tex_v;\n"
        "uniform bool flip_y;\n"
        "uniform bool use_bt709;\n"
        "uniform bool full_range;\n"
        "void main() {\n"
        "  vec2 uv = vec2(v_uv.x, flip_y ? 1.0-v_uv.y : v_uv.y);\n"
        "  float raw_y = texture(tex_y, uv).r * 255.0;\n"
        "  float raw_u = texture(tex_u, uv).r * 255.0 - 128.0;\n"
        "  float raw_v = texture(tex_v, uv).r * 255.0 - 128.0;\n"
        "  float y = full_range ? raw_y/255.0 : (raw_y-16.0)/219.0;\n"
        "  float u = raw_u / (full_range ? 255.0 : 224.0);\n"
        "  float v = raw_v / (full_range ? 255.0 : 224.0);\n"
        "  vec3 rgb;\n"
        "  if (use_bt709)\n"
        "    rgb = vec3(y+1.5748*v, y-0.187324*u-0.468124*v, y+1.8556*u);\n"
        "  else\n"
        "    rgb = vec3(y+1.402*v, y-0.344136*u-0.714136*v, y+1.772*u);\n"
        "  out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
        "}\n";
    const GLuint vertex = bd_compile_yuv_shader(0x8B31 /*GL_VERTEX_SHADER*/,
                                                 vertex_source);
    const GLuint fragment = bd_compile_yuv_shader(0x8B30 /*GL_FRAGMENT_SHADER*/,
                                                   fragment_source);
    if (!vertex || !fragment) {
        if (vertex && glad_glDeleteShader) glad_glDeleteShader(vertex);
        if (fragment && glad_glDeleteShader) glad_glDeleteShader(fragment);
        g_yuv_gpu.failed = true;
        return false;
    }
    const GLuint program = glad_glCreateProgram();
    glad_glAttachShader(program, vertex);
    glad_glAttachShader(program, fragment);
    glad_glLinkProgram(program);
    GLint linked = 0;
    glad_glGetProgramiv(program, 0x8B82 /*GL_LINK_STATUS*/, &linked);
    if (glad_glDeleteShader) {
        glad_glDeleteShader(vertex);
        glad_glDeleteShader(fragment);
    }
    if (!linked) {
        char message[1024]{};
        GLsizei length = 0;
        glad_glGetProgramInfoLog(program, sizeof(message) - 1, &length, message);
        BD_LOG("VIDEO", "YUV program link failed: %s", message);
        if (glad_glDeleteProgram)
            glad_glDeleteProgram(program);
        g_yuv_gpu.failed = true;
        return false;
    }

    g_yuv_gpu.program = program;
    g_yuv_gpu.sampler[0] = glad_glGetUniformLocation(program, "tex_y");
    g_yuv_gpu.sampler[1] = glad_glGetUniformLocation(program, "tex_u");
    g_yuv_gpu.sampler[2] = glad_glGetUniformLocation(program, "tex_v");
    g_yuv_gpu.flip = glad_glGetUniformLocation(program, "flip_y");
    g_yuv_gpu.bt709 = glad_glGetUniformLocation(program, "use_bt709");
    g_yuv_gpu.full_range =
        glad_glGetUniformLocation(program, "full_range");
    glad_glGenTextures(3, g_yuv_gpu.textures);
    glad_glGenFramebuffers(1, &g_yuv_gpu.framebuffer);
    if (glad_glGenVertexArrays)
        glad_glGenVertexArrays(1, &g_yuv_gpu.vertex_array);
    if (!g_yuv_gpu.textures[0] || !g_yuv_gpu.textures[1] ||
        !g_yuv_gpu.textures[2] || !g_yuv_gpu.framebuffer) {
        g_yuv_gpu.failed = true;
        return false;
    }
    BD_LOG("VIDEO", "YUV GPU path ready: program=%u textures=%u/%u/%u fbo=%u",
           program, g_yuv_gpu.textures[0], g_yuv_gpu.textures[1],
           g_yuv_gpu.textures[2], g_yuv_gpu.framebuffer);
    return true;
}

static void bd_set_texture_parameters()
{
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_MIN_FILTER, BD_GL_LINEAR);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_MAG_FILTER, BD_GL_LINEAR);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_WRAP_S, BD_GL_CLAMP_TO_EDGE);
    glad_glTexParameteri(BD_GL_TEXTURE_2D, BD_GL_TEXTURE_WRAP_T, BD_GL_CLAMP_TO_EDGE);
}

static bool bd_present_i420(const bd_video::UploadFrame& frame, GLuint backing)
{
    if (!bd_init_yuv_gpu())
        return false;

    GLint previous_program = 0;
    GLint previous_draw_fbo = 0;
    GLint previous_read_fbo = 0;
    GLint previous_viewport[4]{};
    GLint previous_active = 0;
    GLint previous_unpack = 4;
    GLint previous_unpack_row_length = 0;
    GLint previous_unpack_skip_rows = 0;
    GLint previous_unpack_skip_pixels = 0;
    GLint previous_unpack_buffer = 0;
    GLint previous_array_buffer = 0;
    GLint previous_vertex_array = 0;
    GLint previous_texture[3]{};
    GLint previous_sampler[3]{};
    const unsigned previous_shadow_bound = g_bound_tex_2d;
    GLboolean previous_color_mask[4]{1, 1, 1, 1};
    const GLenum toggles[] = {
        0x0BE2 /*BLEND*/, 0x0B71 /*DEPTH_TEST*/, 0x0B44 /*CULL_FACE*/,
        0x0C11 /*SCISSOR_TEST*/, 0x0B90 /*STENCIL_TEST*/,
        0x8C89 /*RASTERIZER_DISCARD*/, 0x0BD0 /*DITHER*/
    };
    GLboolean enabled[sizeof(toggles) / sizeof(toggles[0])]{};
    glad_glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &previous_program);
    glad_glGetIntegerv(0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/,
                       &previous_draw_fbo);
    glad_glGetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/,
                       &previous_read_fbo);
    glad_glGetIntegerv(0x0BA2 /*VIEWPORT*/, previous_viewport);
    glad_glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previous_active);
    glad_glGetIntegerv(0x0CF5 /*UNPACK_ALIGNMENT*/, &previous_unpack);
    glad_glGetIntegerv(0x0CF2 /*UNPACK_ROW_LENGTH*/,
                       &previous_unpack_row_length);
    glad_glGetIntegerv(0x0CF3 /*UNPACK_SKIP_ROWS*/,
                       &previous_unpack_skip_rows);
    glad_glGetIntegerv(0x0CF4 /*UNPACK_SKIP_PIXELS*/,
                       &previous_unpack_skip_pixels);
    glad_glGetIntegerv(0x88EF /*PIXEL_UNPACK_BUFFER_BINDING*/,
                       &previous_unpack_buffer);
    glad_glGetIntegerv(0x8894 /*ARRAY_BUFFER_BINDING*/,
                       &previous_array_buffer);
    if (glad_glBindVertexArray)
        glad_glGetIntegerv(0x85B5 /*VERTEX_ARRAY_BINDING*/,
                           &previous_vertex_array);
    if (glad_glGetBooleanv)
        glad_glGetBooleanv(0x0C23 /*COLOR_WRITEMASK*/, previous_color_mask);
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); ++i)
        enabled[i] = glad_glIsEnabled ? glad_glIsEnabled(toggles[i]) : false;
    for (int plane = 0; plane < 3; ++plane) {
        glad_glActiveTexture((GLenum)(0x84C5 + plane)); // texture units 5..7
        glad_glGetIntegerv(0x8069 /*TEXTURE_BINDING_2D*/,
                           &previous_texture[plane]);
        if (glad_glBindSampler)
            glad_glGetIntegerv(0x8919 /*SAMPLER_BINDING*/,
                               &previous_sampler[plane]);
    }

    const size_t y_size = (size_t)frame.width * (size_t)frame.height;
    const size_t uv_size = y_size / 4;
    const uint8_t* planes[] = {
        frame.pixels, frame.pixels + y_size, frame.pixels + y_size + uv_size
    };
    glad_glPixelStorei(0x0CF5 /*UNPACK_ALIGNMENT*/, 1);
    glad_glPixelStorei(0x0CF2 /*UNPACK_ROW_LENGTH*/, 0);
    glad_glPixelStorei(0x0CF3 /*UNPACK_SKIP_ROWS*/, 0);
    glad_glPixelStorei(0x0CF4 /*UNPACK_SKIP_PIXELS*/, 0);
    if (glad_glBindBuffer) {
        glad_glBindBuffer(0x88EC /*PIXEL_UNPACK_BUFFER*/, 0);
        glad_glBindBuffer(0x8892 /*ARRAY_BUFFER*/, 0);
    }
    for (int plane = 0; plane < 3; ++plane) {
        const int width = plane == 0 ? frame.width : frame.width / 2;
        const int height = plane == 0 ? frame.height : frame.height / 2;
        glad_glActiveTexture((GLenum)(0x84C5 + plane));
        if (glad_glBindSampler)
            glad_glBindSampler(5 + plane, 0);
        glad_glBindTexture(BD_GL_TEXTURE_2D, g_yuv_gpu.textures[plane]);
        if (frame.width != g_yuv_gpu.width || frame.height != g_yuv_gpu.height) {
            bd_set_texture_parameters();
            glad_glTexImage2D(BD_GL_TEXTURE_2D, 0, BD_GL_R8, width, height, 0,
                              BD_GL_RED, BD_GL_UNSIGNED_BYTE, planes[plane]);
        } else {
            glad_glTexSubImage2D(BD_GL_TEXTURE_2D, 0, 0, 0, width, height,
                                 BD_GL_RED, BD_GL_UNSIGNED_BYTE, planes[plane]);
        }
    }

    // The Unity-facing texture stays ordinary RGBA. Only how it is filled has
    // changed, so the existing SurfaceTexture/shader-cache contract is intact.
    glad_glActiveTexture(0x84C5 /*TEXTURE5*/);
    glad_glBindTexture(BD_GL_TEXTURE_2D, backing);
    if (frame.width != g_video_tex_w || frame.height != g_video_tex_h) {
        glad_glTexImage2D(BD_GL_TEXTURE_2D, 0, BD_GL_RGBA, frame.width,
                          frame.height, 0, BD_GL_RGBA, BD_GL_UNSIGNED_BYTE,
                          nullptr);
        g_video_tex_w = frame.width;
        g_video_tex_h = frame.height;
    }
    glad_glBindFramebuffer(BD_GL_DRAW_FRAMEBUFFER, g_yuv_gpu.framebuffer);
    glad_glFramebufferTexture2D(BD_GL_DRAW_FRAMEBUFFER, BD_GL_COLOR_ATTACHMENT0,
                                BD_GL_TEXTURE_2D, backing, 0);
    // Allocating/attaching backing above used texture unit 5, which is also the
    // Y sampler unit. Re-bind all three planes before drawing; sampling the
    // render target itself is an undefined feedback loop (Mali produced the
    // severely clipped almost-white frame seen in the first device capture).
    for (int plane = 0; plane < 3; ++plane) {
        glad_glActiveTexture((GLenum)(0x84C5 + plane));
        glad_glBindTexture(BD_GL_TEXTURE_2D, g_yuv_gpu.textures[plane]);
    }
    if (glad_glCheckFramebufferStatus(BD_GL_DRAW_FRAMEBUFFER) !=
        BD_GL_FRAMEBUFFER_COMPLETE) {
        BD_LOG("VIDEO", "YUV framebuffer incomplete");
        g_yuv_gpu.failed = true;
    } else {
        for (GLenum toggle : toggles)
            glad_glDisable(toggle);
        glad_glColorMask(true, true, true, true);
        glad_glViewport(0, 0, frame.width, frame.height);
        glad_glUseProgram(g_yuv_gpu.program);
        if (glad_glBindVertexArray && g_yuv_gpu.vertex_array)
            glad_glBindVertexArray(g_yuv_gpu.vertex_array);
        for (int plane = 0; plane < 3; ++plane) {
            if (g_yuv_gpu.sampler[plane] >= 0)
                glad_glUniform1i(g_yuv_gpu.sampler[plane], 5 + plane);
        }
        if (g_yuv_gpu.flip >= 0)
            glad_glUniform1i(g_yuv_gpu.flip, frame.flip ? 1 : 0);
        if (g_yuv_gpu.bt709 >= 0)
            glad_glUniform1i(
                g_yuv_gpu.bt709,
                frame.color_matrix == bd_video::ColorMatrix::BT709 ? 1 : 0);
        if (g_yuv_gpu.full_range >= 0)
            glad_glUniform1i(
                g_yuv_gpu.full_range,
                frame.color_range == bd_video::ColorRange::Full ? 1 : 0);
        glad_glDrawArrays(0x0004 /*GL_TRIANGLES*/, 0, 3);
    }

    glad_glBindFramebuffer(BD_GL_DRAW_FRAMEBUFFER,
                           (GLuint)previous_draw_fbo);
    glad_glBindFramebuffer(BD_GL_READ_FRAMEBUFFER,
                           (GLuint)previous_read_fbo);
    glad_glViewport(previous_viewport[0], previous_viewport[1],
                    previous_viewport[2], previous_viewport[3]);
    glad_glUseProgram((GLuint)previous_program);
    glad_glColorMask(previous_color_mask[0], previous_color_mask[1],
                     previous_color_mask[2], previous_color_mask[3]);
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); ++i) {
        if (enabled[i]) glad_glEnable(toggles[i]);
        else glad_glDisable(toggles[i]);
    }
    for (int plane = 0; plane < 3; ++plane) {
        glad_glActiveTexture((GLenum)(0x84C5 + plane));
        glad_glBindTexture(BD_GL_TEXTURE_2D, (GLuint)previous_texture[plane]);
        if (glad_glBindSampler)
            glad_glBindSampler(5 + plane, (GLuint)previous_sampler[plane]);
    }
    glad_glActiveTexture((GLenum)previous_active);
    glad_glPixelStorei(0x0CF5 /*UNPACK_ALIGNMENT*/, previous_unpack);
    glad_glPixelStorei(0x0CF2 /*UNPACK_ROW_LENGTH*/,
                       previous_unpack_row_length);
    glad_glPixelStorei(0x0CF3 /*UNPACK_SKIP_ROWS*/,
                       previous_unpack_skip_rows);
    glad_glPixelStorei(0x0CF4 /*UNPACK_SKIP_PIXELS*/,
                       previous_unpack_skip_pixels);
    if (glad_glBindBuffer) {
        glad_glBindBuffer(0x88EC /*PIXEL_UNPACK_BUFFER*/,
                          (GLuint)previous_unpack_buffer);
        if (glad_glBindVertexArray)
            glad_glBindVertexArray((GLuint)previous_vertex_array);
        glad_glBindBuffer(0x8892 /*ARRAY_BUFFER*/,
                          (GLuint)previous_array_buffer);
    } else if (glad_glBindVertexArray) {
        glad_glBindVertexArray((GLuint)previous_vertex_array);
    }
    g_bound_tex_2d = previous_shadow_bound;

    if (g_yuv_gpu.failed)
        return false;
    g_yuv_gpu.width = frame.width;
    g_yuv_gpu.height = frame.height;
    return true;
}

// Uploads the newest decoded frame into the backing texture. Installed into the
// video bridge, which calls it from SurfaceTexture.updateTexImage().
extern "C" void bd_video_present()
{
    if (!bd_video::has_sink())
        return;
    // updateTexImage() can run before Unity ever binds the external texture, so
    // create the backing texture on demand rather than waiting for the bind.
    const GLuint backing = bd_ensure_backing_texture();
    if (backing == 0 || !glad_glBindTexture || !glad_glTexImage2D)
        return;

    bd_video::UploadFrame frame;
    if (!bd_video::begin_upload(&frame))
        return;
    const int width = frame.width;
    const int height = frame.height;
    const uint8_t* pixels = frame.pixels;

    if (frame.format == bd_video::UploadFormat::I420) {
        const Uint32 start_ms = SDL_GetTicks();
        if (!bd_present_i420(frame, backing)) {
            bd_video::end_upload();
            bd_video::fallback_to_rgba_cpu("shader/FBO initialization failed");
            return;
        }
        bd_video::end_upload();
        g_video_frame_uploaded = true;
        const Uint32 cost = SDL_GetTicks() - start_ms;
        g_upload_ms += cost;
        g_yuv_blit_ms += cost;
        if (++g_video_uploads <= 3 || (g_video_uploads % 60) == 0)
            BD_DEBUG("VIDEO",
                   "uploaded YUV frame #%llu %dx%d -> texture %u (%.2f ms avg)",
                   (unsigned long long)g_video_uploads, width, height, backing,
                   (double)g_yuv_blit_ms / (double)g_video_uploads);
        log_gl_error("YUV upload/blit");
        return;
    }

    glad_glBindTexture(BD_GL_TEXTURE_2D, backing);
    // glTexSubImage2D in the steady state: same pixels, but it does not ask the
    // driver to re-allocate/re-layout the texture. glTexImage2D is only needed
    // when the size changes (the bridge can downscale a new clip differently).
    const Uint32 start_ms = SDL_GetTicks();
    if (width != g_video_tex_w || height != g_video_tex_h) {
        glad_glTexImage2D(BD_GL_TEXTURE_2D, 0, BD_GL_RGBA, width, height, 0,
                          BD_GL_RGBA, BD_GL_UNSIGNED_BYTE, pixels);
        g_video_tex_w = width;
        g_video_tex_h = height;
        BD_LOG("VIDEO", "video texture (re)allocated %dx%d", width, height);
    } else {
        glad_glTexSubImage2D(BD_GL_TEXTURE_2D, 0, 0, 0, width, height,
                             BD_GL_RGBA, BD_GL_UNSIGNED_BYTE, pixels);
    }
    g_bound_tex_2d = backing;
    bd_video::end_upload();
    g_video_frame_uploaded = true;
    g_upload_ms += SDL_GetTicks() - start_ms;
    if (++g_video_uploads <= 3 || (g_video_uploads % 60) == 0)
        BD_DEBUG("VIDEO", "uploaded frame #%llu %dx%d -> texture %u (%.2f ms avg)",
               (unsigned long long)g_video_uploads, width, height, backing,
               (double)g_upload_ms / (double)g_video_uploads);
    log_gl_error("TexImage2D (video upload)");
}

#include <map>

static std::map<GLuint, GLuint> g_shader_to_program;
static std::set<GLuint> g_rewritten_shaders;
static std::set<GLuint> g_video_programs;
// True while the guest's current program is one built from a rewritten video
// shader. Draw calls consult it to re-assert the video texture binding.
static bool g_video_program_active = false;

// Debug switches for the video GL path, read once:
//   1 = full program/attribute dump for draws that sample the video RT
//   2 = one compact census line per draw (see bd_quad_summary)
//   3 = also log which texture each unit holds, and the bind sequence
static int bd_trace_ui_mode()
{
    static int mode = -1;
    if (mode < 0) {
        const char* value = getenv("BD_VIDEO_TRACE_UI");
        mode = value && *value ? atoi(value) : 0;
    }
    return mode;
}

static bool bd_trace_ui_enabled()
{
    return bd_trace_ui_mode() > 0;
}

// Which texture is bound to each unit, tracked from the calls the guest makes.
// The question "does the video draw sample the loader's texture?" can only be
// answered at draw time, and the answer is the binding at the unit the shader
// samples - not the binding we set when we redirected the guest's external bind
// (that may be a different unit, or may have been overwritten since).
#define BD_MAX_UNITS 8
static GLenum g_active_unit = 0x84C0; // GL_TEXTURE0
static GLuint g_unit_2d[BD_MAX_UNITS] = {};
static GLuint g_unit_ext[BD_MAX_UNITS] = {};

static int unit_index(GLenum unit)
{
    const int index = (int)unit - 0x84C0;
    return (index >= 0 && index < BD_MAX_UNITS) ? index : -1;
}

// Names the guest has bound as GL_TEXTURE_EXTERNAL_OES while a video sink was
// live: its own handle on the video texture. Unity keeps using that name for the
// 2D target too (its state cache "restores" it), so it has to be recognised by
// name, not by the shader that is about to sample it.
static std::set<GLuint> g_guest_video_names;

// Is this the guest's name for the video texture? Both the name it binds as
// GL_TEXTURE_EXTERNAL_OES and the one the SurfaceTexture stub reports qualify:
// Unity uses them interchangeably depending on which cache it is reading.
static bool bd_is_guest_video_texture(GLuint texture)
{
    if (texture == 0)
        return false;
    if (g_guest_video_names.count(texture))
        return true;
    const int name = bd_video::video_texture_name();
    return name > 0 && (GLuint)name == texture;
}

// Unity binds its own (empty) video texture to GL_TEXTURE_2D right before the
// blit draw, which is what leaves the video black once the shader has been
// rewritten to sample a normal texture - and it is exactly the bind that has to
// be caught when the program came from the shader cache, because then no
// glShaderSource/glLinkProgram ever runs and fixup_video_bindings() has no
// program to key on. Swapping the name here makes the video independent of both
// Unity's state cache and its program cache.
static bool bd_video_bind_swap(GLuint texture)
{
    if (!bd_is_guest_video_texture(texture))
        return false;
    const GLuint backing = bd_ensure_backing_texture();
    if (backing == 0)
        return false;
    if (glad_glBindTexture)
        glad_glBindTexture(BD_GL_TEXTURE_2D, backing);
    g_bound_tex_2d = backing;
    const int index = unit_index(g_active_unit);
    if (index >= 0)
        g_unit_2d[index] = backing;
    if (++g_video_swaps <= 5 || (g_video_swaps % 600) == 0)
        BD_LOG("VIDEO",
               "video texture bind %u -> GL_TEXTURE_2D %u (bind swap #%llu)",
               texture, backing, (unsigned long long)g_video_swaps);
    return true;
}

static void track_texture_binding(GLenum target, GLuint texture)
{
    const int index = unit_index(g_active_unit);
    if (index < 0)
        return;
    if (target == BD_GL_TEXTURE_2D)
        g_unit_2d[index] = texture;
    else if (target == BD_GL_TEXTURE_EXTERNAL_OES)
        g_unit_ext[index] = texture;
}

extern "C" void bd_glActiveTexture(GLenum texture)
{
    g_active_unit = texture;
    if (glad_glActiveTexture)
        glad_glActiveTexture(texture);
}

extern "C" void bd_glBindTexture(GLenum target, GLuint texture)
{
    if (target == BD_GL_TEXTURE_2D) g_bound_tex_2d = texture;
    track_texture_binding(target, texture);
    // BD_VIDEO_TRACE_BINDS=1: a video frame's texture setup is a handful of
    // binds, and the order decides whether "the unit whose last bind was an
    // external texture" is a usable way to recognise a video draw. Unity's
    // shader cache can hand back a pre-compiled program, so recognising the
    // draw by shader source alone is not enough.
    if (bd_trace_ui_mode() >= 3 && bd_video::has_sink()) {
        static int logged = 0;
        if (logged < 40) {
            ++logged;
            BD_LOG("VIDEO", "bind #%d target=0x%x texture=%u unit=0x%x", logged,
                   (unsigned)target, texture, (unsigned)g_active_unit);
        }
    }
    // A GL_TEXTURE_EXTERNAL_OES cannot receive decoder output on this device,
    // so while a video sink is live, send it to the texture we can fill.
    if (target == BD_GL_TEXTURE_EXTERNAL_OES && bd_video::has_sink()) {
        // Remember the guest's own name for the video texture. Unity's GL state
        // cache re-binds that name to GL_TEXTURE_2D shortly before it draws the
        // video, which would undo the redirect below; knowing the name lets
        // bd_video_bind_swap() catch that bind as well.
        g_guest_video_names.insert(texture);
        if (bd_redirect_video_bind(texture)) {
            // The guest asked for the external target and got a 2D bind, so the
            // unit bookkeeping has to say so too: everything downstream (the
            // draw census, fixup_video_bindings) reads it as "what GL holds".
            const int index = unit_index(g_active_unit);
            if (index >= 0)
                g_unit_2d[index] = bd_video::backing_texture();
            return;
        }
    }
    // The same texture bound to the 2D target: Unity's state cache believes the
    // unit still holds the empty external texture and "restores" it. This is the
    // one case that has to work without knowing which program is about to draw:
    // with a cached (pre-compiled) video program there is no glShaderSource and
    // no glLinkProgram to key on, so fixup_video_bindings() never runs, and the
    // blit ends up sampling an empty texture - a black video with a healthy
    // decode. Substituting the backing texture here is what makes playback
    // independent of Unity's shader cache.
    if (target == BD_GL_TEXTURE_2D && bd_video::has_sink() &&
        bd_video_bind_swap(texture)) {
        return;
    }
    if (glad_glBindTexture) glad_glBindTexture(target, texture);
}

// Unity wraps its video texture around an EGLImage on real devices. There is no
// gralloc here, so the call can only make the texture unusable - and it would
// fight the GL_TEXTURE_2D we upload into. Report it and swallow it.
extern "C" void bd_glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image)
{
    BD_LOG("VIDEO", "glEGLImageTargetTexture2DOES(target=0x%x image=%p)",
           (unsigned)target, (void*)image);
    if (glad_glEGLImageTargetTexture2DOES)
        glad_glEGLImageTargetTexture2DOES(target, image);
}

// Unity's video blit shader is compiled for samplerExternalOES, which only reads
// a BufferQueue-backed external texture. Rewriting the sampler type makes the
// same shader read the GL_TEXTURE_2D that bd_glBindTexture installs above.
// ---------------------------------------------------------------------------
// Blit-program tracking.
//
// Rewriting the shader source only helps if the program built from it is the
// one Unity draws with, and if that program actually links. Both are invisible
// from the source-side log, so follow the shader through attach/link/use:
//
//   * glShaderSource rewrite   -> remember the shader object
//   * glAttachShader           -> remember which program took it
//   * glLinkProgram            -> report status and the info log for those
//                                 programs, and mark them as video programs
//   * glUseProgram             -> report the first uses of a marked program
//
// A marked program that is never used, or one whose link failed, explains a
// black video quad far more directly than any amount of texture-side logging.
#include <map>

extern "C" void bd_glShaderSource(GLuint shader, GLsizei count,
                                  const GLchar* const* string, const GLint* length)
{
    if (!glad_glShaderSource)
        return;
    if (!string || count <= 0 || shader == 0) {
        glad_glShaderSource(shader, count, string, length);
        return;
    }

    const char* needle = "samplerExternalOES";
    const size_t needle_len = strlen(needle);
    bool found = false;
    for (GLsizei i = 0; i < count && !found; i++) {
        if (string[i] && (!length || length[i] != 0) &&
            strstr(string[i], needle))
            found = true;
    }
    if (!found) {
        glad_glShaderSource(shader, count, string, length);
        return;
    }

    std::string source;
    for (GLsizei i = 0; i < count; i++) {
        if (!string[i])
            continue;
        if (length && length[i] >= 0)
            source.append(string[i], (size_t)length[i]);
        else
            source.append(string[i]);
    }
    unsigned replacements = 0;
    size_t pos = 0;
    while ((pos = source.find(needle, pos)) != std::string::npos) {
        source.replace(pos, needle_len, "sampler2D");
        pos += strlen("sampler2D");
        ++replacements;
    }
    const GLchar* ptr = source.c_str();
    GLint len = (GLint)source.size();
    g_rewritten_shaders.insert(shader);
    BD_LOG("VIDEO",
           "shader %u: samplerExternalOES -> sampler2D x%u (%zu bytes)",
           shader, replacements, source.size());
    // The video blit shader comes from the game's shader assets, not from
    // libunity.so, so dump it once to see how it samples the texture.
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        std::string snippet = source.substr(0, 2048);
        for (auto& ch : snippet) {
            if (ch == '\n') ch = ' ';
        }
        BD_LOG("VIDEO", "video shader source: %s", snippet.c_str());
    }
    glad_glShaderSource(shader, 1, &ptr, &len);
}

extern "C" void bd_glAttachShader(GLuint program, GLuint shader)
{
    if (glad_glAttachShader)
        glad_glAttachShader(program, shader);
    if (g_rewritten_shaders.count(shader)) {
        g_shader_to_program[shader] = program;
        BD_LOG("VIDEO", "attach: rewritten shader %u -> program %u", shader,
               program);
    }
}

extern "C" void bd_glLinkProgram(GLuint program)
{
    if (!glad_glLinkProgram)
        return;
    bool video_program = false;
    for (const auto& entry : g_shader_to_program) {
        if (entry.second == program) {
            video_program = true;
            break;
        }
    }
    glad_glLinkProgram(program);
    if (!video_program)
        return;
    GLint status = 0;
    glad_glGetProgramiv(program, GL_LINK_STATUS, &status);
    GLint log_len = 0;
    glad_glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
    std::string info;
    if (log_len > 1 && glad_glGetProgramInfoLog) {
        info.resize((size_t)log_len);
        GLsizei written = 0;
        glad_glGetProgramInfoLog(program, log_len, &written, &info[0]);
        info.resize((size_t)(written > 0 ? written : 0));
        for (auto& ch : info) {
            if (ch == '\n') ch = ' ';
        }
    }
    BD_LOG("VIDEO", "link program %u (video) status=%d log=\"%s\"", program,
           (int)status, info.c_str());
    if (status == 0)
        return;
    g_video_programs.insert(program);
}

// Unity's video draw samples the texture bound at whichever unit it bound its
// external texture to. Keep the loader's texture in place there, at the last
// moment before the draw: Unity's GL state cache believes that unit holds its
// own texture name (176) and re-binds it, silently undoing the redirect we did
// when the guest bound the external texture. Observed right before the draw:
//
//   video draw #1 program=33 unit_2d=[176 0 0 0] unit_ext=[179 0 0 0] backing=177
//
// A shader rewritten from samplerExternalOES to sampler2D then samples that
// empty texture, which is exactly the "black video, healthy decode" symptom.
// Any unit where the guest bound an external texture is a video unit, so the
// backing texture is always the intended binding there.
static int fixup_video_bindings()
{
    const GLuint backing = bd_video::backing_texture();
    if (backing == 0 || !glad_glBindTexture || !glad_glActiveTexture)
        return 0;
    int rebound = 0;
    for (int unit = 0; unit < BD_MAX_UNITS; unit++) {
        if (g_unit_ext[unit] == 0 || g_unit_2d[unit] == backing)
            continue;
        glad_glActiveTexture((GLenum)(0x84C0 + unit));
        glad_glBindTexture(BD_GL_TEXTURE_2D, backing);
        g_unit_2d[unit] = backing;
        ++rebound;
    }
    if (rebound)
        glad_glActiveTexture(g_active_unit);
    return rebound;
}

// ---------------------------------------------------------------------------
// Draw-time diagnostic: BD_VIDEO_DUMP_DRAW=1
//
// "The video quad shows the wrong part of the frame" cannot be answered from
// the texture side - the pixels, the binding and the shader rewrite are all
// correct - so the only remaining unknowns are the geometry and the texture
// coordinates the guest submits, plus the uniforms the program reads. Dump
// them for the first few video draws. Read-only: the array buffer binding is
// saved and restored, nothing else is touched.
// ---------------------------------------------------------------------------
static bool bd_dump_draw_enabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* value = getenv("BD_VIDEO_DUMP_DRAW");
        enabled = value && *value && strcmp(value, "0") != 0;
    }
    return enabled == 1;
}

// GL_FLOAT_VEC2..GL_FLOAT_MAT4 and friends -> element count, and whether the
// uniform has to be read with glGetUniformiv instead of glGetUniformfv.
static void bd_uniform_shape(GLenum type, int* elements, bool* integer)
{
    *integer = false;
    switch (type) {
        case 0x8B50: *elements = 2; break;  // FLOAT_VEC2
        case 0x8B51: *elements = 3; break;  // FLOAT_VEC3
        case 0x8B52: *elements = 4; break;  // FLOAT_VEC4
        case 0x8B5A: *elements = 4; break;  // FLOAT_MAT2
        case 0x8B5B: *elements = 9; break;  // FLOAT_MAT3
        case 0x8B5C: *elements = 16; break; // FLOAT_MAT4
        case 0x8B53: case 0x8B57: *elements = 2; *integer = true; break;
        case 0x8B54: case 0x8B58: *elements = 3; *integer = true; break;
        case 0x8B55: case 0x8B59: *elements = 4; *integer = true; break;
        case 0x8B5E: case 0x8B60: case 0x8B5F: case 0x8B62: case 0x8D66:
            *elements = 1; *integer = true; break;
        case 0x1404: case 0x8B56: *elements = 1; *integer = true; break;
        default: *elements = 1; break;      // FLOAT and anything unexpected
    }
}

static void bd_dump_program(GLint program)
{
    if (!glad_glGetProgramiv || !glad_glGetActiveAttrib ||
        !glad_glGetAttribLocation)
        return;
    GLint attribs = 0;
    glad_glGetProgramiv(program, 0x8B89 /*ACTIVE_ATTRIBUTES*/, &attribs);
    for (GLint i = 0; i < attribs && i < 12; i++) {
        char name[128] = {};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glad_glGetActiveAttrib(program, (GLuint)i, sizeof(name) - 1, &length,
                               &size, &type, name);
        BD_LOG("VIDEO", "  program %d attribute[%d] %s type=0x%x size=%d location=%d",
               program, i, name, (unsigned)type, size,
               glad_glGetAttribLocation(program, name));
    }
    if (!glad_glGetActiveUniform || !glad_glGetUniformfv ||
        !glad_glGetUniformLocation)
        return;
    GLint uniforms = 0;
    glad_glGetProgramiv(program, 0x8B86 /*ACTIVE_UNIFORMS*/, &uniforms);
    for (GLint i = 0; i < uniforms && i < 24; i++) {
        char name[128] = {};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glad_glGetActiveUniform(program, (GLuint)i, sizeof(name) - 1, &length,
                                &size, &type, name);
        GLint location = glad_glGetUniformLocation(program, name);
        if (location < 0)
            continue;
        int elements = 1;
        bool integer = false;
        bd_uniform_shape(type, &elements, &integer);
        float floats[16] = {};
        GLint ints[16] = {};
        // Unity's hlslcc matrices are declared as `vec4 name[4]`, so
        // glGetActiveUniform reports FLOAT_VEC4 with size 4 and reading the
        // location of "name[0]" returns only the first column. The translation
        // lives in the last column, so a half-dumped matrix cannot answer the
        // only question worth asking - where the quad actually lands. Read every
        // element when the name is an array.
        const std::string base(name, (size_t)length);
        const bool array_uniform =
            base.size() > 3 && base.compare(base.size() - 3, 3, "[0]") == 0;
        if (!integer && array_uniform && size > 1 && size <= 4) {
            elements = size * 4;
            for (GLint e = 0; e < size; e++) {
                const std::string element =
                    base.substr(0, base.size() - 3) + "[" + std::to_string(e) + "]";
                const GLint element_loc =
                    glad_glGetUniformLocation(program, element.c_str());
                if (element_loc >= 0)
                    glad_glGetUniformfv(program, element_loc, floats + e * 4);
            }
        } else if (integer) {
            if (glad_glGetUniformiv)
                glad_glGetUniformiv(program, location, ints);
        } else {
            glad_glGetUniformfv(program, location, floats);
        }
        std::string values;
        for (int e = 0; e < elements; e++) {
            char piece[32];
            if (integer)
                snprintf(piece, sizeof(piece), " %d", ints[e]);
            else
                snprintf(piece, sizeof(piece), " %.5f", floats[e]);
            values += piece;
        }
        BD_LOG("VIDEO", "  program %d uniform[%d] %s type=0x%x size=%d loc=%d ->%s",
               program, i, name, (unsigned)type, size, location, values.c_str());
    }
}

// Bytes of one component for the vertex attribute types GLES actually uses.
static int bd_component_bytes(GLenum type)
{
    switch (type) {
        case 0x1400: case 0x1401: return 1;               // BYTE / UNSIGNED_BYTE
        case 0x1402: case 0x1403: case 0x140B: return 2;  // SHORT / USHORT / HALF
        case 0x1404: case 0x1405: case 0x1406: return 4;  // INT / UINT / FLOAT
        case 0x140C: return 4;                            // FIXED
        default: return 4;
    }
}

// One vertex attribute, read straight out of whatever storage backs it. The
// quad Unity submits for the video blit is small enough that the interesting
// values (positions and texture coordinates) can be printed verbatim, which is
// the only way to tell "quad sized for another resolution" apart from "quad
// fine, viewport wrong".
//
// pnames matter: VERTEX_ATTRIB_ARRAY_STRIDE is 0x8624 and ..._TYPE is 0x8625
// (not the other way round), and the buffer binding is 0x889F, not 0x8869.
static void bd_dump_attribute(GLint index)
{
    GLint enabled = 0;
    if (!glad_glGetVertexAttribiv)
        return;
    glad_glGetVertexAttribiv(index, 0x8622 /*ENABLED*/, &enabled);
    if (!enabled)
        return;
    GLint size = 0;
    GLint type = 0;
    GLint stride = 0;
    GLint normalized = 0;
    GLint buffer = 0;
    glad_glGetVertexAttribiv(index, 0x8623 /*SIZE*/, &size);
    glad_glGetVertexAttribiv(index, 0x8625 /*TYPE*/, &type);
    glad_glGetVertexAttribiv(index, 0x8624 /*STRIDE*/, &stride);
    glad_glGetVertexAttribiv(index, 0x886A /*NORMALIZED*/, &normalized);
    glad_glGetVertexAttribiv(index, 0x889F /*BUFFER_BINDING*/, &buffer);
    void* pointer = nullptr;
    if (glad_glGetVertexAttribPointerv)
        glad_glGetVertexAttribPointerv(index, 0x8645 /*POINTER*/, &pointer);
    const int packed = size * bd_component_bytes((GLenum)type);
    const int step = stride > 0 ? stride : packed;
    BD_LOG("VIDEO",
           "  attrib[%d] size=%d type=0x%x normalized=%d stride=%d buffer=%d offset=%ld",
           index, size, (unsigned)type, normalized, stride, buffer,
           (long)(intptr_t)pointer);
    if (step <= 0 || step > 4096)
        return;

    const GLsizeiptr span = (GLsizeiptr)step * 4;
    uint8_t* base = nullptr;
    GLint previous = 0;
    const bool mapped = buffer != 0;
    if (mapped) {
        if (!glad_glMapBufferRange || !glad_glBindBuffer ||
            !glad_glGetIntegerv)
            return;
        glad_glGetIntegerv(0x8894 /*ARRAY_BUFFER_BINDING*/, &previous);
        glad_glBindBuffer(0x8892 /*ARRAY_BUFFER*/, (GLuint)buffer);
        base = (uint8_t*)glad_glMapBufferRange(0x8892, 0, span,
                                               0x0001 /*MAP_READ_BIT*/);
    } else {
        // GLES2 style client-side array: the pointer is a real host address.
        base = (uint8_t*)pointer;
    }
    if (!base) {
        if (mapped)
            glad_glBindBuffer(0x8892, (GLuint)previous);
        return;
    }

    for (int vertex = 0; vertex < 4; vertex++) {
        const uint8_t* record = base + (size_t)vertex * (size_t)step;
        std::string values;
        for (GLint c = 0; c < size; c++) {
            char piece[48];
            if (type == 0x1406 /*FLOAT*/) {
                if (normalized)
                    snprintf(piece, sizeof(piece), " %.5f",
                             ((const float*)record)[c]);
                else
                    snprintf(piece, sizeof(piece), " %.5f",
                             ((const float*)record)[c]);
            } else {
                int raw = 0;
                if (type == 0x1400 /*BYTE*/) raw = ((const int8_t*)record)[c];
                else if (type == 0x1401 /*U_BYTE*/) raw = record[c];
                else if (type == 0x1402 /*SHORT*/) raw = ((const int16_t*)record)[c];
                else if (type == 0x1403 /*U_SHORT*/) raw = ((const uint16_t*)record)[c];
                else if (type == 0x1405 /*U_INT*/) raw = (int)((const uint32_t*)record)[c];
                else if (type == 0x1404 /*INT*/) raw = ((const int32_t*)record)[c];
                else {
                    // Unknown layout: show the raw bytes so it is still readable.
                    snprintf(piece, sizeof(piece), " [%02x%02x%02x%02x]",
                             record[c * 4], record[c * 4 + 1], record[c * 4 + 2],
                             record[c * 4 + 3]);
                    values += piece;
                    continue;
                }
                snprintf(piece, sizeof(piece), " %d", raw);
            }
            values += piece;
        }
        BD_LOG("VIDEO", "    attrib[%d] vertex[%d]:%s", index, vertex,
               values.c_str());
    }

    if (mapped) {
        glad_glUnmapBuffer(0x8892);
        glad_glBindBuffer(0x8892, (GLuint)previous);
    }
}

// A UI draw is usually one batched call for dozens of widgets, so the first
// four vertices say nothing about where the video quad is. Scan the whole
// scanned range instead and print the extent: the batch's bounding box in its
// own space, which together with the MVP says where the batch lands on screen.
static void bd_dump_attribute_bounds(GLint index, GLsizei draw_vertices)
{
    GLint enabled = 0;
    GLint size = 0;
    GLint type = 0;
    GLint stride = 0;
    GLint buffer = 0;
    if (!glad_glGetVertexAttribiv || !glad_glMapBufferRange || !glad_glBindBuffer ||
        !glad_glGetIntegerv)
        return;
    glad_glGetVertexAttribiv(index, 0x8622 /*ENABLED*/, &enabled);
    if (!enabled)
        return;
    glad_glGetVertexAttribiv(index, 0x8623 /*SIZE*/, &size);
    glad_glGetVertexAttribiv(index, 0x8625 /*TYPE*/, &type);
    glad_glGetVertexAttribiv(index, 0x8624 /*STRIDE*/, &stride);
    glad_glGetVertexAttribiv(index, 0x889F /*BUFFER_BINDING*/, &buffer);
    if (type != 0x1406 /*FLOAT*/ || buffer == 0 || size <= 0 || size > 4)
        return;
    const int step = stride > 0 ? stride : size * 4;
    if (step <= 0 || step > 4096)
        return;
    GLsizei vertices = draw_vertices;
    if (vertices <= 0)
        vertices = 64;
    if (vertices > 4096)
        vertices = 4096;

    GLint previous = 0;
    glad_glGetIntegerv(0x8894 /*ARRAY_BUFFER_BINDING*/, &previous);
    glad_glBindBuffer(0x8892 /*ARRAY_BUFFER*/, (GLuint)buffer);
    // The draw's vertex count can exceed what the bound buffer holds (an
    // indexed draw only needs one vertex per index), and a map bigger than the
    // buffer just fails. Clamp to the buffer size so a batch of 43 quads still
    // reports its bounding box.
    GLint buffer_size = 0;
    if (glad_glGetBufferParameteriv)
        glad_glGetBufferParameteriv(0x8892, 0x8764 /*BUFFER_SIZE*/, &buffer_size);
    if (buffer_size > 0 && (GLsizeiptr)step * vertices > buffer_size)
        vertices = (GLsizei)(buffer_size / step);
    if (vertices <= 0) {
        glad_glBindBuffer(0x8892, (GLuint)previous);
        return;
    }
    const uint8_t* base = (const uint8_t*)glad_glMapBufferRange(
        0x8892, 0, (GLsizeiptr)step * vertices, 0x0001 /*MAP_READ_BIT*/);
    if (base) {
        float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f};
        float hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
        for (GLsizei vertex = 0; vertex < vertices; vertex++) {
            const float* record = (const float*)(base + (size_t)vertex * step);
            for (GLint c = 0; c < size; c++) {
                if (record[c] < lo[c]) lo[c] = record[c];
                if (record[c] > hi[c]) hi[c] = record[c];
            }
        }
        BD_LOG("VIDEO", "  attrib[%d] bounds over %d verts: min=(%.3f,%.3f,%.3f) "
                        "max=(%.3f,%.3f,%.3f)",
               index, vertices, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        glad_glUnmapBuffer(0x8892);
    }
    glad_glBindBuffer(0x8892, (GLuint)previous);
}

// The element buffer of an indexed draw: without it we know the vertex records
// but not which four of them the two triangles actually use.
static void bd_dump_indices(GLenum type, GLsizei count, const void* indices)
{
    if (!glad_glGetIntegerv || !glad_glBindBuffer || !glad_glMapBufferRange)
        return;
    GLint binding = 0;
    glad_glGetIntegerv(0x8895 /*ELEMENT_ARRAY_BUFFER_BINDING*/, &binding);
    const int width = type == 0x1405 /*UNSIGNED_INT*/ ? 4 : 2;
    const int shown = count < 12 ? (int)count : 12;
    std::string values;
    if (binding == 0) {
        // No element buffer: `indices` is a client array (already shown).
        BD_LOG("VIDEO", "  element buffer: none (client indices=%p)", indices);
        return;
    }
    glad_glBindBuffer(0x8893 /*ELEMENT_ARRAY_BUFFER*/, (GLuint)binding);
    uint8_t* base = (uint8_t*)glad_glMapBufferRange(
        0x8893, 0, (GLsizeiptr)width * shown, 0x0001 /*MAP_READ_BIT*/);
    if (base) {
        for (int i = 0; i < shown; i++) {
            const int value = width == 4 ? (int)((const uint32_t*)base)[i]
                                         : (int)((const uint16_t*)base)[i];
            char piece[16];
            snprintf(piece, sizeof(piece), " %d", value);
            values += piece;
        }
        BD_LOG("VIDEO", "  element buffer %d indices[%d]:%s", binding, shown,
               values.c_str());
        glad_glUnmapBuffer(0x8893);
    }
    glad_glBindBuffer(0x8893, (GLuint)binding);
}

static void bd_dump_video_draw(const char* kind, GLenum mode, GLsizei count,
                               GLenum type, const void* indices, GLint first,
                               GLsizei instances)
{
    static int dumps = 0;
    if (!bd_dump_draw_enabled() || dumps >= 3)
        return;
    ++dumps;
    GLint program = 0;
    GLint viewport[4] = {};
    GLint scissor[4] = {};
    GLint fbo = 0;
    glad_glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &program);
    glad_glGetIntegerv(0x0BA2 /*VIEWPORT*/, viewport);
    glad_glGetIntegerv(0x0C10 /*SCISSOR_BOX*/, scissor);
    glad_glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &fbo);
    // The viewport is the whole story for "the video only shows in one corner":
    // a quad built for 1280x720 drawn into a 640x480 viewport, or the reverse,
    // both look like a cropped image. fbo=0 means the real window; anything
    // else is a RenderTexture Unity blits from later.
    BD_LOG("VIDEO",
           "draw dump #%d %s mode=0x%x count=%d first=%d type=0x%x instances=%d "
           "indices=%p fbo=%d viewport=[%d %d %d %d] scissor=[%d %d %d %d] "
           "scissor_test=%d",
           dumps, kind, (unsigned)mode, count, first, (unsigned)type, instances,
           indices, fbo, viewport[0], viewport[1], viewport[2], viewport[3],
           scissor[0], scissor[1], scissor[2], scissor[3],
           glad_glIsEnabled ? (int)glad_glIsEnabled(0x0C11 /*SCISSOR_TEST*/) : -1);
    bd_dump_program(program);
    for (GLint i = 0; i < 8; i++)
        bd_dump_attribute(i);
    if (type == 0x1401 || type == 0x1403 || type == 0x1405)
        bd_dump_indices(type, count, indices);
}

// ---------------------------------------------------------------------------
// Where the video lands on screen: BD_VIDEO_TRACE_UI=1
//
// Unity's video pipeline draws twice. First the decode shader blits the decoder
// texture into the game's RenderTexture (that draw is the "video program", and
// the viewport dump shows the RT is 1280x720). Then the game shows that RT with
// a RawImage under UIIntro/Video/Video1, and at that point only the UI layer
// decides whether the clip is fitted or cropped.
//
// So remember the texture the decode blit rendered into, and when some later
// draw samples it, dump that draw's geometry: a quad whose clip-space extent
// leaves [-1, 1] is a quad larger than the screen, and on a 640x480 panel a
// 1280x720-sized quad anchored bottom-left shows exactly the bottom-left
// 640x480 of the clip - i.e. "the video is cropped", with the decoder innocent.
// ---------------------------------------------------------------------------
static GLuint g_video_rt_texture = 0;
static uint64_t g_ui_dumps = 0;

// Called for the decode blit: the currently bound framebuffer's colour
// attachment is the RenderTexture the game will sample later.
static void bd_remember_video_rt()
{
    if (!glad_glGetIntegerv) {
        BD_LOG("VIDEO", "video RT probe: glGetIntegerv unavailable");
        return;
    }
    GLint fbo = 0;
    glad_glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &fbo);
    // Middle-state LOG builds used to emit this every blit (~1/frame) and drown
    // publish / [BD-MEM] / seek worker lines. Keep first few + changes + sparse.
    static uint64_t blit_probe_logs = 0;
    static GLint last_logged_fbo = -1;
    ++blit_probe_logs;
    if (fbo != last_logged_fbo || blit_probe_logs <= 3 ||
        (blit_probe_logs % 120) == 0) {
        last_logged_fbo = fbo;
        BD_LOG("VIDEO", "video blit fbo=%d attachment-query=%s (n=%llu)", fbo,
               glad_glGetFramebufferAttachmentParameteriv ? "yes" : "no",
               (unsigned long long)blit_probe_logs);
    }
    if (fbo == 0 || !glad_glGetFramebufferAttachmentParameteriv)
        return;
    GLint texture = 0;
    glad_glGetFramebufferAttachmentParameteriv(
        0x8D40 /*FRAMEBUFFER*/, 0x8CE0 /*COLOR_ATTACHMENT0*/,
        0x8CD1 /*FRAMEBUFFER_ATTACHMENT_OBJECT_NAME*/, &texture);
    // A renderbuffer attachment answers here too (with its own name), so this
    // is "the attachment", not necessarily a texture.
    if (texture == 0) {
        static uint64_t no_name_logs = 0;
        if (++no_name_logs <= 3 || (no_name_logs % 120) == 0)
            BD_LOG("VIDEO", "video blit attachment 0 has no name (n=%llu)",
                   (unsigned long long)no_name_logs);
        return;
    }
    GLint viewport[4] = {};
    glad_glGetIntegerv(0x0BA2 /*VIEWPORT*/, viewport);
    // The blit covers the viewport, so a viewport smaller than the RT leaves the
    // clip in one corner of it - and every later draw of that RT (the RawImage)
    // then shows a cropped picture even though its own quad is the right size.
    // Log both numbers together to tell that apart from a mis-sized UI quad.
    GLint rt_w = 0;
    GLint rt_h = 0;
    if (glad_glActiveTexture && glad_glGetTexLevelParameteriv) {
        GLint previous_unit = 0;
        GLint previous_tex = 0;
        glad_glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previous_unit);
        glad_glActiveTexture(0x84C0 /*TEXTURE0*/);
        glad_glGetIntegerv(0x8069 /*TEXTURE_BINDING_2D*/, &previous_tex);
        glad_glBindTexture(0x0DE1 /*TEXTURE_2D*/, (GLuint)texture);
        glad_glGetTexLevelParameteriv(0x0DE1, 0, 0x1000, &rt_w);
        glad_glGetTexLevelParameteriv(0x0DE1, 0, 0x1001, &rt_h);
        glad_glBindTexture(0x0DE1, (GLuint)previous_tex);
        glad_glActiveTexture((GLenum)previous_unit);
    }
    if ((GLuint)texture == g_video_rt_texture)
        return;
    g_video_rt_texture = (GLuint)texture;
    BD_LOG("VIDEO", "video blit target attachment 0 is object %u size=%dx%d "
                    "viewport=[%d %d %d %d]",
           g_video_rt_texture, rt_w, rt_h, viewport[0], viewport[1], viewport[2],
           viewport[3]);
}

// A draw that samples the video RT: dump it once so the mapping is on record.
static void bd_maybe_dump_ui_draw(const char* kind, GLenum mode, GLsizei count,
                                  GLenum type, const void* indices, GLint first,
                                  GLsizei instances)
{
    if (!bd_trace_ui_enabled() || g_video_rt_texture == 0 || g_ui_dumps >= 4)
        return;
    bool samples_video = false;
    for (int unit = 0; unit < BD_MAX_UNITS; unit++)
        if (g_unit_2d[unit] == g_video_rt_texture)
            samples_video = true;
    if (!samples_video)
        return;

    ++g_ui_dumps;
    GLint viewport[4] = {};
    GLint program = 0;
    GLint fbo = 0;
    glad_glGetIntegerv(0x0BA2 /*VIEWPORT*/, viewport);
    glad_glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &program);
    glad_glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &fbo);
    BD_LOG("VIDEO",
           "UI draw #%llu samples the video RT %u: %s count=%d first=%d program=%d "
           "fbo=%d viewport=[%d %d %d %d] unit0=%u",
           (unsigned long long)g_ui_dumps, g_video_rt_texture, kind, count, first,
           program, fbo, viewport[0], viewport[1], viewport[2], viewport[3],
           g_unit_2d[0]);
    bd_dump_program(program);
    for (GLint i = 0; i < 8; i++)
        bd_dump_attribute(i);
    if (type == 0x1401 || type == 0x1403 || type == 0x1405)
        bd_dump_indices(type, count, indices);
}

// Which sampler uniforms a program declares, cached because a UI frame issues
// dozens of draws and re-querying the interface every time is wasteful.
struct BdSampler
{
    std::string name;
    GLint location = -1;
    GLenum type = 0;
};
static std::map<GLuint, std::vector<BdSampler>> g_program_samplers;

// Does this draw actually *read* the video? The question cannot be answered by
// looking at the bound textures: Unity draws plenty of passes whose shader has
// no sampler at all, and those inherit whatever texture the last bind left on
// unit 0. Ask the program which samplers it has, read the texture unit each one
// points at, and compare that with the video textures. Only a draw that samples
// the video texture can put the video on screen, so only that draw's transform
// and geometry matter.
static bool bd_draw_samples_video(GLint program, std::string* sampler,
                                  int* unit_index, GLuint* texture)
{
    if (!glad_glGetActiveUniform || !glad_glGetUniformLocation ||
        !glad_glGetUniformiv || !glad_glGetProgramiv)
        return false;
    const GLuint key = (GLuint)program;
    auto entry = g_program_samplers.find(key);
    if (entry == g_program_samplers.end()) {
        std::vector<BdSampler> found;
        GLint uniforms = 0;
        glad_glGetProgramiv(program, 0x8B86 /*ACTIVE_UNIFORMS*/, &uniforms);
        for (GLint i = 0; i < uniforms && i < 64; i++) {
            char name[128] = {};
            GLsizei length = 0;
            GLint size = 0;
            GLenum type = 0;
            glad_glGetActiveUniform(program, (GLuint)i, sizeof(name) - 1, &length,
                                    &size, &type, name);
            const bool is_sampler =
                (type >= 0x8B5E && type <= 0x8B62) || type == 0x8D66;
            if (!is_sampler)
                continue;
            BdSampler info;
            info.name.assign(name, (size_t)length);
            info.location = glad_glGetUniformLocation(program, name);
            info.type = type;
            found.push_back(info);
        }
        entry = g_program_samplers.emplace(key, std::move(found)).first;
    }

    const GLuint backing = bd_video::backing_texture();
    for (const BdSampler& info : entry->second) {
        if (info.location < 0)
            continue;
        GLint unit = -1;
        glad_glGetUniformiv(program, info.location, &unit);
        if (unit < 0 || unit >= BD_MAX_UNITS)
            continue;
        const GLuint bound_2d = g_unit_2d[unit];
        const GLuint bound_ext = g_unit_ext[unit];
        const bool is_video =
            (backing != 0 &&
             (bound_2d == backing || bound_ext == backing)) ||
            (g_video_rt_texture != 0 && bound_2d == g_video_rt_texture);
        if (!is_video)
            continue;
        if (sampler)
            *sampler = info.name;
        if (unit_index)
            *unit_index = unit;
        if (texture)
            *texture = bound_2d != 0 ? bound_2d : bound_ext;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Geometry census for the video: BD_VIDEO_TRACE_UI=2
//
// Once the video texture exists, the draws that sample it are the whole story
// of how the clip reaches the screen, so print them in full: program uniforms
// (the MVP matrix included), every vertex attribute with size/stride, and the
// index buffer. A quad whose positions are sized for a canvas other than 640x480
// is a quad laid out for a different screen, which is how "only part of the
// video is visible" happens.
static int g_census_left = 0;
static bool g_census_started = false;
static uint64_t g_census_seen = 0;

// Name/type/unit/current-texture of every sampler a program declares, as one
// string. Printed for every draw in the census window: whether the video is on
// screen at all comes down to which draw reads which texture, and this is the
// line that says so without dumping megabytes.
static std::string bd_sampler_summary(GLint program)
{
    std::string out;
    if (!glad_glGetActiveUniform || !glad_glGetUniformLocation ||
        !glad_glGetUniformiv || !glad_glGetProgramiv)
        return "(no sampler API)";
    GLint uniforms = 0;
    glad_glGetProgramiv(program, 0x8B86 /*ACTIVE_UNIFORMS*/, &uniforms);
    for (GLint i = 0; i < uniforms && i < 8; i++) {
        char name[128] = {};
        GLsizei length = 0;
        GLint size = 0;
        GLenum type = 0;
        glad_glGetActiveUniform(program, (GLuint)i, sizeof(name) - 1, &length,
                                &size, &type, name);
        const bool is_sampler =
            (type >= 0x8B5E && type <= 0x8B62) || type == 0x8D66;
        if (!is_sampler)
            continue;
        const GLint location = glad_glGetUniformLocation(program, name);
        GLint unit = -1;
        if (location >= 0)
            glad_glGetUniformiv(program, location, &unit);
        char piece[192];
        if (unit >= 0 && unit < BD_MAX_UNITS) {
            // The dimensions answer "is the sampler reading the game's
            // 1280x720 video RT, a 640x360 decoded frame, or a sprite atlas?"
            // without guessing from the texture name. Each unit already has the
            // right texture bound, so only the active unit needs switching.
            GLint width = 0;
            GLint height = 0;
            if (glad_glActiveTexture && glad_glGetTexLevelParameteriv &&
                g_unit_2d[unit] != 0) {
                GLint previous = 0;
                glad_glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previous);
                glad_glActiveTexture((GLenum)(0x84C0 + unit));
                glad_glGetTexLevelParameteriv(0x0DE1 /*TEXTURE_2D*/, 0, 0x1000,
                                              &width);
                glad_glGetTexLevelParameteriv(0x0DE1 /*TEXTURE_2D*/, 0, 0x1001,
                                              &height);
                glad_glActiveTexture((GLenum)previous);
            }
            snprintf(piece, sizeof(piece), " %s(0x%x)@%d=2d:%u(%dx%d),ext:%u",
                     name, (unsigned)type, unit, g_unit_2d[unit], width, height,
                     g_unit_ext[unit]);
        } else {
            snprintf(piece, sizeof(piece), " %s(0x%x)@%d", name, (unsigned)type,
                     unit);
        }
        out += piece;
    }
    return out.empty() ? "(none)" : out;
}

static void bd_quad_summary(const char* kind, GLenum mode, GLsizei count,
                            GLenum type, const void* indices)
{
    if (bd_trace_ui_mode() < 2)
        return;
    if (g_census_left <= 0) {
        // Start counting once a video frame is on the GPU: every draw from here
        // on is a candidate for putting it on screen.
        if (!g_video_frame_uploaded || g_census_started)
            return;
        g_census_started = true;
        g_census_left = 300;
        BD_LOG("VIDEO", "--- geometry census: every draw from the first uploaded "
                        "video frame (300 max) ---");
    }
    --g_census_left;
    ++g_census_seen;

    GLint program = 0;
    GLint fbo = 0;
    GLint viewport[4] = {};
    glad_glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &program);
    glad_glGetIntegerv(0x0BA2 /*VIEWPORT*/, viewport);
    glad_glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &fbo);
    BD_LOG("VIDEO",
           "census #%llu %s count=%d program=%d fbo=%d vp=[%d %d %d %d] "
           "units_2d=[%u %u %u %u] units_ext=[%u %u %u %u] backing=%u rt=%u "
           "samplers:%s",
           (unsigned long long)g_census_seen, kind, count, program, fbo,
           viewport[0], viewport[1], viewport[2], viewport[3], g_unit_2d[0],
           g_unit_2d[1], g_unit_2d[2], g_unit_2d[3], g_unit_ext[0], g_unit_ext[1],
           g_unit_ext[2], g_unit_ext[3], bd_video::backing_texture(),
           g_video_rt_texture, bd_sampler_summary(program).c_str());

    std::string sampler;
    int unit = -1;
    GLuint texture = 0;
    const bool samples_video =
        bd_draw_samples_video(program, &sampler, &unit, &texture);

    // The clip does not necessarily reach the screen through the texture the
    // loader recognises: Unity can blit it into a RenderTexture of its own and
    // hand the RawImage that one, in which case nothing here mentions the
    // backing texture and no draw ever samples the video RT either. A RawImage
    // still submits a quad, though, so keep dumping quad-sized draws: their MVP
    // says where on screen they land, which is the only question left.
    if (!samples_video && count > 12)
        return;

    // A draw that samples the video *into* an FBO is Unity's own video blit, so
    // that FBO's colour attachment is the RenderTexture the UI will show later.
    // Remember it here: the guard that normally does this only runs for programs
    // the loader rewrote, and with the bind swap alone no rewrite is needed.
    if (samples_video && fbo != 0) {
        bd_remember_video_rt();
        bd_maybe_dump_ui_draw(kind, mode, count, type, indices, 0, 0);
    }

    if (samples_video) {
        BD_LOG("VIDEO",
               "=== the video itself: %s count=%d program=%d fbo=%d sampler=%s "
               "unit=%d texture=%u",
               kind, count, program, fbo, sampler.c_str(), unit, texture);
    } else {
        BD_LOG("VIDEO",
               "--- small draw %s count=%d program=%d fbo=%d (no video sampler)",
               kind, count, program, fbo);
    }
    if (glad_glGetActiveAttrib && glad_glGetAttribLocation) {
        GLint attribs = 0;
        glad_glGetProgramiv(program, 0x8B89 /*ACTIVE_ATTRIBUTES*/, &attribs);
        for (GLint i = 0; i < attribs && i < 8; i++) {
            char name[128] = {};
            GLsizei length = 0;
            GLint size = 0;
            GLenum attrib_type = 0;
            glad_glGetActiveAttrib(program, (GLuint)i, sizeof(name) - 1, &length,
                                   &size, &attrib_type, name);
            const GLint location = glad_glGetAttribLocation(program, name);
            GLint enabled = 0;
            GLint buffer = 0;
            GLint stride = 0;
            if (location >= 0)
                glad_glGetVertexAttribiv((GLuint)location, 0x8622 /*ENABLED*/,
                                         &enabled);
            if (location >= 0)
                glad_glGetVertexAttribiv((GLuint)location, 0x889F /*BUFFER*/,
                                         &buffer);
            if (location >= 0)
                glad_glGetVertexAttribiv((GLuint)location, 0x8624 /*STRIDE*/,
                                         &stride);
            BD_LOG("VIDEO", "    attribute %d %s type=0x%x size=%d location=%d "
                            "enabled=%d buffer=%d stride=%d",
                   i, name, (unsigned)attrib_type, size, location, enabled, buffer,
                   stride);
        }
    }
    for (GLint i = 0; i < 8; i++)
        bd_dump_attribute(i);
    for (GLint i = 0; i < 4; i++)
        bd_dump_attribute_bounds(i, count);
    if (type == 0x1401 || type == 0x1403 || type == 0x1405)
        bd_dump_indices(type, count, indices);
    bd_dump_program(program);
}

// ---------------------------------------------------------------------------
// Which texture holds the video: BD_VIDEO_TRACE_UI=3
//
// The shader reads whichever texture is bound at the unit its sampler uses. If
// that turns out to be a texture Unity itself created for the video (i.e. it has
// the clip's dimensions and is used nowhere else), the decoded frames can be
// uploaded straight into it and the loader's own redirect texture - plus the
// per-frame rebinding and the shader rewrite that goes with it - become
// unnecessary. That would also make playback independent of the shader cache,
// which is what silently kills the video when Unity loads a pre-compiled
// program. Log the sizes to tell the two cases apart.
static void bd_dump_unit_textures()
{
    if (bd_trace_ui_mode() < 3)
        return;
    static int logged = 0;
    if (logged >= 6)
        return;
    ++logged;

    GLint previous_unit = 0;
    glad_glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previous_unit);
    for (int unit = 0; unit < 4; unit++) {
        if (g_unit_2d[unit] == 0 && g_unit_ext[unit] == 0)
            continue;
        glad_glActiveTexture((GLenum)(0x84C0 + unit));
        const GLuint textures[2] = {g_unit_2d[unit], g_unit_ext[unit]};
        const GLenum targets[2] = {BD_GL_TEXTURE_2D, BD_GL_TEXTURE_EXTERNAL_OES};
        for (int i = 0; i < 2; i++) {
            if (textures[i] == 0)
                continue;
            GLint width = -1;
            GLint height = -1;
            if (glad_glBindTexture && glad_glGetTexLevelParameteriv) {
                glad_glBindTexture(targets[i], textures[i]);
                glad_glGetTexLevelParameteriv(targets[i], 0, 0x1000 /*WIDTH*/,
                                              &width);
                glad_glGetTexLevelParameteriv(targets[i], 0, 0x1001 /*HEIGHT*/,
                                              &height);
            }
            BD_LOG("VIDEO", "unit %d %s texture %u is %dx%d (backing=%u)", unit,
                   i == 0 ? "2d " : "ext", textures[i], width, height,
                   bd_video::backing_texture());
        }
    }
    glad_glActiveTexture((GLenum)previous_unit);
}

template <typename DrawFn>
static void video_draw_guard(DrawFn&& draw)
{
    if (g_video_program_active) {
        bd_dump_unit_textures();
        bd_remember_video_rt();
        const int rebound = fixup_video_bindings();
        if (rebound) {
            static uint64_t fixed = 0;
            if (++fixed <= 6 || (fixed % 300) == 0)
                BD_LOG("VIDEO",
                       "re-bound backing at %d unit(s) before video draw (#%llu)",
                       rebound, (unsigned long long)fixed);
        }
    }
    draw();
}

extern "C" void bd_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    if (g_video_program_active)
        bd_dump_video_draw("drawArrays", mode, count, 0, nullptr, first, 0);
    else {
        bd_maybe_dump_ui_draw("drawArrays", mode, count, 0, nullptr, first, 0);
        bd_quad_summary("drawArrays", mode, count, 0, nullptr);
    }
    video_draw_guard([&] {
        if (glad_glDrawArrays)
            glad_glDrawArrays(mode, first, count);
    });
}

extern "C" void bd_glDrawElements(GLenum mode, GLsizei count, GLenum type,
                                   const void* indices)
{
    if (g_video_program_active)
        bd_dump_video_draw("drawElements", mode, count, type, indices, 0, 0);
    else {
        bd_maybe_dump_ui_draw("drawElements", mode, count, type, indices, 0, 0);
        bd_quad_summary("drawElements", mode, count, type, indices);
    }
    video_draw_guard([&] {
        if (glad_glDrawElements)
            glad_glDrawElements(mode, count, type, indices);
    });
}

extern "C" void bd_glDrawArraysInstanced(GLenum mode, GLint first, GLsizei count,
                                         GLsizei instances)
{
    if (g_video_program_active)
        bd_dump_video_draw("drawArraysInstanced", mode, count, 0, nullptr, first,
                           instances);
    else
        bd_quad_summary("drawArraysInstanced", mode, count, 0, nullptr);
    video_draw_guard([&] {
        if (glad_glDrawArraysInstanced)
            glad_glDrawArraysInstanced(mode, first, count, instances);
    });
}

extern "C" void bd_glDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                           const void* indices, GLsizei instances)
{
    if (g_video_program_active)
        bd_dump_video_draw("drawElementsInstanced", mode, count, type, indices, 0,
                           instances);
    else
        bd_quad_summary("drawElementsInstanced", mode, count, type, indices);
    video_draw_guard([&] {
        if (glad_glDrawElementsInstanced)
            glad_glDrawElementsInstanced(mode, count, type, indices, instances);
    });
}

extern "C" void bd_glUseProgram(GLuint program)
{
    if (glad_glUseProgram)
        glad_glUseProgram(program);
    g_video_program_active = g_video_programs.count(program) > 0;
    if (!g_video_program_active)
        return;
    static uint64_t uses = 0;
    ++uses;
    if (uses <= 12 || (uses % 120) == 0) {
        BD_DEBUG("VIDEO",
               "video draw #%llu program=%u unit_2d=[%u %u %u %u] unit_ext=[%u %u %u %u] backing=%u",
               (unsigned long long)uses, program, g_unit_2d[0], g_unit_2d[1],
               g_unit_2d[2], g_unit_2d[3], g_unit_ext[0], g_unit_ext[1],
               g_unit_ext[2], g_unit_ext[3], bd_video::backing_texture());
    }
}

extern "C" void bd_glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    if (target == BD_GL_TEXTURE_EXTERNAL_OES && bd_video::backing_texture() != 0 &&
        g_bound_tex_2d == bd_video::backing_texture())
        target = BD_GL_TEXTURE_2D;
    if (glad_glTexParameteri) glad_glTexParameteri(target, pname, param);
}

extern "C" void bd_glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    if (target == BD_GL_TEXTURE_EXTERNAL_OES && bd_video::backing_texture() != 0 &&
        g_bound_tex_2d == bd_video::backing_texture())
        target = BD_GL_TEXTURE_2D;
    if (glad_glTexParameterf) glad_glTexParameterf(target, pname, param);
}

extern "C" void bd_glDeleteTextures(GLsizei n, const GLuint* textures)
{
    if (textures) {
        for (GLsizei i = 0; i < n; i++) {
            g_tex_scales.erase((unsigned)textures[i]);
            if (textures[i] == g_bound_tex_2d) g_bound_tex_2d = 0;
            if (textures[i] == bd_video::backing_texture())
                bd_video::set_backing_texture(0);
            // A deleted name can be handed out again, so it must not stay on the
            // "this is the video texture" list.
            g_guest_video_names.erase(textures[i]);
        }
    }
    if (glad_glDeleteTextures) glad_glDeleteTextures(n, textures);
}

extern "C" void bd_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    // The video blit legitimately runs in a 1280x720 viewport (Unity's video
    // RenderTexture is the clip's size), so a wrong viewport there is normal.
    // What cannot be normal is the DEFAULT framebuffer being set up for anything
    // other than the drawable: GL clamps rasterization to the framebuffer, so a
    // 1280x720 viewport on a 640x480 window shows the bottom-left quadrant of
    // everything - UI and video alike - which reads as "the video is cropped".
    if (glad_glGetIntegerv) {
        GLint fbo = 0;
        glad_glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &fbo);
        const int drawable_w = bd_video::display_width();
        const int drawable_h = bd_video::display_height();
        if (fbo == 0 && drawable_w > 0 &&
            (width != drawable_w || height != drawable_h)) {
            static uint64_t offscreen = 0;
            if (++offscreen <= 5 || (offscreen % 600) == 0)
                BD_LOG("VIDEO",
                       "screen viewport #%llu is %dx%d+%d+%d but the drawable is %dx%d",
                       (unsigned long long)offscreen, width, height, x, y,
                       drawable_w, drawable_h);
        }
    }
    if (glad_glViewport)
        glad_glViewport(x, y, width, height);
}

extern "C" void bd_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                   GLsizei width, GLsizei height)
{
    GLsizei out_w = width, out_h = height;
    int max_dim = bd_cap_for_format(internalformat);
    int short_side = std::min(width, height);
    int long_side  = std::max(width, height);
    bool looks_like_lut = (short_side <= 32) || (long_side > 4 * short_side);
    int disp_w = bd_device_display_width();
    int disp_h = bd_device_display_height();
    bool looks_like_rt =
        (disp_w > 0 && (int)width == disp_w && (int)height == disp_h);

    if (target == 0x0DE1 /*GL_TEXTURE_2D*/
        && max_dim > 0
        && long_side > max_dim
        && !looks_like_lut
        && !looks_like_rt) {
        float s = (float)max_dim / (float)long_side;
        int nw = std::max(4, ((int)(width * s) / 4) * 4);
        int nh = std::max(4, ((int)(height * s) / 4) * 4);
        if (g_bound_tex_2d != 0) {
            g_tex_scales[g_bound_tex_2d] =
                BD_ScaleInfo{(int)width, (int)height, nw, nh, (float)nw / (float)width};
        }
        out_w = nw; out_h = nh;
    }
    if (glad_glTexStorage2D) {
        glad_glTexStorage2D(target, levels, internalformat, out_w, out_h);
    }
}

extern "C" void bd_glTexSubImage2D(GLenum target, GLint level,
                                    GLint xoffset, GLint yoffset,
                                    GLsizei width, GLsizei height,
                                    GLenum format, GLenum type, const void* pixels)
{
    auto it = g_tex_scales.find(g_bound_tex_2d);
    if (it == g_tex_scales.end() || target != 0x0DE1 /*GL_TEXTURE_2D*/) {
        if (glad_glTexSubImage2D)
            glad_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
        return;
    }
    int bpp = bd_bpp_from_format_type(format, type);
    if (bpp == 0 || pixels == nullptr) {
        if (glad_glTexSubImage2D)
            glad_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
        return;
    }
    const auto& info = it->second;
    int new_x = (int)(xoffset * info.scale);
    int new_y = (int)(yoffset * info.scale);
    int new_w = (int)(width * info.scale);
    int new_h = (int)(height * info.scale);
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;
    int level_w = std::max(1, info.new_w >> level);
    int level_h = std::max(1, info.new_h >> level);
    if (new_x + new_w > level_w) new_w = level_w - new_x;
    if (new_y + new_h > level_h) new_h = level_h - new_y;
    if (new_w <= 0 || new_h <= 0) return;
    std::vector<uint8_t> resized((size_t)new_w * (size_t)new_h * (size_t)bpp);
    bd_box_downsample((const uint8_t*)pixels, (int)width, (int)height,
                      resized.data(), new_w, new_h, bpp);
    if (glad_glTexSubImage2D) {
        glad_glTexSubImage2D(target, level, new_x, new_y, new_w, new_h, format, type, resized.data());
    }
}

static void bd_symtable_override(const char* sym, uintptr_t fn)
{
    for (int i = 0; i < symtable_gles2_index; i++) {
        if (symtable_gles2[i].symbol && std::strcmp(symtable_gles2[i].symbol, sym) == 0) {
            symtable_gles2[i].func = fn;
            return;
        }
    }
}

void load_gles2_funcs()
{
	glad_glActiveTexture = (PFNGLACTIVETEXTUREPROC)PTR_RESOLVE(glActiveTexture);
	glad_glAttachShader = (PFNGLATTACHSHADERPROC)PTR_RESOLVE(glAttachShader);
	glad_glBindAttribLocation = (PFNGLBINDATTRIBLOCATIONPROC)PTR_RESOLVE(glBindAttribLocation);
	glad_glBindBuffer = (PFNGLBINDBUFFERPROC)PTR_RESOLVE(glBindBuffer);
	glad_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)PTR_RESOLVE(glBindFramebuffer);
	glad_glBindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)PTR_RESOLVE(glBindRenderbuffer);
	glad_glBindTexture = (PFNGLBINDTEXTUREPROC)PTR_RESOLVE(glBindTexture);
	glad_glBlendColor = (PFNGLBLENDCOLORPROC)PTR_RESOLVE(glBlendColor);
	glad_glBlendEquation = (PFNGLBLENDEQUATIONPROC)PTR_RESOLVE(glBlendEquation);
	glad_glBlendEquationSeparate = (PFNGLBLENDEQUATIONSEPARATEPROC)PTR_RESOLVE(glBlendEquationSeparate);
	glad_glBlendFunc = (PFNGLBLENDFUNCPROC)PTR_RESOLVE(glBlendFunc);
	glad_glBlendFuncSeparate = (PFNGLBLENDFUNCSEPARATEPROC)PTR_RESOLVE(glBlendFuncSeparate);
	glad_glBufferData = (PFNGLBUFFERDATAPROC)PTR_RESOLVE(glBufferData);
	glad_glBufferSubData = (PFNGLBUFFERSUBDATAPROC)PTR_RESOLVE(glBufferSubData);
	glad_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)PTR_RESOLVE(glCheckFramebufferStatus);
	glad_glClear = (PFNGLCLEARPROC)PTR_RESOLVE(glClear);
	glad_glClearColor = (PFNGLCLEARCOLORPROC)PTR_RESOLVE(glClearColor);
	glad_glClearDepthf = (PFNGLCLEARDEPTHFPROC)PTR_RESOLVE(glClearDepthf);
	glad_glClearStencil = (PFNGLCLEARSTENCILPROC)PTR_RESOLVE(glClearStencil);
	glad_glColorMask = (PFNGLCOLORMASKPROC)PTR_RESOLVE(glColorMask);
	glad_glCompileShader = (PFNGLCOMPILESHADERPROC)PTR_RESOLVE(glCompileShader);
	glad_glCompressedTexImage2D = (PFNGLCOMPRESSEDTEXIMAGE2DPROC)PTR_RESOLVE(glCompressedTexImage2D);
	glad_glCompressedTexSubImage2D = (PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC)PTR_RESOLVE(glCompressedTexSubImage2D);
	glad_glCopyTexImage2D = (PFNGLCOPYTEXIMAGE2DPROC)PTR_RESOLVE(glCopyTexImage2D);
	glad_glCopyTexSubImage2D = (PFNGLCOPYTEXSUBIMAGE2DPROC)PTR_RESOLVE(glCopyTexSubImage2D);
	glad_glCreateProgram = (PFNGLCREATEPROGRAMPROC)PTR_RESOLVE(glCreateProgram);
	glad_glCreateShader = (PFNGLCREATESHADERPROC)PTR_RESOLVE(glCreateShader);
	glad_glCullFace = (PFNGLCULLFACEPROC)PTR_RESOLVE(glCullFace);
	glad_glDeleteBuffers = (PFNGLDELETEBUFFERSPROC)PTR_RESOLVE(glDeleteBuffers);
	glad_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)PTR_RESOLVE(glDeleteFramebuffers);
	glad_glDeleteProgram = (PFNGLDELETEPROGRAMPROC)PTR_RESOLVE(glDeleteProgram);
	glad_glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)PTR_RESOLVE(glDeleteRenderbuffers);
	glad_glDeleteShader = (PFNGLDELETESHADERPROC)PTR_RESOLVE(glDeleteShader);
	glad_glDeleteTextures = (PFNGLDELETETEXTURESPROC)PTR_RESOLVE(glDeleteTextures);
	glad_glDepthFunc = (PFNGLDEPTHFUNCPROC)PTR_RESOLVE(glDepthFunc);
	glad_glDepthMask = (PFNGLDEPTHMASKPROC)PTR_RESOLVE(glDepthMask);
	glad_glDepthRangef = (PFNGLDEPTHRANGEFPROC)PTR_RESOLVE(glDepthRangef);
	glad_glDetachShader = (PFNGLDETACHSHADERPROC)PTR_RESOLVE(glDetachShader);
	glad_glDisable = (PFNGLDISABLEPROC)PTR_RESOLVE(glDisable);
	glad_glDisableVertexAttribArray = (PFNGLDISABLEVERTEXATTRIBARRAYPROC)PTR_RESOLVE(glDisableVertexAttribArray);
	glad_glDrawArrays = (PFNGLDRAWARRAYSPROC)PTR_RESOLVE(glDrawArrays);
	glad_glDrawElements = (PFNGLDRAWELEMENTSPROC)PTR_RESOLVE(glDrawElements);
	glad_glEnable = (PFNGLENABLEPROC)PTR_RESOLVE(glEnable);
	glad_glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)PTR_RESOLVE(glEnableVertexAttribArray);
	glad_glFinish = (PFNGLFINISHPROC)PTR_RESOLVE(glFinish);
	glad_glFlush = (PFNGLFLUSHPROC)PTR_RESOLVE(glFlush);
	glad_glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)PTR_RESOLVE(glFramebufferRenderbuffer);
	glad_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)PTR_RESOLVE(glFramebufferTexture2D);
	glad_glFrontFace = (PFNGLFRONTFACEPROC)PTR_RESOLVE(glFrontFace);
	glad_glGenBuffers = (PFNGLGENBUFFERSPROC)PTR_RESOLVE(glGenBuffers);
	glad_glGenerateMipmap = (PFNGLGENERATEMIPMAPPROC)PTR_RESOLVE(glGenerateMipmap);
	glad_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)PTR_RESOLVE(glGenFramebuffers);
	glad_glGenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)PTR_RESOLVE(glGenRenderbuffers);
	glad_glGenTextures = (PFNGLGENTEXTURESPROC)PTR_RESOLVE(glGenTextures);
	glad_glGetActiveAttrib = (PFNGLGETACTIVEATTRIBPROC)PTR_RESOLVE(glGetActiveAttrib);
	glad_glGetActiveUniform = (PFNGLGETACTIVEUNIFORMPROC)PTR_RESOLVE(glGetActiveUniform);
	glad_glGetAttachedShaders = (PFNGLGETATTACHEDSHADERSPROC)PTR_RESOLVE(glGetAttachedShaders);
	glad_glGetAttribLocation = (PFNGLGETATTRIBLOCATIONPROC)PTR_RESOLVE(glGetAttribLocation);
	glad_glGetBooleanv = (PFNGLGETBOOLEANVPROC)PTR_RESOLVE(glGetBooleanv);
	glad_glGetBufferParameteriv = (PFNGLGETBUFFERPARAMETERIVPROC)PTR_RESOLVE(glGetBufferParameteriv);
	glad_glGetError = (PFNGLGETERRORPROC)PTR_RESOLVE(glGetError);
	glad_glGetFloatv = (PFNGLGETFLOATVPROC)PTR_RESOLVE(glGetFloatv);
	glad_glGetFramebufferAttachmentParameteriv = (PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC)PTR_RESOLVE(glGetFramebufferAttachmentParameteriv);
	glad_glGetIntegerv = (PFNGLGETINTEGERVPROC)PTR_RESOLVE(glGetIntegerv);
	glad_glGetProgramiv = (PFNGLGETPROGRAMIVPROC)PTR_RESOLVE(glGetProgramiv);
	glad_glGetProgramInfoLog = (PFNGLGETPROGRAMINFOLOGPROC)PTR_RESOLVE(glGetProgramInfoLog);
	glad_glGetRenderbufferParameteriv = (PFNGLGETRENDERBUFFERPARAMETERIVPROC)PTR_RESOLVE(glGetRenderbufferParameteriv);
	glad_glGetShaderiv = (PFNGLGETSHADERIVPROC)PTR_RESOLVE(glGetShaderiv);
	glad_glGetShaderInfoLog = (PFNGLGETSHADERINFOLOGPROC)PTR_RESOLVE(glGetShaderInfoLog);
	glad_glGetShaderPrecisionFormat = (PFNGLGETSHADERPRECISIONFORMATPROC)PTR_RESOLVE(glGetShaderPrecisionFormat);
	glad_glGetShaderSource = (PFNGLGETSHADERSOURCEPROC)PTR_RESOLVE(glGetShaderSource);
	glad_glGetString = (PFNGLGETSTRINGPROC)PTR_RESOLVE(glGetString);
	glad_glGetTexParameterfv = (PFNGLGETTEXPARAMETERFVPROC)PTR_RESOLVE(glGetTexParameterfv);
	glad_glGetTexParameteriv = (PFNGLGETTEXPARAMETERIVPROC)PTR_RESOLVE(glGetTexParameteriv);
	glad_glGetUniformfv = (PFNGLGETUNIFORMFVPROC)PTR_RESOLVE(glGetUniformfv);
	glad_glGetUniformiv = (PFNGLGETUNIFORMIVPROC)PTR_RESOLVE(glGetUniformiv);
	glad_glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)PTR_RESOLVE(glGetUniformLocation);
	glad_glGetVertexAttribfv = (PFNGLGETVERTEXATTRIBFVPROC)PTR_RESOLVE(glGetVertexAttribfv);
	glad_glGetVertexAttribiv = (PFNGLGETVERTEXATTRIBIVPROC)PTR_RESOLVE(glGetVertexAttribiv);
	glad_glGetVertexAttribPointerv = (PFNGLGETVERTEXATTRIBPOINTERVPROC)PTR_RESOLVE(glGetVertexAttribPointerv);
	glad_glHint = (PFNGLHINTPROC)PTR_RESOLVE(glHint);
	glad_glIsBuffer = (PFNGLISBUFFERPROC)PTR_RESOLVE(glIsBuffer);
	glad_glIsEnabled = (PFNGLISENABLEDPROC)PTR_RESOLVE(glIsEnabled);
	glad_glIsFramebuffer = (PFNGLISFRAMEBUFFERPROC)PTR_RESOLVE(glIsFramebuffer);
	glad_glIsProgram = (PFNGLISPROGRAMPROC)PTR_RESOLVE(glIsProgram);
	glad_glIsRenderbuffer = (PFNGLISRENDERBUFFERPROC)PTR_RESOLVE(glIsRenderbuffer);
	glad_glIsShader = (PFNGLISSHADERPROC)PTR_RESOLVE(glIsShader);
	glad_glIsTexture = (PFNGLISTEXTUREPROC)PTR_RESOLVE(glIsTexture);
	glad_glLineWidth = (PFNGLLINEWIDTHPROC)PTR_RESOLVE(glLineWidth);
	glad_glLinkProgram = (PFNGLLINKPROGRAMPROC)PTR_RESOLVE(glLinkProgram);
	glad_glPixelStorei = (PFNGLPIXELSTOREIPROC)PTR_RESOLVE(glPixelStorei);
	glad_glPolygonOffset = (PFNGLPOLYGONOFFSETPROC)PTR_RESOLVE(glPolygonOffset);
	glad_glReadPixels = (PFNGLREADPIXELSPROC)PTR_RESOLVE(glReadPixels);
	glad_glReleaseShaderCompiler = (PFNGLRELEASESHADERCOMPILERPROC)PTR_RESOLVE(glReleaseShaderCompiler);
	glad_glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)PTR_RESOLVE(glRenderbufferStorage);
	glad_glSampleCoverage = (PFNGLSAMPLECOVERAGEPROC)PTR_RESOLVE(glSampleCoverage);
	glad_glScissor = (PFNGLSCISSORPROC)PTR_RESOLVE(glScissor);
	glad_glShaderBinary = (PFNGLSHADERBINARYPROC)PTR_RESOLVE(glShaderBinary);
	glad_glShaderSource = (PFNGLSHADERSOURCEPROC)PTR_RESOLVE(glShaderSource);
	glad_glStencilFunc = (PFNGLSTENCILFUNCPROC)PTR_RESOLVE(glStencilFunc);
	glad_glStencilFuncSeparate = (PFNGLSTENCILFUNCSEPARATEPROC)PTR_RESOLVE(glStencilFuncSeparate);
	glad_glStencilMask = (PFNGLSTENCILMASKPROC)PTR_RESOLVE(glStencilMask);
	glad_glStencilMaskSeparate = (PFNGLSTENCILMASKSEPARATEPROC)PTR_RESOLVE(glStencilMaskSeparate);
	glad_glStencilOp = (PFNGLSTENCILOPPROC)PTR_RESOLVE(glStencilOp);
	glad_glStencilOpSeparate = (PFNGLSTENCILOPSEPARATEPROC)PTR_RESOLVE(glStencilOpSeparate);
	glad_glTexImage2D = (PFNGLTEXIMAGE2DPROC)PTR_RESOLVE(glTexImage2D);
	glad_glTexParameterf = (PFNGLTEXPARAMETERFPROC)PTR_RESOLVE(glTexParameterf);
	glad_glTexParameterfv = (PFNGLTEXPARAMETERFVPROC)PTR_RESOLVE(glTexParameterfv);
	glad_glTexParameteri = (PFNGLTEXPARAMETERIPROC)PTR_RESOLVE(glTexParameteri);
	glad_glTexParameteriv = (PFNGLTEXPARAMETERIVPROC)PTR_RESOLVE(glTexParameteriv);
	glad_glTexSubImage2D = (PFNGLTEXSUBIMAGE2DPROC)PTR_RESOLVE(glTexSubImage2D);
	glad_glUniform1f = (PFNGLUNIFORM1FPROC)PTR_RESOLVE(glUniform1f);
	glad_glUniform1fv = (PFNGLUNIFORM1FVPROC)PTR_RESOLVE(glUniform1fv);
	glad_glUniform1i = (PFNGLUNIFORM1IPROC)PTR_RESOLVE(glUniform1i);
	glad_glUniform1iv = (PFNGLUNIFORM1IVPROC)PTR_RESOLVE(glUniform1iv);
	glad_glUniform2f = (PFNGLUNIFORM2FPROC)PTR_RESOLVE(glUniform2f);
	glad_glUniform2fv = (PFNGLUNIFORM2FVPROC)PTR_RESOLVE(glUniform2fv);
	glad_glUniform2i = (PFNGLUNIFORM2IPROC)PTR_RESOLVE(glUniform2i);
	glad_glUniform2iv = (PFNGLUNIFORM2IVPROC)PTR_RESOLVE(glUniform2iv);
	glad_glUniform3f = (PFNGLUNIFORM3FPROC)PTR_RESOLVE(glUniform3f);
	glad_glUniform3fv = (PFNGLUNIFORM3FVPROC)PTR_RESOLVE(glUniform3fv);
	glad_glUniform3i = (PFNGLUNIFORM3IPROC)PTR_RESOLVE(glUniform3i);
	glad_glUniform3iv = (PFNGLUNIFORM3IVPROC)PTR_RESOLVE(glUniform3iv);
	glad_glUniform4f = (PFNGLUNIFORM4FPROC)PTR_RESOLVE(glUniform4f);
	glad_glUniform4fv = (PFNGLUNIFORM4FVPROC)PTR_RESOLVE(glUniform4fv);
	glad_glUniform4i = (PFNGLUNIFORM4IPROC)PTR_RESOLVE(glUniform4i);
	glad_glUniform4iv = (PFNGLUNIFORM4IVPROC)PTR_RESOLVE(glUniform4iv);
	glad_glUniformMatrix2fv = (PFNGLUNIFORMMATRIX2FVPROC)PTR_RESOLVE(glUniformMatrix2fv);
	glad_glUniformMatrix3fv = (PFNGLUNIFORMMATRIX3FVPROC)PTR_RESOLVE(glUniformMatrix3fv);
	glad_glUniformMatrix4fv = (PFNGLUNIFORMMATRIX4FVPROC)PTR_RESOLVE(glUniformMatrix4fv);
	glad_glUseProgram = (PFNGLUSEPROGRAMPROC)PTR_RESOLVE(glUseProgram);
	glad_glValidateProgram = (PFNGLVALIDATEPROGRAMPROC)PTR_RESOLVE(glValidateProgram);
	glad_glVertexAttrib1f = (PFNGLVERTEXATTRIB1FPROC)PTR_RESOLVE(glVertexAttrib1f);
	glad_glVertexAttrib1fv = (PFNGLVERTEXATTRIB1FVPROC)PTR_RESOLVE(glVertexAttrib1fv);
	glad_glVertexAttrib2f = (PFNGLVERTEXATTRIB2FPROC)PTR_RESOLVE(glVertexAttrib2f);
	glad_glVertexAttrib2fv = (PFNGLVERTEXATTRIB2FVPROC)PTR_RESOLVE(glVertexAttrib2fv);
	glad_glVertexAttrib3f = (PFNGLVERTEXATTRIB3FPROC)PTR_RESOLVE(glVertexAttrib3f);
	glad_glVertexAttrib3fv = (PFNGLVERTEXATTRIB3FVPROC)PTR_RESOLVE(glVertexAttrib3fv);
	glad_glVertexAttrib4f = (PFNGLVERTEXATTRIB4FPROC)PTR_RESOLVE(glVertexAttrib4f);
	glad_glVertexAttrib4fv = (PFNGLVERTEXATTRIB4FVPROC)PTR_RESOLVE(glVertexAttrib4fv);
	glad_glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)PTR_RESOLVE(glVertexAttribPointer);
	glad_glViewport = (PFNGLVIEWPORTPROC)PTR_RESOLVE(glViewport);
	glad_glReadBuffer = (PFNGLREADBUFFERPROC)PTR_RESOLVE(glReadBuffer);
	glad_glDrawRangeElements = (PFNGLDRAWRANGEELEMENTSPROC)PTR_RESOLVE(glDrawRangeElements);
	glad_glTexImage3D = (PFNGLTEXIMAGE3DPROC)PTR_RESOLVE(glTexImage3D);
	glad_glTexSubImage3D = (PFNGLTEXSUBIMAGE3DPROC)PTR_RESOLVE(glTexSubImage3D);
	glad_glCopyTexSubImage3D = (PFNGLCOPYTEXSUBIMAGE3DPROC)PTR_RESOLVE(glCopyTexSubImage3D);
	glad_glCompressedTexImage3D = (PFNGLCOMPRESSEDTEXIMAGE3DPROC)PTR_RESOLVE(glCompressedTexImage3D);
	glad_glCompressedTexSubImage3D = (PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC)PTR_RESOLVE(glCompressedTexSubImage3D);
	glad_glGenQueries = (PFNGLGENQUERIESPROC)PTR_RESOLVE(glGenQueries);
	glad_glDeleteQueries = (PFNGLDELETEQUERIESPROC)PTR_RESOLVE(glDeleteQueries);
	glad_glIsQuery = (PFNGLISQUERYPROC)PTR_RESOLVE(glIsQuery);
	glad_glBeginQuery = (PFNGLBEGINQUERYPROC)PTR_RESOLVE(glBeginQuery);
	glad_glEndQuery = (PFNGLENDQUERYPROC)PTR_RESOLVE(glEndQuery);
	glad_glGetQueryiv = (PFNGLGETQUERYIVPROC)PTR_RESOLVE(glGetQueryiv);
	glad_glGetQueryObjectuiv = (PFNGLGETQUERYOBJECTUIVPROC)PTR_RESOLVE(glGetQueryObjectuiv);
	glad_glUnmapBuffer = (PFNGLUNMAPBUFFERPROC)PTR_RESOLVE(glUnmapBuffer);
	glad_glGetBufferPointerv = (PFNGLGETBUFFERPOINTERVPROC)PTR_RESOLVE(glGetBufferPointerv);
	glad_glDrawBuffers = (PFNGLDRAWBUFFERSPROC)PTR_RESOLVE(glDrawBuffers);
	glad_glUniformMatrix2x3fv = (PFNGLUNIFORMMATRIX2X3FVPROC)PTR_RESOLVE(glUniformMatrix2x3fv);
	glad_glUniformMatrix3x2fv = (PFNGLUNIFORMMATRIX3X2FVPROC)PTR_RESOLVE(glUniformMatrix3x2fv);
	glad_glUniformMatrix2x4fv = (PFNGLUNIFORMMATRIX2X4FVPROC)PTR_RESOLVE(glUniformMatrix2x4fv);
	glad_glUniformMatrix4x2fv = (PFNGLUNIFORMMATRIX4X2FVPROC)PTR_RESOLVE(glUniformMatrix4x2fv);
	glad_glUniformMatrix3x4fv = (PFNGLUNIFORMMATRIX3X4FVPROC)PTR_RESOLVE(glUniformMatrix3x4fv);
	glad_glUniformMatrix4x3fv = (PFNGLUNIFORMMATRIX4X3FVPROC)PTR_RESOLVE(glUniformMatrix4x3fv);
	glad_glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)PTR_RESOLVE(glBlitFramebuffer);
	glad_glRenderbufferStorageMultisample = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)PTR_RESOLVE(glRenderbufferStorageMultisample);
	glad_glFramebufferTextureLayer = (PFNGLFRAMEBUFFERTEXTURELAYERPROC)PTR_RESOLVE(glFramebufferTextureLayer);
	glad_glMapBufferRange = (PFNGLMAPBUFFERRANGEPROC)PTR_RESOLVE(glMapBufferRange);
	glad_glFlushMappedBufferRange = (PFNGLFLUSHMAPPEDBUFFERRANGEPROC)PTR_RESOLVE(glFlushMappedBufferRange);
	glad_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)PTR_RESOLVE(glBindVertexArray);
	glad_glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)PTR_RESOLVE(glDeleteVertexArrays);
	glad_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)PTR_RESOLVE(glGenVertexArrays);
	glad_glIsVertexArray = (PFNGLISVERTEXARRAYPROC)PTR_RESOLVE(glIsVertexArray);
	glad_glGetIntegeri_v = (PFNGLGETINTEGERI_VPROC)PTR_RESOLVE(glGetIntegeri_v);
	glad_glBeginTransformFeedback = (PFNGLBEGINTRANSFORMFEEDBACKPROC)PTR_RESOLVE(glBeginTransformFeedback);
	glad_glEndTransformFeedback = (PFNGLENDTRANSFORMFEEDBACKPROC)PTR_RESOLVE(glEndTransformFeedback);
	glad_glBindBufferRange = (PFNGLBINDBUFFERRANGEPROC)PTR_RESOLVE(glBindBufferRange);
	glad_glBindBufferBase = (PFNGLBINDBUFFERBASEPROC)PTR_RESOLVE(glBindBufferBase);
	glad_glTransformFeedbackVaryings = (PFNGLTRANSFORMFEEDBACKVARYINGSPROC)PTR_RESOLVE(glTransformFeedbackVaryings);
	glad_glGetTransformFeedbackVarying = (PFNGLGETTRANSFORMFEEDBACKVARYINGPROC)PTR_RESOLVE(glGetTransformFeedbackVarying);
	glad_glVertexAttribIPointer = (PFNGLVERTEXATTRIBIPOINTERPROC)PTR_RESOLVE(glVertexAttribIPointer);
	glad_glGetVertexAttribIiv = (PFNGLGETVERTEXATTRIBIIVPROC)PTR_RESOLVE(glGetVertexAttribIiv);
	glad_glGetVertexAttribIuiv = (PFNGLGETVERTEXATTRIBIUIVPROC)PTR_RESOLVE(glGetVertexAttribIuiv);
	glad_glVertexAttribI4i = (PFNGLVERTEXATTRIBI4IPROC)PTR_RESOLVE(glVertexAttribI4i);
	glad_glVertexAttribI4ui = (PFNGLVERTEXATTRIBI4UIPROC)PTR_RESOLVE(glVertexAttribI4ui);
	glad_glVertexAttribI4iv = (PFNGLVERTEXATTRIBI4IVPROC)PTR_RESOLVE(glVertexAttribI4iv);
	glad_glVertexAttribI4uiv = (PFNGLVERTEXATTRIBI4UIVPROC)PTR_RESOLVE(glVertexAttribI4uiv);
	glad_glGetUniformuiv = (PFNGLGETUNIFORMUIVPROC)PTR_RESOLVE(glGetUniformuiv);
	glad_glGetFragDataLocation = (PFNGLGETFRAGDATALOCATIONPROC)PTR_RESOLVE(glGetFragDataLocation);
	glad_glUniform1ui = (PFNGLUNIFORM1UIPROC)PTR_RESOLVE(glUniform1ui);
	glad_glUniform2ui = (PFNGLUNIFORM2UIPROC)PTR_RESOLVE(glUniform2ui);
	glad_glUniform3ui = (PFNGLUNIFORM3UIPROC)PTR_RESOLVE(glUniform3ui);
	glad_glUniform4ui = (PFNGLUNIFORM4UIPROC)PTR_RESOLVE(glUniform4ui);
	glad_glUniform1uiv = (PFNGLUNIFORM1UIVPROC)PTR_RESOLVE(glUniform1uiv);
	glad_glUniform2uiv = (PFNGLUNIFORM2UIVPROC)PTR_RESOLVE(glUniform2uiv);
	glad_glUniform3uiv = (PFNGLUNIFORM3UIVPROC)PTR_RESOLVE(glUniform3uiv);
	glad_glUniform4uiv = (PFNGLUNIFORM4UIVPROC)PTR_RESOLVE(glUniform4uiv);
	glad_glClearBufferiv = (PFNGLCLEARBUFFERIVPROC)PTR_RESOLVE(glClearBufferiv);
	glad_glClearBufferuiv = (PFNGLCLEARBUFFERUIVPROC)PTR_RESOLVE(glClearBufferuiv);
	glad_glClearBufferfv = (PFNGLCLEARBUFFERFVPROC)PTR_RESOLVE(glClearBufferfv);
	glad_glClearBufferfi = (PFNGLCLEARBUFFERFIPROC)PTR_RESOLVE(glClearBufferfi);
	glad_glGetStringi = (PFNGLGETSTRINGIPROC)PTR_RESOLVE(glGetStringi);
	glad_glCopyBufferSubData = (PFNGLCOPYBUFFERSUBDATAPROC)PTR_RESOLVE(glCopyBufferSubData);
	glad_glGetUniformIndices = (PFNGLGETUNIFORMINDICESPROC)PTR_RESOLVE(glGetUniformIndices);
	glad_glGetActiveUniformsiv = (PFNGLGETACTIVEUNIFORMSIVPROC)PTR_RESOLVE(glGetActiveUniformsiv);
	glad_glGetUniformBlockIndex = (PFNGLGETUNIFORMBLOCKINDEXPROC)PTR_RESOLVE(glGetUniformBlockIndex);
	glad_glGetActiveUniformBlockiv = (PFNGLGETACTIVEUNIFORMBLOCKIVPROC)PTR_RESOLVE(glGetActiveUniformBlockiv);
	glad_glGetActiveUniformBlockName = (PFNGLGETACTIVEUNIFORMBLOCKNAMEPROC)PTR_RESOLVE(glGetActiveUniformBlockName);
	glad_glUniformBlockBinding = (PFNGLUNIFORMBLOCKBINDINGPROC)PTR_RESOLVE(glUniformBlockBinding);
	glad_glDrawArraysInstanced = (PFNGLDRAWARRAYSINSTANCEDPROC)PTR_RESOLVE(glDrawArraysInstanced);
	glad_glDrawElementsInstanced = (PFNGLDRAWELEMENTSINSTANCEDPROC)PTR_RESOLVE(glDrawElementsInstanced);
	glad_glFenceSync = (PFNGLFENCESYNCPROC)PTR_RESOLVE(glFenceSync);
	glad_glIsSync = (PFNGLISSYNCPROC)PTR_RESOLVE(glIsSync);
	glad_glDeleteSync = (PFNGLDELETESYNCPROC)PTR_RESOLVE(glDeleteSync);
	glad_glClientWaitSync = (PFNGLCLIENTWAITSYNCPROC)PTR_RESOLVE(glClientWaitSync);
	glad_glWaitSync = (PFNGLWAITSYNCPROC)PTR_RESOLVE(glWaitSync);
	glad_glGetInteger64v = (PFNGLGETINTEGER64VPROC)PTR_RESOLVE(glGetInteger64v);
	glad_glGetSynciv = (PFNGLGETSYNCIVPROC)PTR_RESOLVE(glGetSynciv);
	glad_glGetInteger64i_v = (PFNGLGETINTEGER64I_VPROC)PTR_RESOLVE(glGetInteger64i_v);
	glad_glGetBufferParameteri64v = (PFNGLGETBUFFERPARAMETERI64VPROC)PTR_RESOLVE(glGetBufferParameteri64v);
	glad_glGenSamplers = (PFNGLGENSAMPLERSPROC)PTR_RESOLVE(glGenSamplers);
	glad_glDeleteSamplers = (PFNGLDELETESAMPLERSPROC)PTR_RESOLVE(glDeleteSamplers);
	glad_glIsSampler = (PFNGLISSAMPLERPROC)PTR_RESOLVE(glIsSampler);
	glad_glBindSampler = (PFNGLBINDSAMPLERPROC)PTR_RESOLVE(glBindSampler);
	glad_glSamplerParameteri = (PFNGLSAMPLERPARAMETERIPROC)PTR_RESOLVE(glSamplerParameteri);
	glad_glSamplerParameteriv = (PFNGLSAMPLERPARAMETERIVPROC)PTR_RESOLVE(glSamplerParameteriv);
	glad_glSamplerParameterf = (PFNGLSAMPLERPARAMETERFPROC)PTR_RESOLVE(glSamplerParameterf);
	glad_glSamplerParameterfv = (PFNGLSAMPLERPARAMETERFVPROC)PTR_RESOLVE(glSamplerParameterfv);
	glad_glGetSamplerParameteriv = (PFNGLGETSAMPLERPARAMETERIVPROC)PTR_RESOLVE(glGetSamplerParameteriv);
	glad_glGetSamplerParameterfv = (PFNGLGETSAMPLERPARAMETERFVPROC)PTR_RESOLVE(glGetSamplerParameterfv);
	glad_glVertexAttribDivisor = (PFNGLVERTEXATTRIBDIVISORPROC)PTR_RESOLVE(glVertexAttribDivisor);
	glad_glBindTransformFeedback = (PFNGLBINDTRANSFORMFEEDBACKPROC)PTR_RESOLVE(glBindTransformFeedback);
	glad_glDeleteTransformFeedbacks = (PFNGLDELETETRANSFORMFEEDBACKSPROC)PTR_RESOLVE(glDeleteTransformFeedbacks);
	glad_glGenTransformFeedbacks = (PFNGLGENTRANSFORMFEEDBACKSPROC)PTR_RESOLVE(glGenTransformFeedbacks);
	glad_glIsTransformFeedback = (PFNGLISTRANSFORMFEEDBACKPROC)PTR_RESOLVE(glIsTransformFeedback);
	glad_glPauseTransformFeedback = (PFNGLPAUSETRANSFORMFEEDBACKPROC)PTR_RESOLVE(glPauseTransformFeedback);
	glad_glResumeTransformFeedback = (PFNGLRESUMETRANSFORMFEEDBACKPROC)PTR_RESOLVE(glResumeTransformFeedback);
	glad_glGetProgramBinary = (PFNGLGETPROGRAMBINARYPROC)PTR_RESOLVE(glGetProgramBinary);
	glad_glProgramBinary = (PFNGLPROGRAMBINARYPROC)PTR_RESOLVE(glProgramBinary);
	glad_glProgramParameteri = (PFNGLPROGRAMPARAMETERIPROC)PTR_RESOLVE(glProgramParameteri);
	glad_glInvalidateFramebuffer = (PFNGLINVALIDATEFRAMEBUFFERPROC)PTR_RESOLVE(glInvalidateFramebuffer);
	glad_glInvalidateSubFramebuffer = (PFNGLINVALIDATESUBFRAMEBUFFERPROC)PTR_RESOLVE(glInvalidateSubFramebuffer);
	glad_glTexStorage2D = (PFNGLTEXSTORAGE2DPROC)PTR_RESOLVE(glTexStorage2D);
	glad_glTexStorage3D = (PFNGLTEXSTORAGE3DPROC)PTR_RESOLVE(glTexStorage3D);
	glad_glGetInternalformativ = (PFNGLGETINTERNALFORMATIVPROC)PTR_RESOLVE(glGetInternalformativ);
	glad_glDispatchCompute = (PFNGLDISPATCHCOMPUTEPROC)PTR_RESOLVE(glDispatchCompute);
	glad_glDispatchComputeIndirect = (PFNGLDISPATCHCOMPUTEINDIRECTPROC)PTR_RESOLVE(glDispatchComputeIndirect);
	glad_glDrawArraysIndirect = (PFNGLDRAWARRAYSINDIRECTPROC)PTR_RESOLVE(glDrawArraysIndirect);
	glad_glDrawElementsIndirect = (PFNGLDRAWELEMENTSINDIRECTPROC)PTR_RESOLVE(glDrawElementsIndirect);
	glad_glFramebufferParameteri = (PFNGLFRAMEBUFFERPARAMETERIPROC)PTR_RESOLVE(glFramebufferParameteri);
	glad_glGetFramebufferParameteriv = (PFNGLGETFRAMEBUFFERPARAMETERIVPROC)PTR_RESOLVE(glGetFramebufferParameteriv);
	glad_glGetProgramInterfaceiv = (PFNGLGETPROGRAMINTERFACEIVPROC)PTR_RESOLVE(glGetProgramInterfaceiv);
	glad_glGetProgramResourceIndex = (PFNGLGETPROGRAMRESOURCEINDEXPROC)PTR_RESOLVE(glGetProgramResourceIndex);
	glad_glGetProgramResourceName = (PFNGLGETPROGRAMRESOURCENAMEPROC)PTR_RESOLVE(glGetProgramResourceName);
	glad_glGetProgramResourceiv = (PFNGLGETPROGRAMRESOURCEIVPROC)PTR_RESOLVE(glGetProgramResourceiv);
	glad_glGetProgramResourceLocation = (PFNGLGETPROGRAMRESOURCELOCATIONPROC)PTR_RESOLVE(glGetProgramResourceLocation);
	glad_glUseProgramStages = (PFNGLUSEPROGRAMSTAGESPROC)PTR_RESOLVE(glUseProgramStages);
	glad_glActiveShaderProgram = (PFNGLACTIVESHADERPROGRAMPROC)PTR_RESOLVE(glActiveShaderProgram);
	glad_glCreateShaderProgramv = (PFNGLCREATESHADERPROGRAMVPROC)PTR_RESOLVE(glCreateShaderProgramv);
	glad_glBindProgramPipeline = (PFNGLBINDPROGRAMPIPELINEPROC)PTR_RESOLVE(glBindProgramPipeline);
	glad_glDeleteProgramPipelines = (PFNGLDELETEPROGRAMPIPELINESPROC)PTR_RESOLVE(glDeleteProgramPipelines);
	glad_glGenProgramPipelines = (PFNGLGENPROGRAMPIPELINESPROC)PTR_RESOLVE(glGenProgramPipelines);
	glad_glIsProgramPipeline = (PFNGLISPROGRAMPIPELINEPROC)PTR_RESOLVE(glIsProgramPipeline);
	glad_glGetProgramPipelineiv = (PFNGLGETPROGRAMPIPELINEIVPROC)PTR_RESOLVE(glGetProgramPipelineiv);
	glad_glProgramUniform1i = (PFNGLPROGRAMUNIFORM1IPROC)PTR_RESOLVE(glProgramUniform1i);
	glad_glProgramUniform2i = (PFNGLPROGRAMUNIFORM2IPROC)PTR_RESOLVE(glProgramUniform2i);
	glad_glProgramUniform3i = (PFNGLPROGRAMUNIFORM3IPROC)PTR_RESOLVE(glProgramUniform3i);
	glad_glProgramUniform4i = (PFNGLPROGRAMUNIFORM4IPROC)PTR_RESOLVE(glProgramUniform4i);
	glad_glProgramUniform1ui = (PFNGLPROGRAMUNIFORM1UIPROC)PTR_RESOLVE(glProgramUniform1ui);
	glad_glProgramUniform2ui = (PFNGLPROGRAMUNIFORM2UIPROC)PTR_RESOLVE(glProgramUniform2ui);
	glad_glProgramUniform3ui = (PFNGLPROGRAMUNIFORM3UIPROC)PTR_RESOLVE(glProgramUniform3ui);
	glad_glProgramUniform4ui = (PFNGLPROGRAMUNIFORM4UIPROC)PTR_RESOLVE(glProgramUniform4ui);
	glad_glProgramUniform1f = (PFNGLPROGRAMUNIFORM1FPROC)PTR_RESOLVE(glProgramUniform1f);
	glad_glProgramUniform2f = (PFNGLPROGRAMUNIFORM2FPROC)PTR_RESOLVE(glProgramUniform2f);
	glad_glProgramUniform3f = (PFNGLPROGRAMUNIFORM3FPROC)PTR_RESOLVE(glProgramUniform3f);
	glad_glProgramUniform4f = (PFNGLPROGRAMUNIFORM4FPROC)PTR_RESOLVE(glProgramUniform4f);
	glad_glProgramUniform1iv = (PFNGLPROGRAMUNIFORM1IVPROC)PTR_RESOLVE(glProgramUniform1iv);
	glad_glProgramUniform2iv = (PFNGLPROGRAMUNIFORM2IVPROC)PTR_RESOLVE(glProgramUniform2iv);
	glad_glProgramUniform3iv = (PFNGLPROGRAMUNIFORM3IVPROC)PTR_RESOLVE(glProgramUniform3iv);
	glad_glProgramUniform4iv = (PFNGLPROGRAMUNIFORM4IVPROC)PTR_RESOLVE(glProgramUniform4iv);
	glad_glProgramUniform1uiv = (PFNGLPROGRAMUNIFORM1UIVPROC)PTR_RESOLVE(glProgramUniform1uiv);
	glad_glProgramUniform2uiv = (PFNGLPROGRAMUNIFORM2UIVPROC)PTR_RESOLVE(glProgramUniform2uiv);
	glad_glProgramUniform3uiv = (PFNGLPROGRAMUNIFORM3UIVPROC)PTR_RESOLVE(glProgramUniform3uiv);
	glad_glProgramUniform4uiv = (PFNGLPROGRAMUNIFORM4UIVPROC)PTR_RESOLVE(glProgramUniform4uiv);
	glad_glProgramUniform1fv = (PFNGLPROGRAMUNIFORM1FVPROC)PTR_RESOLVE(glProgramUniform1fv);
	glad_glProgramUniform2fv = (PFNGLPROGRAMUNIFORM2FVPROC)PTR_RESOLVE(glProgramUniform2fv);
	glad_glProgramUniform3fv = (PFNGLPROGRAMUNIFORM3FVPROC)PTR_RESOLVE(glProgramUniform3fv);
	glad_glProgramUniform4fv = (PFNGLPROGRAMUNIFORM4FVPROC)PTR_RESOLVE(glProgramUniform4fv);
	glad_glProgramUniformMatrix2fv = (PFNGLPROGRAMUNIFORMMATRIX2FVPROC)PTR_RESOLVE(glProgramUniformMatrix2fv);
	glad_glProgramUniformMatrix3fv = (PFNGLPROGRAMUNIFORMMATRIX3FVPROC)PTR_RESOLVE(glProgramUniformMatrix3fv);
	glad_glProgramUniformMatrix4fv = (PFNGLPROGRAMUNIFORMMATRIX4FVPROC)PTR_RESOLVE(glProgramUniformMatrix4fv);
	glad_glProgramUniformMatrix2x3fv = (PFNGLPROGRAMUNIFORMMATRIX2X3FVPROC)PTR_RESOLVE(glProgramUniformMatrix2x3fv);
	glad_glProgramUniformMatrix3x2fv = (PFNGLPROGRAMUNIFORMMATRIX3X2FVPROC)PTR_RESOLVE(glProgramUniformMatrix3x2fv);
	glad_glProgramUniformMatrix2x4fv = (PFNGLPROGRAMUNIFORMMATRIX2X4FVPROC)PTR_RESOLVE(glProgramUniformMatrix2x4fv);
	glad_glProgramUniformMatrix4x2fv = (PFNGLPROGRAMUNIFORMMATRIX4X2FVPROC)PTR_RESOLVE(glProgramUniformMatrix4x2fv);
	glad_glProgramUniformMatrix3x4fv = (PFNGLPROGRAMUNIFORMMATRIX3X4FVPROC)PTR_RESOLVE(glProgramUniformMatrix3x4fv);
	glad_glProgramUniformMatrix4x3fv = (PFNGLPROGRAMUNIFORMMATRIX4X3FVPROC)PTR_RESOLVE(glProgramUniformMatrix4x3fv);
	glad_glValidateProgramPipeline = (PFNGLVALIDATEPROGRAMPIPELINEPROC)PTR_RESOLVE(glValidateProgramPipeline);
	glad_glGetProgramPipelineInfoLog = (PFNGLGETPROGRAMPIPELINEINFOLOGPROC)PTR_RESOLVE(glGetProgramPipelineInfoLog);
	glad_glBindImageTexture = (PFNGLBINDIMAGETEXTUREPROC)PTR_RESOLVE(glBindImageTexture);
	glad_glGetBooleani_v = (PFNGLGETBOOLEANI_VPROC)PTR_RESOLVE(glGetBooleani_v);
	glad_glMemoryBarrier = (PFNGLMEMORYBARRIERPROC)PTR_RESOLVE(glMemoryBarrier);
	glad_glMemoryBarrierByRegion = (PFNGLMEMORYBARRIERBYREGIONPROC)PTR_RESOLVE(glMemoryBarrierByRegion);
	glad_glTexStorage2DMultisample = (PFNGLTEXSTORAGE2DMULTISAMPLEPROC)PTR_RESOLVE(glTexStorage2DMultisample);
	glad_glGetMultisamplefv = (PFNGLGETMULTISAMPLEFVPROC)PTR_RESOLVE(glGetMultisamplefv);
	glad_glSampleMaski = (PFNGLSAMPLEMASKIPROC)PTR_RESOLVE(glSampleMaski);
	glad_glGetTexLevelParameteriv = (PFNGLGETTEXLEVELPARAMETERIVPROC)PTR_RESOLVE(glGetTexLevelParameteriv);
	glad_glGetTexLevelParameterfv = (PFNGLGETTEXLEVELPARAMETERFVPROC)PTR_RESOLVE(glGetTexLevelParameterfv);
	glad_glBindVertexBuffer = (PFNGLBINDVERTEXBUFFERPROC)PTR_RESOLVE(glBindVertexBuffer);
	glad_glVertexAttribFormat = (PFNGLVERTEXATTRIBFORMATPROC)PTR_RESOLVE(glVertexAttribFormat);
	glad_glVertexAttribIFormat = (PFNGLVERTEXATTRIBIFORMATPROC)PTR_RESOLVE(glVertexAttribIFormat);
	glad_glVertexAttribBinding = (PFNGLVERTEXATTRIBBINDINGPROC)PTR_RESOLVE(glVertexAttribBinding);
	glad_glVertexBindingDivisor = (PFNGLVERTEXBINDINGDIVISORPROC)PTR_RESOLVE(glVertexBindingDivisor);
	glad_glBlendBarrier = (PFNGLBLENDBARRIERPROC)PTR_RESOLVE(glBlendBarrier);
	glad_glCopyImageSubData = (PFNGLCOPYIMAGESUBDATAPROC)PTR_RESOLVE(glCopyImageSubData);
	glad_glDebugMessageControl = (PFNGLDEBUGMESSAGECONTROLPROC)PTR_RESOLVE(glDebugMessageControl);
	glad_glDebugMessageInsert = (PFNGLDEBUGMESSAGEINSERTPROC)PTR_RESOLVE(glDebugMessageInsert);
	glad_glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)PTR_RESOLVE(glDebugMessageCallback);
	glad_glGetDebugMessageLog = (PFNGLGETDEBUGMESSAGELOGPROC)PTR_RESOLVE(glGetDebugMessageLog);
	glad_glPushDebugGroup = (PFNGLPUSHDEBUGGROUPPROC)PTR_RESOLVE(glPushDebugGroup);
	glad_glPopDebugGroup = (PFNGLPOPDEBUGGROUPPROC)PTR_RESOLVE(glPopDebugGroup);
	glad_glObjectLabel = (PFNGLOBJECTLABELPROC)PTR_RESOLVE(glObjectLabel);
	glad_glGetObjectLabel = (PFNGLGETOBJECTLABELPROC)PTR_RESOLVE(glGetObjectLabel);
	glad_glObjectPtrLabel = (PFNGLOBJECTPTRLABELPROC)PTR_RESOLVE(glObjectPtrLabel);
	glad_glGetObjectPtrLabel = (PFNGLGETOBJECTPTRLABELPROC)PTR_RESOLVE(glGetObjectPtrLabel);
	glad_glGetPointerv = (PFNGLGETPOINTERVPROC)PTR_RESOLVE(glGetPointerv);
	glad_glEnablei = (PFNGLENABLEIPROC)PTR_RESOLVE(glEnablei);
	glad_glDisablei = (PFNGLDISABLEIPROC)PTR_RESOLVE(glDisablei);
	glad_glBlendEquationi = (PFNGLBLENDEQUATIONIPROC)PTR_RESOLVE(glBlendEquationi);
	glad_glBlendEquationSeparatei = (PFNGLBLENDEQUATIONSEPARATEIPROC)PTR_RESOLVE(glBlendEquationSeparatei);
	glad_glBlendFunci = (PFNGLBLENDFUNCIPROC)PTR_RESOLVE(glBlendFunci);
	glad_glBlendFuncSeparatei = (PFNGLBLENDFUNCSEPARATEIPROC)PTR_RESOLVE(glBlendFuncSeparatei);
	glad_glColorMaski = (PFNGLCOLORMASKIPROC)PTR_RESOLVE(glColorMaski);
	glad_glIsEnabledi = (PFNGLISENABLEDIPROC)PTR_RESOLVE(glIsEnabledi);
	glad_glDrawElementsBaseVertex = (PFNGLDRAWELEMENTSBASEVERTEXPROC)PTR_RESOLVE(glDrawElementsBaseVertex);
	glad_glDrawRangeElementsBaseVertex = (PFNGLDRAWRANGEELEMENTSBASEVERTEXPROC)PTR_RESOLVE(glDrawRangeElementsBaseVertex);
	glad_glDrawElementsInstancedBaseVertex = (PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXPROC)PTR_RESOLVE(glDrawElementsInstancedBaseVertex);
	glad_glFramebufferTexture = (PFNGLFRAMEBUFFERTEXTUREPROC)PTR_RESOLVE(glFramebufferTexture);
	glad_glPrimitiveBoundingBox = (PFNGLPRIMITIVEBOUNDINGBOXPROC)PTR_RESOLVE(glPrimitiveBoundingBox);
	glad_glGetGraphicsResetStatus = (PFNGLGETGRAPHICSRESETSTATUSPROC)PTR_RESOLVE(glGetGraphicsResetStatus);
	glad_glReadnPixels = (PFNGLREADNPIXELSPROC)PTR_RESOLVE(glReadnPixels);
	glad_glGetnUniformfv = (PFNGLGETNUNIFORMFVPROC)PTR_RESOLVE(glGetnUniformfv);
	glad_glGetnUniformiv = (PFNGLGETNUNIFORMIVPROC)PTR_RESOLVE(glGetnUniformiv);
	glad_glGetnUniformuiv = (PFNGLGETNUNIFORMUIVPROC)PTR_RESOLVE(glGetnUniformuiv);
	glad_glMinSampleShading = (PFNGLMINSAMPLESHADINGPROC)PTR_RESOLVE(glMinSampleShading);
	glad_glPatchParameteri = (PFNGLPATCHPARAMETERIPROC)PTR_RESOLVE(glPatchParameteri);
	glad_glTexParameterIiv = (PFNGLTEXPARAMETERIIVPROC)PTR_RESOLVE(glTexParameterIiv);
	glad_glTexParameterIuiv = (PFNGLTEXPARAMETERIUIVPROC)PTR_RESOLVE(glTexParameterIuiv);
	glad_glGetTexParameterIiv = (PFNGLGETTEXPARAMETERIIVPROC)PTR_RESOLVE(glGetTexParameterIiv);
	glad_glGetTexParameterIuiv = (PFNGLGETTEXPARAMETERIUIVPROC)PTR_RESOLVE(glGetTexParameterIuiv);
	glad_glSamplerParameterIiv = (PFNGLSAMPLERPARAMETERIIVPROC)PTR_RESOLVE(glSamplerParameterIiv);
	glad_glSamplerParameterIuiv = (PFNGLSAMPLERPARAMETERIUIVPROC)PTR_RESOLVE(glSamplerParameterIuiv);
	glad_glGetSamplerParameterIiv = (PFNGLGETSAMPLERPARAMETERIIVPROC)PTR_RESOLVE(glGetSamplerParameterIiv);
	glad_glGetSamplerParameterIuiv = (PFNGLGETSAMPLERPARAMETERIUIVPROC)PTR_RESOLVE(glGetSamplerParameterIuiv);
	glad_glTexBuffer = (PFNGLTEXBUFFERPROC)PTR_RESOLVE(glTexBuffer);
	glad_glTexBufferRange = (PFNGLTEXBUFFERRANGEPROC)PTR_RESOLVE(glTexBufferRange);
	glad_glTexStorage3DMultisample = (PFNGLTEXSTORAGE3DMULTISAMPLEPROC)PTR_RESOLVE(glTexStorage3DMultisample);
	glad_glRenderbufferStorageMultisampleAdvancedAMD = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEADVANCEDAMDPROC)PTR_RESOLVE(glRenderbufferStorageMultisampleAdvancedAMD);
	glad_glNamedRenderbufferStorageMultisampleAdvancedAMD = (PFNGLNAMEDRENDERBUFFERSTORAGEMULTISAMPLEADVANCEDAMDPROC)PTR_RESOLVE(glNamedRenderbufferStorageMultisampleAdvancedAMD);
	glad_glGetPerfMonitorGroupsAMD = (PFNGLGETPERFMONITORGROUPSAMDPROC)PTR_RESOLVE(glGetPerfMonitorGroupsAMD);
	glad_glGetPerfMonitorCountersAMD = (PFNGLGETPERFMONITORCOUNTERSAMDPROC)PTR_RESOLVE(glGetPerfMonitorCountersAMD);
	glad_glGetPerfMonitorGroupStringAMD = (PFNGLGETPERFMONITORGROUPSTRINGAMDPROC)PTR_RESOLVE(glGetPerfMonitorGroupStringAMD);
	glad_glGetPerfMonitorCounterStringAMD = (PFNGLGETPERFMONITORCOUNTERSTRINGAMDPROC)PTR_RESOLVE(glGetPerfMonitorCounterStringAMD);
	glad_glGetPerfMonitorCounterInfoAMD = (PFNGLGETPERFMONITORCOUNTERINFOAMDPROC)PTR_RESOLVE(glGetPerfMonitorCounterInfoAMD);
	glad_glGenPerfMonitorsAMD = (PFNGLGENPERFMONITORSAMDPROC)PTR_RESOLVE(glGenPerfMonitorsAMD);
	glad_glDeletePerfMonitorsAMD = (PFNGLDELETEPERFMONITORSAMDPROC)PTR_RESOLVE(glDeletePerfMonitorsAMD);
	glad_glSelectPerfMonitorCountersAMD = (PFNGLSELECTPERFMONITORCOUNTERSAMDPROC)PTR_RESOLVE(glSelectPerfMonitorCountersAMD);
	glad_glBeginPerfMonitorAMD = (PFNGLBEGINPERFMONITORAMDPROC)PTR_RESOLVE(glBeginPerfMonitorAMD);
	glad_glEndPerfMonitorAMD = (PFNGLENDPERFMONITORAMDPROC)PTR_RESOLVE(glEndPerfMonitorAMD);
	glad_glGetPerfMonitorCounterDataAMD = (PFNGLGETPERFMONITORCOUNTERDATAAMDPROC)PTR_RESOLVE(glGetPerfMonitorCounterDataAMD);
	glad_glBlitFramebufferANGLE = (PFNGLBLITFRAMEBUFFERANGLEPROC)PTR_RESOLVE(glBlitFramebufferANGLE);
	glad_glRenderbufferStorageMultisampleANGLE = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEANGLEPROC)PTR_RESOLVE(glRenderbufferStorageMultisampleANGLE);
	glad_glDrawArraysInstancedANGLE = (PFNGLDRAWARRAYSINSTANCEDANGLEPROC)PTR_RESOLVE(glDrawArraysInstancedANGLE);
	glad_glDrawElementsInstancedANGLE = (PFNGLDRAWELEMENTSINSTANCEDANGLEPROC)PTR_RESOLVE(glDrawElementsInstancedANGLE);
	glad_glVertexAttribDivisorANGLE = (PFNGLVERTEXATTRIBDIVISORANGLEPROC)PTR_RESOLVE(glVertexAttribDivisorANGLE);
	glad_glGetTranslatedShaderSourceANGLE = (PFNGLGETTRANSLATEDSHADERSOURCEANGLEPROC)PTR_RESOLVE(glGetTranslatedShaderSourceANGLE);
	glad_glCopyTextureLevelsAPPLE = (PFNGLCOPYTEXTURELEVELSAPPLEPROC)PTR_RESOLVE(glCopyTextureLevelsAPPLE);
	glad_glRenderbufferStorageMultisampleAPPLE = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEAPPLEPROC)PTR_RESOLVE(glRenderbufferStorageMultisampleAPPLE);
	glad_glResolveMultisampleFramebufferAPPLE = (PFNGLRESOLVEMULTISAMPLEFRAMEBUFFERAPPLEPROC)PTR_RESOLVE(glResolveMultisampleFramebufferAPPLE);
	glad_glFenceSyncAPPLE = (PFNGLFENCESYNCAPPLEPROC)PTR_RESOLVE(glFenceSyncAPPLE);
	glad_glIsSyncAPPLE = (PFNGLISSYNCAPPLEPROC)PTR_RESOLVE(glIsSyncAPPLE);
	glad_glDeleteSyncAPPLE = (PFNGLDELETESYNCAPPLEPROC)PTR_RESOLVE(glDeleteSyncAPPLE);
	glad_glClientWaitSyncAPPLE = (PFNGLCLIENTWAITSYNCAPPLEPROC)PTR_RESOLVE(glClientWaitSyncAPPLE);
	glad_glWaitSyncAPPLE = (PFNGLWAITSYNCAPPLEPROC)PTR_RESOLVE(glWaitSyncAPPLE);
	glad_glGetInteger64vAPPLE = (PFNGLGETINTEGER64VAPPLEPROC)PTR_RESOLVE(glGetInteger64vAPPLE);
	glad_glGetSyncivAPPLE = (PFNGLGETSYNCIVAPPLEPROC)PTR_RESOLVE(glGetSyncivAPPLE);
	glad_glMaxActiveShaderCoresARM = (PFNGLMAXACTIVESHADERCORESARMPROC)PTR_RESOLVE(glMaxActiveShaderCoresARM);
	glad_glEGLImageTargetTexStorageEXT = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)PTR_RESOLVE(glEGLImageTargetTexStorageEXT);
	glad_glEGLImageTargetTextureStorageEXT = (PFNGLEGLIMAGETARGETTEXTURESTORAGEEXTPROC)PTR_RESOLVE(glEGLImageTargetTextureStorageEXT);
	glad_glDrawArraysInstancedBaseInstanceEXT = (PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEEXTPROC)PTR_RESOLVE(glDrawArraysInstancedBaseInstanceEXT);
	glad_glDrawElementsInstancedBaseInstanceEXT = (PFNGLDRAWELEMENTSINSTANCEDBASEINSTANCEEXTPROC)PTR_RESOLVE(glDrawElementsInstancedBaseInstanceEXT);
	glad_glDrawElementsInstancedBaseVertexBaseInstanceEXT = (PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEEXTPROC)PTR_RESOLVE(glDrawElementsInstancedBaseVertexBaseInstanceEXT);
	glad_glBindFragDataLocationIndexedEXT = (PFNGLBINDFRAGDATALOCATIONINDEXEDEXTPROC)PTR_RESOLVE(glBindFragDataLocationIndexedEXT);
	glad_glBindFragDataLocationEXT = (PFNGLBINDFRAGDATALOCATIONEXTPROC)PTR_RESOLVE(glBindFragDataLocationEXT);
	glad_glGetProgramResourceLocationIndexEXT = (PFNGLGETPROGRAMRESOURCELOCATIONINDEXEXTPROC)PTR_RESOLVE(glGetProgramResourceLocationIndexEXT);
	glad_glGetFragDataIndexEXT = (PFNGLGETFRAGDATAINDEXEXTPROC)PTR_RESOLVE(glGetFragDataIndexEXT);
	glad_glBlendEquationEXT = (PFNGLBLENDEQUATIONEXTPROC)PTR_RESOLVE(glBlendEquationEXT);
	glad_glBufferStorageEXT = (PFNGLBUFFERSTORAGEEXTPROC)PTR_RESOLVE(glBufferStorageEXT);
	glad_glClearTexImageEXT = (PFNGLCLEARTEXIMAGEEXTPROC)PTR_RESOLVE(glClearTexImageEXT);
	glad_glClearTexSubImageEXT = (PFNGLCLEARTEXSUBIMAGEEXTPROC)PTR_RESOLVE(glClearTexSubImageEXT);
	glad_glClipControlEXT = (PFNGLCLIPCONTROLEXTPROC)PTR_RESOLVE(glClipControlEXT);
	glad_glCopyImageSubDataEXT = (PFNGLCOPYIMAGESUBDATAEXTPROC)PTR_RESOLVE(glCopyImageSubDataEXT);
	glad_glLabelObjectEXT = (PFNGLLABELOBJECTEXTPROC)PTR_RESOLVE(glLabelObjectEXT);
	glad_glGetObjectLabelEXT = (PFNGLGETOBJECTLABELEXTPROC)PTR_RESOLVE(glGetObjectLabelEXT);
	glad_glInsertEventMarkerEXT = (PFNGLINSERTEVENTMARKEREXTPROC)PTR_RESOLVE(glInsertEventMarkerEXT);
	glad_glPushGroupMarkerEXT = (PFNGLPUSHGROUPMARKEREXTPROC)PTR_RESOLVE(glPushGroupMarkerEXT);
	glad_glPopGroupMarkerEXT = (PFNGLPOPGROUPMARKEREXTPROC)PTR_RESOLVE(glPopGroupMarkerEXT);
	glad_glDiscardFramebufferEXT = (PFNGLDISCARDFRAMEBUFFEREXTPROC)PTR_RESOLVE(glDiscardFramebufferEXT);
	glad_glGenQueriesEXT = (PFNGLGENQUERIESEXTPROC)PTR_RESOLVE(glGenQueriesEXT);
	glad_glDeleteQueriesEXT = (PFNGLDELETEQUERIESEXTPROC)PTR_RESOLVE(glDeleteQueriesEXT);
	glad_glIsQueryEXT = (PFNGLISQUERYEXTPROC)PTR_RESOLVE(glIsQueryEXT);
	glad_glBeginQueryEXT = (PFNGLBEGINQUERYEXTPROC)PTR_RESOLVE(glBeginQueryEXT);
	glad_glEndQueryEXT = (PFNGLENDQUERYEXTPROC)PTR_RESOLVE(glEndQueryEXT);
	glad_glQueryCounterEXT = (PFNGLQUERYCOUNTEREXTPROC)PTR_RESOLVE(glQueryCounterEXT);
	glad_glGetQueryivEXT = (PFNGLGETQUERYIVEXTPROC)PTR_RESOLVE(glGetQueryivEXT);
	glad_glGetQueryObjectivEXT = (PFNGLGETQUERYOBJECTIVEXTPROC)PTR_RESOLVE(glGetQueryObjectivEXT);
	glad_glGetQueryObjectuivEXT = (PFNGLGETQUERYOBJECTUIVEXTPROC)PTR_RESOLVE(glGetQueryObjectuivEXT);
	glad_glGetQueryObjecti64vEXT = (PFNGLGETQUERYOBJECTI64VEXTPROC)PTR_RESOLVE(glGetQueryObjecti64vEXT);
	glad_glGetQueryObjectui64vEXT = (PFNGLGETQUERYOBJECTUI64VEXTPROC)PTR_RESOLVE(glGetQueryObjectui64vEXT);
	glad_glGetInteger64vEXT = (PFNGLGETINTEGER64VEXTPROC)PTR_RESOLVE(glGetInteger64vEXT);
	glad_glDrawBuffersEXT = (PFNGLDRAWBUFFERSEXTPROC)PTR_RESOLVE(glDrawBuffersEXT);
	glad_glEnableiEXT = (PFNGLENABLEIEXTPROC)PTR_RESOLVE(glEnableiEXT);
	glad_glDisableiEXT = (PFNGLDISABLEIEXTPROC)PTR_RESOLVE(glDisableiEXT);
	glad_glBlendEquationiEXT = (PFNGLBLENDEQUATIONIEXTPROC)PTR_RESOLVE(glBlendEquationiEXT);
	glad_glBlendEquationSeparateiEXT = (PFNGLBLENDEQUATIONSEPARATEIEXTPROC)PTR_RESOLVE(glBlendEquationSeparateiEXT);
	glad_glBlendFunciEXT = (PFNGLBLENDFUNCIEXTPROC)PTR_RESOLVE(glBlendFunciEXT);
	glad_glBlendFuncSeparateiEXT = (PFNGLBLENDFUNCSEPARATEIEXTPROC)PTR_RESOLVE(glBlendFuncSeparateiEXT);
	glad_glColorMaskiEXT = (PFNGLCOLORMASKIEXTPROC)PTR_RESOLVE(glColorMaskiEXT);
	glad_glIsEnablediEXT = (PFNGLISENABLEDIEXTPROC)PTR_RESOLVE(glIsEnablediEXT);
	glad_glDrawElementsBaseVertexEXT = (PFNGLDRAWELEMENTSBASEVERTEXEXTPROC)PTR_RESOLVE(glDrawElementsBaseVertexEXT);
	glad_glDrawRangeElementsBaseVertexEXT = (PFNGLDRAWRANGEELEMENTSBASEVERTEXEXTPROC)PTR_RESOLVE(glDrawRangeElementsBaseVertexEXT);
	glad_glDrawElementsInstancedBaseVertexEXT = (PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXEXTPROC)PTR_RESOLVE(glDrawElementsInstancedBaseVertexEXT);
	glad_glMultiDrawElementsBaseVertexEXT = (PFNGLMULTIDRAWELEMENTSBASEVERTEXEXTPROC)PTR_RESOLVE(glMultiDrawElementsBaseVertexEXT);
	glad_glDrawArraysInstancedEXT = (PFNGLDRAWARRAYSINSTANCEDEXTPROC)PTR_RESOLVE(glDrawArraysInstancedEXT);
	glad_glDrawElementsInstancedEXT = (PFNGLDRAWELEMENTSINSTANCEDEXTPROC)PTR_RESOLVE(glDrawElementsInstancedEXT);
	glad_glDrawTransformFeedbackEXT = (PFNGLDRAWTRANSFORMFEEDBACKEXTPROC)PTR_RESOLVE(glDrawTransformFeedbackEXT);
	glad_glDrawTransformFeedbackInstancedEXT = (PFNGLDRAWTRANSFORMFEEDBACKINSTANCEDEXTPROC)PTR_RESOLVE(glDrawTransformFeedbackInstancedEXT);
	glad_glBufferStorageExternalEXT = (PFNGLBUFFERSTORAGEEXTERNALEXTPROC)PTR_RESOLVE(glBufferStorageExternalEXT);
	glad_glNamedBufferStorageExternalEXT = (PFNGLNAMEDBUFFERSTORAGEEXTERNALEXTPROC)PTR_RESOLVE(glNamedBufferStorageExternalEXT);
	glad_glGetFragmentShadingRatesEXT = (PFNGLGETFRAGMENTSHADINGRATESEXTPROC)PTR_RESOLVE(glGetFragmentShadingRatesEXT);
	glad_glShadingRateEXT = (PFNGLSHADINGRATEEXTPROC)PTR_RESOLVE(glShadingRateEXT);
	glad_glShadingRateCombinerOpsEXT = (PFNGLSHADINGRATECOMBINEROPSEXTPROC)PTR_RESOLVE(glShadingRateCombinerOpsEXT);
	glad_glFramebufferShadingRateEXT = (PFNGLFRAMEBUFFERSHADINGRATEEXTPROC)PTR_RESOLVE(glFramebufferShadingRateEXT);
	glad_glBlitFramebufferLayersEXT = (PFNGLBLITFRAMEBUFFERLAYERSEXTPROC)PTR_RESOLVE(glBlitFramebufferLayersEXT);
	glad_glBlitFramebufferLayerEXT = (PFNGLBLITFRAMEBUFFERLAYEREXTPROC)PTR_RESOLVE(glBlitFramebufferLayerEXT);
	glad_glFramebufferTextureEXT = (PFNGLFRAMEBUFFERTEXTUREEXTPROC)PTR_RESOLVE(glFramebufferTextureEXT);
	glad_glDrawArraysInstancedEXT = (PFNGLDRAWARRAYSINSTANCEDEXTPROC)PTR_RESOLVE(glDrawArraysInstancedEXT);
	glad_glDrawElementsInstancedEXT = (PFNGLDRAWELEMENTSINSTANCEDEXTPROC)PTR_RESOLVE(glDrawElementsInstancedEXT);
	glad_glVertexAttribDivisorEXT = (PFNGLVERTEXATTRIBDIVISOREXTPROC)PTR_RESOLVE(glVertexAttribDivisorEXT);
	glad_glMapBufferRangeEXT = (PFNGLMAPBUFFERRANGEEXTPROC)PTR_RESOLVE(glMapBufferRangeEXT);
	glad_glFlushMappedBufferRangeEXT = (PFNGLFLUSHMAPPEDBUFFERRANGEEXTPROC)PTR_RESOLVE(glFlushMappedBufferRangeEXT);
	glad_glGetUnsignedBytevEXT = (PFNGLGETUNSIGNEDBYTEVEXTPROC)PTR_RESOLVE(glGetUnsignedBytevEXT);
	glad_glGetUnsignedBytei_vEXT = (PFNGLGETUNSIGNEDBYTEI_VEXTPROC)PTR_RESOLVE(glGetUnsignedBytei_vEXT);
	glad_glDeleteMemoryObjectsEXT = (PFNGLDELETEMEMORYOBJECTSEXTPROC)PTR_RESOLVE(glDeleteMemoryObjectsEXT);
	glad_glIsMemoryObjectEXT = (PFNGLISMEMORYOBJECTEXTPROC)PTR_RESOLVE(glIsMemoryObjectEXT);
	glad_glCreateMemoryObjectsEXT = (PFNGLCREATEMEMORYOBJECTSEXTPROC)PTR_RESOLVE(glCreateMemoryObjectsEXT);
	glad_glMemoryObjectParameterivEXT = (PFNGLMEMORYOBJECTPARAMETERIVEXTPROC)PTR_RESOLVE(glMemoryObjectParameterivEXT);
	glad_glGetMemoryObjectParameterivEXT = (PFNGLGETMEMORYOBJECTPARAMETERIVEXTPROC)PTR_RESOLVE(glGetMemoryObjectParameterivEXT);
	glad_glTexStorageMem2DEXT = (PFNGLTEXSTORAGEMEM2DEXTPROC)PTR_RESOLVE(glTexStorageMem2DEXT);
	glad_glTexStorageMem2DMultisampleEXT = (PFNGLTEXSTORAGEMEM2DMULTISAMPLEEXTPROC)PTR_RESOLVE(glTexStorageMem2DMultisampleEXT);
	glad_glTexStorageMem3DEXT = (PFNGLTEXSTORAGEMEM3DEXTPROC)PTR_RESOLVE(glTexStorageMem3DEXT);
	glad_glTexStorageMem3DMultisampleEXT = (PFNGLTEXSTORAGEMEM3DMULTISAMPLEEXTPROC)PTR_RESOLVE(glTexStorageMem3DMultisampleEXT);
	glad_glBufferStorageMemEXT = (PFNGLBUFFERSTORAGEMEMEXTPROC)PTR_RESOLVE(glBufferStorageMemEXT);
	glad_glTextureStorageMem2DEXT = (PFNGLTEXTURESTORAGEMEM2DEXTPROC)PTR_RESOLVE(glTextureStorageMem2DEXT);
	glad_glTextureStorageMem2DMultisampleEXT = (PFNGLTEXTURESTORAGEMEM2DMULTISAMPLEEXTPROC)PTR_RESOLVE(glTextureStorageMem2DMultisampleEXT);
	glad_glTextureStorageMem3DEXT = (PFNGLTEXTURESTORAGEMEM3DEXTPROC)PTR_RESOLVE(glTextureStorageMem3DEXT);
	glad_glTextureStorageMem3DMultisampleEXT = (PFNGLTEXTURESTORAGEMEM3DMULTISAMPLEEXTPROC)PTR_RESOLVE(glTextureStorageMem3DMultisampleEXT);
	glad_glNamedBufferStorageMemEXT = (PFNGLNAMEDBUFFERSTORAGEMEMEXTPROC)PTR_RESOLVE(glNamedBufferStorageMemEXT);
	glad_glTexStorageMem1DEXT = (PFNGLTEXSTORAGEMEM1DEXTPROC)PTR_RESOLVE(glTexStorageMem1DEXT);
	glad_glTextureStorageMem1DEXT = (PFNGLTEXTURESTORAGEMEM1DEXTPROC)PTR_RESOLVE(glTextureStorageMem1DEXT);
	glad_glImportMemoryFdEXT = (PFNGLIMPORTMEMORYFDEXTPROC)PTR_RESOLVE(glImportMemoryFdEXT);
	glad_glImportMemoryWin32HandleEXT = (PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)PTR_RESOLVE(glImportMemoryWin32HandleEXT);
	glad_glImportMemoryWin32NameEXT = (PFNGLIMPORTMEMORYWIN32NAMEEXTPROC)PTR_RESOLVE(glImportMemoryWin32NameEXT);
	glad_glMultiDrawArraysEXT = (PFNGLMULTIDRAWARRAYSEXTPROC)PTR_RESOLVE(glMultiDrawArraysEXT);
	glad_glMultiDrawElementsEXT = (PFNGLMULTIDRAWELEMENTSEXTPROC)PTR_RESOLVE(glMultiDrawElementsEXT);
	glad_glMultiDrawArraysIndirectEXT = (PFNGLMULTIDRAWARRAYSINDIRECTEXTPROC)PTR_RESOLVE(glMultiDrawArraysIndirectEXT);
	glad_glMultiDrawElementsIndirectEXT = (PFNGLMULTIDRAWELEMENTSINDIRECTEXTPROC)PTR_RESOLVE(glMultiDrawElementsIndirectEXT);
	glad_glRenderbufferStorageMultisampleEXT = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)PTR_RESOLVE(glRenderbufferStorageMultisampleEXT);
	glad_glFramebufferTexture2DMultisampleEXT = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)PTR_RESOLVE(glFramebufferTexture2DMultisampleEXT);
	glad_glReadBufferIndexedEXT = (PFNGLREADBUFFERINDEXEDEXTPROC)PTR_RESOLVE(glReadBufferIndexedEXT);
	glad_glDrawBuffersIndexedEXT = (PFNGLDRAWBUFFERSINDEXEDEXTPROC)PTR_RESOLVE(glDrawBuffersIndexedEXT);
	glad_glGetIntegeri_vEXT = (PFNGLGETINTEGERI_VEXTPROC)PTR_RESOLVE(glGetIntegeri_vEXT);
	glad_glGenQueriesEXT = (PFNGLGENQUERIESEXTPROC)PTR_RESOLVE(glGenQueriesEXT);
	glad_glDeleteQueriesEXT = (PFNGLDELETEQUERIESEXTPROC)PTR_RESOLVE(glDeleteQueriesEXT);
	glad_glIsQueryEXT = (PFNGLISQUERYEXTPROC)PTR_RESOLVE(glIsQueryEXT);
	glad_glBeginQueryEXT = (PFNGLBEGINQUERYEXTPROC)PTR_RESOLVE(glBeginQueryEXT);
	glad_glEndQueryEXT = (PFNGLENDQUERYEXTPROC)PTR_RESOLVE(glEndQueryEXT);
	glad_glGetQueryivEXT = (PFNGLGETQUERYIVEXTPROC)PTR_RESOLVE(glGetQueryivEXT);
	glad_glGetQueryObjectuivEXT = (PFNGLGETQUERYOBJECTUIVEXTPROC)PTR_RESOLVE(glGetQueryObjectuivEXT);
	glad_glPolygonOffsetClampEXT = (PFNGLPOLYGONOFFSETCLAMPEXTPROC)PTR_RESOLVE(glPolygonOffsetClampEXT);
	glad_glPrimitiveBoundingBoxEXT = (PFNGLPRIMITIVEBOUNDINGBOXEXTPROC)PTR_RESOLVE(glPrimitiveBoundingBoxEXT);
	glad_glRasterSamplesEXT = (PFNGLRASTERSAMPLESEXTPROC)PTR_RESOLVE(glRasterSamplesEXT);
	glad_glGetGraphicsResetStatusEXT = (PFNGLGETGRAPHICSRESETSTATUSEXTPROC)PTR_RESOLVE(glGetGraphicsResetStatusEXT);
	glad_glReadnPixelsEXT = (PFNGLREADNPIXELSEXTPROC)PTR_RESOLVE(glReadnPixelsEXT);
	glad_glGetnUniformfvEXT = (PFNGLGETNUNIFORMFVEXTPROC)PTR_RESOLVE(glGetnUniformfvEXT);
	glad_glGetnUniformivEXT = (PFNGLGETNUNIFORMIVEXTPROC)PTR_RESOLVE(glGetnUniformivEXT);
	glad_glGetUnsignedBytevEXT = (PFNGLGETUNSIGNEDBYTEVEXTPROC)PTR_RESOLVE(glGetUnsignedBytevEXT);
	glad_glGetUnsignedBytei_vEXT = (PFNGLGETUNSIGNEDBYTEI_VEXTPROC)PTR_RESOLVE(glGetUnsignedBytei_vEXT);
	glad_glGenSemaphoresEXT = (PFNGLGENSEMAPHORESEXTPROC)PTR_RESOLVE(glGenSemaphoresEXT);
	glad_glDeleteSemaphoresEXT = (PFNGLDELETESEMAPHORESEXTPROC)PTR_RESOLVE(glDeleteSemaphoresEXT);
	glad_glIsSemaphoreEXT = (PFNGLISSEMAPHOREEXTPROC)PTR_RESOLVE(glIsSemaphoreEXT);
	glad_glSemaphoreParameterui64vEXT = (PFNGLSEMAPHOREPARAMETERUI64VEXTPROC)PTR_RESOLVE(glSemaphoreParameterui64vEXT);
	glad_glGetSemaphoreParameterui64vEXT = (PFNGLGETSEMAPHOREPARAMETERUI64VEXTPROC)PTR_RESOLVE(glGetSemaphoreParameterui64vEXT);
	glad_glWaitSemaphoreEXT = (PFNGLWAITSEMAPHOREEXTPROC)PTR_RESOLVE(glWaitSemaphoreEXT);
	glad_glSignalSemaphoreEXT = (PFNGLSIGNALSEMAPHOREEXTPROC)PTR_RESOLVE(glSignalSemaphoreEXT);
	glad_glImportSemaphoreFdEXT = (PFNGLIMPORTSEMAPHOREFDEXTPROC)PTR_RESOLVE(glImportSemaphoreFdEXT);
	glad_glImportSemaphoreWin32HandleEXT = (PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)PTR_RESOLVE(glImportSemaphoreWin32HandleEXT);
	glad_glImportSemaphoreWin32NameEXT = (PFNGLIMPORTSEMAPHOREWIN32NAMEEXTPROC)PTR_RESOLVE(glImportSemaphoreWin32NameEXT);
	glad_glUseShaderProgramEXT = (PFNGLUSESHADERPROGRAMEXTPROC)PTR_RESOLVE(glUseShaderProgramEXT);
	glad_glActiveProgramEXT = (PFNGLACTIVEPROGRAMEXTPROC)PTR_RESOLVE(glActiveProgramEXT);
	glad_glCreateShaderProgramEXT = (PFNGLCREATESHADERPROGRAMEXTPROC)PTR_RESOLVE(glCreateShaderProgramEXT);
	glad_glActiveShaderProgramEXT = (PFNGLACTIVESHADERPROGRAMEXTPROC)PTR_RESOLVE(glActiveShaderProgramEXT);
	glad_glBindProgramPipelineEXT = (PFNGLBINDPROGRAMPIPELINEEXTPROC)PTR_RESOLVE(glBindProgramPipelineEXT);
	glad_glCreateShaderProgramvEXT = (PFNGLCREATESHADERPROGRAMVEXTPROC)PTR_RESOLVE(glCreateShaderProgramvEXT);
	glad_glDeleteProgramPipelinesEXT = (PFNGLDELETEPROGRAMPIPELINESEXTPROC)PTR_RESOLVE(glDeleteProgramPipelinesEXT);
	glad_glGenProgramPipelinesEXT = (PFNGLGENPROGRAMPIPELINESEXTPROC)PTR_RESOLVE(glGenProgramPipelinesEXT);
	glad_glGetProgramPipelineInfoLogEXT = (PFNGLGETPROGRAMPIPELINEINFOLOGEXTPROC)PTR_RESOLVE(glGetProgramPipelineInfoLogEXT);
	glad_glGetProgramPipelineivEXT = (PFNGLGETPROGRAMPIPELINEIVEXTPROC)PTR_RESOLVE(glGetProgramPipelineivEXT);
	glad_glIsProgramPipelineEXT = (PFNGLISPROGRAMPIPELINEEXTPROC)PTR_RESOLVE(glIsProgramPipelineEXT);
	glad_glProgramParameteriEXT = (PFNGLPROGRAMPARAMETERIEXTPROC)PTR_RESOLVE(glProgramParameteriEXT);
	glad_glProgramUniform1fEXT = (PFNGLPROGRAMUNIFORM1FEXTPROC)PTR_RESOLVE(glProgramUniform1fEXT);
	glad_glProgramUniform1fvEXT = (PFNGLPROGRAMUNIFORM1FVEXTPROC)PTR_RESOLVE(glProgramUniform1fvEXT);
	glad_glProgramUniform1iEXT = (PFNGLPROGRAMUNIFORM1IEXTPROC)PTR_RESOLVE(glProgramUniform1iEXT);
	glad_glProgramUniform1ivEXT = (PFNGLPROGRAMUNIFORM1IVEXTPROC)PTR_RESOLVE(glProgramUniform1ivEXT);
	glad_glProgramUniform2fEXT = (PFNGLPROGRAMUNIFORM2FEXTPROC)PTR_RESOLVE(glProgramUniform2fEXT);
	glad_glProgramUniform2fvEXT = (PFNGLPROGRAMUNIFORM2FVEXTPROC)PTR_RESOLVE(glProgramUniform2fvEXT);
	glad_glProgramUniform2iEXT = (PFNGLPROGRAMUNIFORM2IEXTPROC)PTR_RESOLVE(glProgramUniform2iEXT);
	glad_glProgramUniform2ivEXT = (PFNGLPROGRAMUNIFORM2IVEXTPROC)PTR_RESOLVE(glProgramUniform2ivEXT);
	glad_glProgramUniform3fEXT = (PFNGLPROGRAMUNIFORM3FEXTPROC)PTR_RESOLVE(glProgramUniform3fEXT);
	glad_glProgramUniform3fvEXT = (PFNGLPROGRAMUNIFORM3FVEXTPROC)PTR_RESOLVE(glProgramUniform3fvEXT);
	glad_glProgramUniform3iEXT = (PFNGLPROGRAMUNIFORM3IEXTPROC)PTR_RESOLVE(glProgramUniform3iEXT);
	glad_glProgramUniform3ivEXT = (PFNGLPROGRAMUNIFORM3IVEXTPROC)PTR_RESOLVE(glProgramUniform3ivEXT);
	glad_glProgramUniform4fEXT = (PFNGLPROGRAMUNIFORM4FEXTPROC)PTR_RESOLVE(glProgramUniform4fEXT);
	glad_glProgramUniform4fvEXT = (PFNGLPROGRAMUNIFORM4FVEXTPROC)PTR_RESOLVE(glProgramUniform4fvEXT);
	glad_glProgramUniform4iEXT = (PFNGLPROGRAMUNIFORM4IEXTPROC)PTR_RESOLVE(glProgramUniform4iEXT);
	glad_glProgramUniform4ivEXT = (PFNGLPROGRAMUNIFORM4IVEXTPROC)PTR_RESOLVE(glProgramUniform4ivEXT);
	glad_glProgramUniformMatrix2fvEXT = (PFNGLPROGRAMUNIFORMMATRIX2FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix2fvEXT);
	glad_glProgramUniformMatrix3fvEXT = (PFNGLPROGRAMUNIFORMMATRIX3FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix3fvEXT);
	glad_glProgramUniformMatrix4fvEXT = (PFNGLPROGRAMUNIFORMMATRIX4FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix4fvEXT);
	glad_glUseProgramStagesEXT = (PFNGLUSEPROGRAMSTAGESEXTPROC)PTR_RESOLVE(glUseProgramStagesEXT);
	glad_glValidateProgramPipelineEXT = (PFNGLVALIDATEPROGRAMPIPELINEEXTPROC)PTR_RESOLVE(glValidateProgramPipelineEXT);
	glad_glProgramUniform1uiEXT = (PFNGLPROGRAMUNIFORM1UIEXTPROC)PTR_RESOLVE(glProgramUniform1uiEXT);
	glad_glProgramUniform2uiEXT = (PFNGLPROGRAMUNIFORM2UIEXTPROC)PTR_RESOLVE(glProgramUniform2uiEXT);
	glad_glProgramUniform3uiEXT = (PFNGLPROGRAMUNIFORM3UIEXTPROC)PTR_RESOLVE(glProgramUniform3uiEXT);
	glad_glProgramUniform4uiEXT = (PFNGLPROGRAMUNIFORM4UIEXTPROC)PTR_RESOLVE(glProgramUniform4uiEXT);
	glad_glProgramUniform1uivEXT = (PFNGLPROGRAMUNIFORM1UIVEXTPROC)PTR_RESOLVE(glProgramUniform1uivEXT);
	glad_glProgramUniform2uivEXT = (PFNGLPROGRAMUNIFORM2UIVEXTPROC)PTR_RESOLVE(glProgramUniform2uivEXT);
	glad_glProgramUniform3uivEXT = (PFNGLPROGRAMUNIFORM3UIVEXTPROC)PTR_RESOLVE(glProgramUniform3uivEXT);
	glad_glProgramUniform4uivEXT = (PFNGLPROGRAMUNIFORM4UIVEXTPROC)PTR_RESOLVE(glProgramUniform4uivEXT);
	glad_glProgramUniformMatrix2x3fvEXT = (PFNGLPROGRAMUNIFORMMATRIX2X3FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix2x3fvEXT);
	glad_glProgramUniformMatrix3x2fvEXT = (PFNGLPROGRAMUNIFORMMATRIX3X2FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix3x2fvEXT);
	glad_glProgramUniformMatrix2x4fvEXT = (PFNGLPROGRAMUNIFORMMATRIX2X4FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix2x4fvEXT);
	glad_glProgramUniformMatrix4x2fvEXT = (PFNGLPROGRAMUNIFORMMATRIX4X2FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix4x2fvEXT);
	glad_glProgramUniformMatrix3x4fvEXT = (PFNGLPROGRAMUNIFORMMATRIX3X4FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix3x4fvEXT);
	glad_glProgramUniformMatrix4x3fvEXT = (PFNGLPROGRAMUNIFORMMATRIX4X3FVEXTPROC)PTR_RESOLVE(glProgramUniformMatrix4x3fvEXT);
	glad_glFramebufferFetchBarrierEXT = (PFNGLFRAMEBUFFERFETCHBARRIEREXTPROC)PTR_RESOLVE(glFramebufferFetchBarrierEXT);
	glad_glFramebufferPixelLocalStorageSizeEXT = (PFNGLFRAMEBUFFERPIXELLOCALSTORAGESIZEEXTPROC)PTR_RESOLVE(glFramebufferPixelLocalStorageSizeEXT);
	glad_glGetFramebufferPixelLocalStorageSizeEXT = (PFNGLGETFRAMEBUFFERPIXELLOCALSTORAGESIZEEXTPROC)PTR_RESOLVE(glGetFramebufferPixelLocalStorageSizeEXT);
	glad_glClearPixelLocalStorageuiEXT = (PFNGLCLEARPIXELLOCALSTORAGEUIEXTPROC)PTR_RESOLVE(glClearPixelLocalStorageuiEXT);
	glad_glTexPageCommitmentEXT = (PFNGLTEXPAGECOMMITMENTEXTPROC)PTR_RESOLVE(glTexPageCommitmentEXT);
	glad_glPatchParameteriEXT = (PFNGLPATCHPARAMETERIEXTPROC)PTR_RESOLVE(glPatchParameteriEXT);
	glad_glTexParameterIivEXT = (PFNGLTEXPARAMETERIIVEXTPROC)PTR_RESOLVE(glTexParameterIivEXT);
	glad_glTexParameterIuivEXT = (PFNGLTEXPARAMETERIUIVEXTPROC)PTR_RESOLVE(glTexParameterIuivEXT);
	glad_glGetTexParameterIivEXT = (PFNGLGETTEXPARAMETERIIVEXTPROC)PTR_RESOLVE(glGetTexParameterIivEXT);
	glad_glGetTexParameterIuivEXT = (PFNGLGETTEXPARAMETERIUIVEXTPROC)PTR_RESOLVE(glGetTexParameterIuivEXT);
	glad_glSamplerParameterIivEXT = (PFNGLSAMPLERPARAMETERIIVEXTPROC)PTR_RESOLVE(glSamplerParameterIivEXT);
	glad_glSamplerParameterIuivEXT = (PFNGLSAMPLERPARAMETERIUIVEXTPROC)PTR_RESOLVE(glSamplerParameterIuivEXT);
	glad_glGetSamplerParameterIivEXT = (PFNGLGETSAMPLERPARAMETERIIVEXTPROC)PTR_RESOLVE(glGetSamplerParameterIivEXT);
	glad_glGetSamplerParameterIuivEXT = (PFNGLGETSAMPLERPARAMETERIUIVEXTPROC)PTR_RESOLVE(glGetSamplerParameterIuivEXT);
	glad_glTexBufferEXT = (PFNGLTEXBUFFEREXTPROC)PTR_RESOLVE(glTexBufferEXT);
	glad_glTexBufferRangeEXT = (PFNGLTEXBUFFERRANGEEXTPROC)PTR_RESOLVE(glTexBufferRangeEXT);
	glad_glTexStorage1DEXT = (PFNGLTEXSTORAGE1DEXTPROC)PTR_RESOLVE(glTexStorage1DEXT);
	glad_glTexStorage2DEXT = (PFNGLTEXSTORAGE2DEXTPROC)PTR_RESOLVE(glTexStorage2DEXT);
	glad_glTexStorage3DEXT = (PFNGLTEXSTORAGE3DEXTPROC)PTR_RESOLVE(glTexStorage3DEXT);
	glad_glTextureStorage1DEXT = (PFNGLTEXTURESTORAGE1DEXTPROC)PTR_RESOLVE(glTextureStorage1DEXT);
	glad_glTextureStorage2DEXT = (PFNGLTEXTURESTORAGE2DEXTPROC)PTR_RESOLVE(glTextureStorage2DEXT);
	glad_glTextureStorage3DEXT = (PFNGLTEXTURESTORAGE3DEXTPROC)PTR_RESOLVE(glTextureStorage3DEXT);
	glad_glTexStorageAttribs2DEXT = (PFNGLTEXSTORAGEATTRIBS2DEXTPROC)PTR_RESOLVE(glTexStorageAttribs2DEXT);
	glad_glTexStorageAttribs3DEXT = (PFNGLTEXSTORAGEATTRIBS3DEXTPROC)PTR_RESOLVE(glTexStorageAttribs3DEXT);
	glad_glTextureViewEXT = (PFNGLTEXTUREVIEWEXTPROC)PTR_RESOLVE(glTextureViewEXT);
	glad_glAcquireKeyedMutexWin32EXT = (PFNGLACQUIREKEYEDMUTEXWIN32EXTPROC)PTR_RESOLVE(glAcquireKeyedMutexWin32EXT);
	glad_glReleaseKeyedMutexWin32EXT = (PFNGLRELEASEKEYEDMUTEXWIN32EXTPROC)PTR_RESOLVE(glReleaseKeyedMutexWin32EXT);
	glad_glWindowRectanglesEXT = (PFNGLWINDOWRECTANGLESEXTPROC)PTR_RESOLVE(glWindowRectanglesEXT);
	glad_glGetTextureHandleIMG = (PFNGLGETTEXTUREHANDLEIMGPROC)PTR_RESOLVE(glGetTextureHandleIMG);
	glad_glGetTextureSamplerHandleIMG = (PFNGLGETTEXTURESAMPLERHANDLEIMGPROC)PTR_RESOLVE(glGetTextureSamplerHandleIMG);
	glad_glUniformHandleui64IMG = (PFNGLUNIFORMHANDLEUI64IMGPROC)PTR_RESOLVE(glUniformHandleui64IMG);
	glad_glUniformHandleui64vIMG = (PFNGLUNIFORMHANDLEUI64VIMGPROC)PTR_RESOLVE(glUniformHandleui64vIMG);
	glad_glProgramUniformHandleui64IMG = (PFNGLPROGRAMUNIFORMHANDLEUI64IMGPROC)PTR_RESOLVE(glProgramUniformHandleui64IMG);
	glad_glProgramUniformHandleui64vIMG = (PFNGLPROGRAMUNIFORMHANDLEUI64VIMGPROC)PTR_RESOLVE(glProgramUniformHandleui64vIMG);
	glad_glFramebufferTexture2DDownsampleIMG = (PFNGLFRAMEBUFFERTEXTURE2DDOWNSAMPLEIMGPROC)PTR_RESOLVE(glFramebufferTexture2DDownsampleIMG);
	glad_glFramebufferTextureLayerDownsampleIMG = (PFNGLFRAMEBUFFERTEXTURELAYERDOWNSAMPLEIMGPROC)PTR_RESOLVE(glFramebufferTextureLayerDownsampleIMG);
	glad_glRenderbufferStorageMultisampleIMG = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEIMGPROC)PTR_RESOLVE(glRenderbufferStorageMultisampleIMG);
	glad_glFramebufferTexture2DMultisampleIMG = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEIMGPROC)PTR_RESOLVE(glFramebufferTexture2DMultisampleIMG);
	glad_glApplyFramebufferAttachmentCMAAINTEL = (PFNGLAPPLYFRAMEBUFFERATTACHMENTCMAAINTELPROC)PTR_RESOLVE(glApplyFramebufferAttachmentCMAAINTEL);
	glad_glBeginPerfQueryINTEL = (PFNGLBEGINPERFQUERYINTELPROC)PTR_RESOLVE(glBeginPerfQueryINTEL);
	glad_glCreatePerfQueryINTEL = (PFNGLCREATEPERFQUERYINTELPROC)PTR_RESOLVE(glCreatePerfQueryINTEL);
	glad_glDeletePerfQueryINTEL = (PFNGLDELETEPERFQUERYINTELPROC)PTR_RESOLVE(glDeletePerfQueryINTEL);
	glad_glEndPerfQueryINTEL = (PFNGLENDPERFQUERYINTELPROC)PTR_RESOLVE(glEndPerfQueryINTEL);
	glad_glGetFirstPerfQueryIdINTEL = (PFNGLGETFIRSTPERFQUERYIDINTELPROC)PTR_RESOLVE(glGetFirstPerfQueryIdINTEL);
	glad_glGetNextPerfQueryIdINTEL = (PFNGLGETNEXTPERFQUERYIDINTELPROC)PTR_RESOLVE(glGetNextPerfQueryIdINTEL);
	glad_glGetPerfCounterInfoINTEL = (PFNGLGETPERFCOUNTERINFOINTELPROC)PTR_RESOLVE(glGetPerfCounterInfoINTEL);
	glad_glGetPerfQueryDataINTEL = (PFNGLGETPERFQUERYDATAINTELPROC)PTR_RESOLVE(glGetPerfQueryDataINTEL);
	glad_glGetPerfQueryIdByNameINTEL = (PFNGLGETPERFQUERYIDBYNAMEINTELPROC)PTR_RESOLVE(glGetPerfQueryIdByNameINTEL);
	glad_glGetPerfQueryInfoINTEL = (PFNGLGETPERFQUERYINFOINTELPROC)PTR_RESOLVE(glGetPerfQueryInfoINTEL);
	glad_glBlendBarrierKHR = (PFNGLBLENDBARRIERKHRPROC)PTR_RESOLVE(glBlendBarrierKHR);
	glad_glDebugMessageControl = (PFNGLDEBUGMESSAGECONTROLPROC)PTR_RESOLVE(glDebugMessageControl);
	glad_glDebugMessageInsert = (PFNGLDEBUGMESSAGEINSERTPROC)PTR_RESOLVE(glDebugMessageInsert);
	glad_glDebugMessageCallback = (PFNGLDEBUGMESSAGECALLBACKPROC)PTR_RESOLVE(glDebugMessageCallback);
	glad_glGetDebugMessageLog = (PFNGLGETDEBUGMESSAGELOGPROC)PTR_RESOLVE(glGetDebugMessageLog);
	glad_glPushDebugGroup = (PFNGLPUSHDEBUGGROUPPROC)PTR_RESOLVE(glPushDebugGroup);
	glad_glPopDebugGroup = (PFNGLPOPDEBUGGROUPPROC)PTR_RESOLVE(glPopDebugGroup);
	glad_glObjectLabel = (PFNGLOBJECTLABELPROC)PTR_RESOLVE(glObjectLabel);
	glad_glGetObjectLabel = (PFNGLGETOBJECTLABELPROC)PTR_RESOLVE(glGetObjectLabel);
	glad_glObjectPtrLabel = (PFNGLOBJECTPTRLABELPROC)PTR_RESOLVE(glObjectPtrLabel);
	glad_glGetObjectPtrLabel = (PFNGLGETOBJECTPTRLABELPROC)PTR_RESOLVE(glGetObjectPtrLabel);
	glad_glGetPointerv = (PFNGLGETPOINTERVPROC)PTR_RESOLVE(glGetPointerv);
	glad_glDebugMessageControlKHR = (PFNGLDEBUGMESSAGECONTROLKHRPROC)PTR_RESOLVE(glDebugMessageControlKHR);
	glad_glDebugMessageInsertKHR = (PFNGLDEBUGMESSAGEINSERTKHRPROC)PTR_RESOLVE(glDebugMessageInsertKHR);
	glad_glDebugMessageCallbackKHR = (PFNGLDEBUGMESSAGECALLBACKKHRPROC)PTR_RESOLVE(glDebugMessageCallbackKHR);
	glad_glGetDebugMessageLogKHR = (PFNGLGETDEBUGMESSAGELOGKHRPROC)PTR_RESOLVE(glGetDebugMessageLogKHR);
	glad_glPushDebugGroupKHR = (PFNGLPUSHDEBUGGROUPKHRPROC)PTR_RESOLVE(glPushDebugGroupKHR);
	glad_glPopDebugGroupKHR = (PFNGLPOPDEBUGGROUPKHRPROC)PTR_RESOLVE(glPopDebugGroupKHR);
	glad_glObjectLabelKHR = (PFNGLOBJECTLABELKHRPROC)PTR_RESOLVE(glObjectLabelKHR);
	glad_glGetObjectLabelKHR = (PFNGLGETOBJECTLABELKHRPROC)PTR_RESOLVE(glGetObjectLabelKHR);
	glad_glObjectPtrLabelKHR = (PFNGLOBJECTPTRLABELKHRPROC)PTR_RESOLVE(glObjectPtrLabelKHR);
	glad_glGetObjectPtrLabelKHR = (PFNGLGETOBJECTPTRLABELKHRPROC)PTR_RESOLVE(glGetObjectPtrLabelKHR);
	glad_glGetPointervKHR = (PFNGLGETPOINTERVKHRPROC)PTR_RESOLVE(glGetPointervKHR);
	glad_glMaxShaderCompilerThreadsKHR = (PFNGLMAXSHADERCOMPILERTHREADSKHRPROC)PTR_RESOLVE(glMaxShaderCompilerThreadsKHR);
	glad_glGetGraphicsResetStatus = (PFNGLGETGRAPHICSRESETSTATUSPROC)PTR_RESOLVE(glGetGraphicsResetStatus);
	glad_glReadnPixels = (PFNGLREADNPIXELSPROC)PTR_RESOLVE(glReadnPixels);
	glad_glGetnUniformfv = (PFNGLGETNUNIFORMFVPROC)PTR_RESOLVE(glGetnUniformfv);
	glad_glGetnUniformiv = (PFNGLGETNUNIFORMIVPROC)PTR_RESOLVE(glGetnUniformiv);
	glad_glGetnUniformuiv = (PFNGLGETNUNIFORMUIVPROC)PTR_RESOLVE(glGetnUniformuiv);
	glad_glGetGraphicsResetStatusKHR = (PFNGLGETGRAPHICSRESETSTATUSKHRPROC)PTR_RESOLVE(glGetGraphicsResetStatusKHR);
	glad_glReadnPixelsKHR = (PFNGLREADNPIXELSKHRPROC)PTR_RESOLVE(glReadnPixelsKHR);
	glad_glGetnUniformfvKHR = (PFNGLGETNUNIFORMFVKHRPROC)PTR_RESOLVE(glGetnUniformfvKHR);
	glad_glGetnUniformivKHR = (PFNGLGETNUNIFORMIVKHRPROC)PTR_RESOLVE(glGetnUniformivKHR);
	glad_glGetnUniformuivKHR = (PFNGLGETNUNIFORMUIVKHRPROC)PTR_RESOLVE(glGetnUniformuivKHR);
	glad_glFramebufferParameteriMESA = (PFNGLFRAMEBUFFERPARAMETERIMESAPROC)PTR_RESOLVE(glFramebufferParameteriMESA);
	glad_glGetFramebufferParameterivMESA = (PFNGLGETFRAMEBUFFERPARAMETERIVMESAPROC)PTR_RESOLVE(glGetFramebufferParameterivMESA);
	glad_glGenSamplers = (PFNGLGENSAMPLERSPROC)PTR_RESOLVE(glGenSamplers);
	glad_glDeleteSamplers = (PFNGLDELETESAMPLERSPROC)PTR_RESOLVE(glDeleteSamplers);
	glad_glIsSampler = (PFNGLISSAMPLERPROC)PTR_RESOLVE(glIsSampler);
	glad_glBindSampler = (PFNGLBINDSAMPLERPROC)PTR_RESOLVE(glBindSampler);
	glad_glSamplerParameteri = (PFNGLSAMPLERPARAMETERIPROC)PTR_RESOLVE(glSamplerParameteri);
	glad_glSamplerParameteriv = (PFNGLSAMPLERPARAMETERIVPROC)PTR_RESOLVE(glSamplerParameteriv);
	glad_glSamplerParameterf = (PFNGLSAMPLERPARAMETERFPROC)PTR_RESOLVE(glSamplerParameterf);
	glad_glSamplerParameterfv = (PFNGLSAMPLERPARAMETERFVPROC)PTR_RESOLVE(glSamplerParameterfv);
	glad_glGetSamplerParameteriv = (PFNGLGETSAMPLERPARAMETERIVPROC)PTR_RESOLVE(glGetSamplerParameteriv);
	glad_glGetSamplerParameterfv = (PFNGLGETSAMPLERPARAMETERFVPROC)PTR_RESOLVE(glGetSamplerParameterfv);
	glad_glGetTextureHandleNV = (PFNGLGETTEXTUREHANDLENVPROC)PTR_RESOLVE(glGetTextureHandleNV);
	glad_glGetTextureSamplerHandleNV = (PFNGLGETTEXTURESAMPLERHANDLENVPROC)PTR_RESOLVE(glGetTextureSamplerHandleNV);
	glad_glMakeTextureHandleResidentNV = (PFNGLMAKETEXTUREHANDLERESIDENTNVPROC)PTR_RESOLVE(glMakeTextureHandleResidentNV);
	glad_glMakeTextureHandleNonResidentNV = (PFNGLMAKETEXTUREHANDLENONRESIDENTNVPROC)PTR_RESOLVE(glMakeTextureHandleNonResidentNV);
	glad_glGetImageHandleNV = (PFNGLGETIMAGEHANDLENVPROC)PTR_RESOLVE(glGetImageHandleNV);
	glad_glMakeImageHandleResidentNV = (PFNGLMAKEIMAGEHANDLERESIDENTNVPROC)PTR_RESOLVE(glMakeImageHandleResidentNV);
	glad_glMakeImageHandleNonResidentNV = (PFNGLMAKEIMAGEHANDLENONRESIDENTNVPROC)PTR_RESOLVE(glMakeImageHandleNonResidentNV);
	glad_glUniformHandleui64NV = (PFNGLUNIFORMHANDLEUI64NVPROC)PTR_RESOLVE(glUniformHandleui64NV);
	glad_glUniformHandleui64vNV = (PFNGLUNIFORMHANDLEUI64VNVPROC)PTR_RESOLVE(glUniformHandleui64vNV);
	glad_glProgramUniformHandleui64NV = (PFNGLPROGRAMUNIFORMHANDLEUI64NVPROC)PTR_RESOLVE(glProgramUniformHandleui64NV);
	glad_glProgramUniformHandleui64vNV = (PFNGLPROGRAMUNIFORMHANDLEUI64VNVPROC)PTR_RESOLVE(glProgramUniformHandleui64vNV);
	glad_glIsTextureHandleResidentNV = (PFNGLISTEXTUREHANDLERESIDENTNVPROC)PTR_RESOLVE(glIsTextureHandleResidentNV);
	glad_glIsImageHandleResidentNV = (PFNGLISIMAGEHANDLERESIDENTNVPROC)PTR_RESOLVE(glIsImageHandleResidentNV);
	glad_glBlendParameteriNV = (PFNGLBLENDPARAMETERINVPROC)PTR_RESOLVE(glBlendParameteriNV);
	glad_glBlendBarrierNV = (PFNGLBLENDBARRIERNVPROC)PTR_RESOLVE(glBlendBarrierNV);
	glad_glViewportPositionWScaleNV = (PFNGLVIEWPORTPOSITIONWSCALENVPROC)PTR_RESOLVE(glViewportPositionWScaleNV);
	glad_glBeginConditionalRenderNV = (PFNGLBEGINCONDITIONALRENDERNVPROC)PTR_RESOLVE(glBeginConditionalRenderNV);
	glad_glEndConditionalRenderNV = (PFNGLENDCONDITIONALRENDERNVPROC)PTR_RESOLVE(glEndConditionalRenderNV);
	glad_glSubpixelPrecisionBiasNV = (PFNGLSUBPIXELPRECISIONBIASNVPROC)PTR_RESOLVE(glSubpixelPrecisionBiasNV);
	glad_glConservativeRasterParameteriNV = (PFNGLCONSERVATIVERASTERPARAMETERINVPROC)PTR_RESOLVE(glConservativeRasterParameteriNV);
	glad_glCopyBufferSubDataNV = (PFNGLCOPYBUFFERSUBDATANVPROC)PTR_RESOLVE(glCopyBufferSubDataNV);
	glad_glCoverageMaskNV = (PFNGLCOVERAGEMASKNVPROC)PTR_RESOLVE(glCoverageMaskNV);
	glad_glCoverageOperationNV = (PFNGLCOVERAGEOPERATIONNVPROC)PTR_RESOLVE(glCoverageOperationNV);
	glad_glDrawBuffersNV = (PFNGLDRAWBUFFERSNVPROC)PTR_RESOLVE(glDrawBuffersNV);
	glad_glDrawArraysInstancedNV = (PFNGLDRAWARRAYSINSTANCEDNVPROC)PTR_RESOLVE(glDrawArraysInstancedNV);
	glad_glDrawElementsInstancedNV = (PFNGLDRAWELEMENTSINSTANCEDNVPROC)PTR_RESOLVE(glDrawElementsInstancedNV);
	glad_glDrawVkImageNV = (PFNGLDRAWVKIMAGENVPROC)PTR_RESOLVE(glDrawVkImageNV);
	glad_glGetVkProcAddrNV = (PFNGLGETVKPROCADDRNVPROC)PTR_RESOLVE(glGetVkProcAddrNV);
	glad_glWaitVkSemaphoreNV = (PFNGLWAITVKSEMAPHORENVPROC)PTR_RESOLVE(glWaitVkSemaphoreNV);
	glad_glSignalVkSemaphoreNV = (PFNGLSIGNALVKSEMAPHORENVPROC)PTR_RESOLVE(glSignalVkSemaphoreNV);
	glad_glSignalVkFenceNV = (PFNGLSIGNALVKFENCENVPROC)PTR_RESOLVE(glSignalVkFenceNV);
	glad_glDeleteFencesNV = (PFNGLDELETEFENCESNVPROC)PTR_RESOLVE(glDeleteFencesNV);
	glad_glGenFencesNV = (PFNGLGENFENCESNVPROC)PTR_RESOLVE(glGenFencesNV);
	glad_glIsFenceNV = (PFNGLISFENCENVPROC)PTR_RESOLVE(glIsFenceNV);
	glad_glTestFenceNV = (PFNGLTESTFENCENVPROC)PTR_RESOLVE(glTestFenceNV);
	glad_glGetFenceivNV = (PFNGLGETFENCEIVNVPROC)PTR_RESOLVE(glGetFenceivNV);
	glad_glFinishFenceNV = (PFNGLFINISHFENCENVPROC)PTR_RESOLVE(glFinishFenceNV);
	glad_glSetFenceNV = (PFNGLSETFENCENVPROC)PTR_RESOLVE(glSetFenceNV);
	glad_glFragmentCoverageColorNV = (PFNGLFRAGMENTCOVERAGECOLORNVPROC)PTR_RESOLVE(glFragmentCoverageColorNV);
	glad_glBlitFramebufferNV = (PFNGLBLITFRAMEBUFFERNVPROC)PTR_RESOLVE(glBlitFramebufferNV);
	glad_glRasterSamplesEXT = (PFNGLRASTERSAMPLESEXTPROC)PTR_RESOLVE(glRasterSamplesEXT);
	glad_glCoverageModulationTableNV = (PFNGLCOVERAGEMODULATIONTABLENVPROC)PTR_RESOLVE(glCoverageModulationTableNV);
	glad_glGetCoverageModulationTableNV = (PFNGLGETCOVERAGEMODULATIONTABLENVPROC)PTR_RESOLVE(glGetCoverageModulationTableNV);
	glad_glCoverageModulationNV = (PFNGLCOVERAGEMODULATIONNVPROC)PTR_RESOLVE(glCoverageModulationNV);
	glad_glRenderbufferStorageMultisampleNV = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLENVPROC)PTR_RESOLVE(glRenderbufferStorageMultisampleNV);
	glad_glUniform1i64NV = (PFNGLUNIFORM1I64NVPROC)PTR_RESOLVE(glUniform1i64NV);
	glad_glUniform2i64NV = (PFNGLUNIFORM2I64NVPROC)PTR_RESOLVE(glUniform2i64NV);
	glad_glUniform3i64NV = (PFNGLUNIFORM3I64NVPROC)PTR_RESOLVE(glUniform3i64NV);
	glad_glUniform4i64NV = (PFNGLUNIFORM4I64NVPROC)PTR_RESOLVE(glUniform4i64NV);
	glad_glUniform1i64vNV = (PFNGLUNIFORM1I64VNVPROC)PTR_RESOLVE(glUniform1i64vNV);
	glad_glUniform2i64vNV = (PFNGLUNIFORM2I64VNVPROC)PTR_RESOLVE(glUniform2i64vNV);
	glad_glUniform3i64vNV = (PFNGLUNIFORM3I64VNVPROC)PTR_RESOLVE(glUniform3i64vNV);
	glad_glUniform4i64vNV = (PFNGLUNIFORM4I64VNVPROC)PTR_RESOLVE(glUniform4i64vNV);
	glad_glUniform1ui64NV = (PFNGLUNIFORM1UI64NVPROC)PTR_RESOLVE(glUniform1ui64NV);
	glad_glUniform2ui64NV = (PFNGLUNIFORM2UI64NVPROC)PTR_RESOLVE(glUniform2ui64NV);
	glad_glUniform3ui64NV = (PFNGLUNIFORM3UI64NVPROC)PTR_RESOLVE(glUniform3ui64NV);
	glad_glUniform4ui64NV = (PFNGLUNIFORM4UI64NVPROC)PTR_RESOLVE(glUniform4ui64NV);
	glad_glUniform1ui64vNV = (PFNGLUNIFORM1UI64VNVPROC)PTR_RESOLVE(glUniform1ui64vNV);
	glad_glUniform2ui64vNV = (PFNGLUNIFORM2UI64VNVPROC)PTR_RESOLVE(glUniform2ui64vNV);
	glad_glUniform3ui64vNV = (PFNGLUNIFORM3UI64VNVPROC)PTR_RESOLVE(glUniform3ui64vNV);
	glad_glUniform4ui64vNV = (PFNGLUNIFORM4UI64VNVPROC)PTR_RESOLVE(glUniform4ui64vNV);
	glad_glGetUniformi64vNV = (PFNGLGETUNIFORMI64VNVPROC)PTR_RESOLVE(glGetUniformi64vNV);
	glad_glProgramUniform1i64NV = (PFNGLPROGRAMUNIFORM1I64NVPROC)PTR_RESOLVE(glProgramUniform1i64NV);
	glad_glProgramUniform2i64NV = (PFNGLPROGRAMUNIFORM2I64NVPROC)PTR_RESOLVE(glProgramUniform2i64NV);
	glad_glProgramUniform3i64NV = (PFNGLPROGRAMUNIFORM3I64NVPROC)PTR_RESOLVE(glProgramUniform3i64NV);
	glad_glProgramUniform4i64NV = (PFNGLPROGRAMUNIFORM4I64NVPROC)PTR_RESOLVE(glProgramUniform4i64NV);
	glad_glProgramUniform1i64vNV = (PFNGLPROGRAMUNIFORM1I64VNVPROC)PTR_RESOLVE(glProgramUniform1i64vNV);
	glad_glProgramUniform2i64vNV = (PFNGLPROGRAMUNIFORM2I64VNVPROC)PTR_RESOLVE(glProgramUniform2i64vNV);
	glad_glProgramUniform3i64vNV = (PFNGLPROGRAMUNIFORM3I64VNVPROC)PTR_RESOLVE(glProgramUniform3i64vNV);
	glad_glProgramUniform4i64vNV = (PFNGLPROGRAMUNIFORM4I64VNVPROC)PTR_RESOLVE(glProgramUniform4i64vNV);
	glad_glProgramUniform1ui64NV = (PFNGLPROGRAMUNIFORM1UI64NVPROC)PTR_RESOLVE(glProgramUniform1ui64NV);
	glad_glProgramUniform2ui64NV = (PFNGLPROGRAMUNIFORM2UI64NVPROC)PTR_RESOLVE(glProgramUniform2ui64NV);
	glad_glProgramUniform3ui64NV = (PFNGLPROGRAMUNIFORM3UI64NVPROC)PTR_RESOLVE(glProgramUniform3ui64NV);
	glad_glProgramUniform4ui64NV = (PFNGLPROGRAMUNIFORM4UI64NVPROC)PTR_RESOLVE(glProgramUniform4ui64NV);
	glad_glProgramUniform1ui64vNV = (PFNGLPROGRAMUNIFORM1UI64VNVPROC)PTR_RESOLVE(glProgramUniform1ui64vNV);
	glad_glProgramUniform2ui64vNV = (PFNGLPROGRAMUNIFORM2UI64VNVPROC)PTR_RESOLVE(glProgramUniform2ui64vNV);
	glad_glProgramUniform3ui64vNV = (PFNGLPROGRAMUNIFORM3UI64VNVPROC)PTR_RESOLVE(glProgramUniform3ui64vNV);
	glad_glProgramUniform4ui64vNV = (PFNGLPROGRAMUNIFORM4UI64VNVPROC)PTR_RESOLVE(glProgramUniform4ui64vNV);
	glad_glVertexAttribDivisorNV = (PFNGLVERTEXATTRIBDIVISORNVPROC)PTR_RESOLVE(glVertexAttribDivisorNV);
	glad_glGetInternalformatSampleivNV = (PFNGLGETINTERNALFORMATSAMPLEIVNVPROC)PTR_RESOLVE(glGetInternalformatSampleivNV);
	glad_glGetMemoryObjectDetachedResourcesuivNV = (PFNGLGETMEMORYOBJECTDETACHEDRESOURCESUIVNVPROC)PTR_RESOLVE(glGetMemoryObjectDetachedResourcesuivNV);
	glad_glResetMemoryObjectParameterNV = (PFNGLRESETMEMORYOBJECTPARAMETERNVPROC)PTR_RESOLVE(glResetMemoryObjectParameterNV);
	glad_glTexAttachMemoryNV = (PFNGLTEXATTACHMEMORYNVPROC)PTR_RESOLVE(glTexAttachMemoryNV);
	glad_glBufferAttachMemoryNV = (PFNGLBUFFERATTACHMEMORYNVPROC)PTR_RESOLVE(glBufferAttachMemoryNV);
	glad_glTextureAttachMemoryNV = (PFNGLTEXTUREATTACHMEMORYNVPROC)PTR_RESOLVE(glTextureAttachMemoryNV);
	glad_glNamedBufferAttachMemoryNV = (PFNGLNAMEDBUFFERATTACHMEMORYNVPROC)PTR_RESOLVE(glNamedBufferAttachMemoryNV);
	glad_glBufferPageCommitmentMemNV = (PFNGLBUFFERPAGECOMMITMENTMEMNVPROC)PTR_RESOLVE(glBufferPageCommitmentMemNV);
	glad_glTexPageCommitmentMemNV = (PFNGLTEXPAGECOMMITMENTMEMNVPROC)PTR_RESOLVE(glTexPageCommitmentMemNV);
	glad_glNamedBufferPageCommitmentMemNV = (PFNGLNAMEDBUFFERPAGECOMMITMENTMEMNVPROC)PTR_RESOLVE(glNamedBufferPageCommitmentMemNV);
	glad_glTexturePageCommitmentMemNV = (PFNGLTEXTUREPAGECOMMITMENTMEMNVPROC)PTR_RESOLVE(glTexturePageCommitmentMemNV);
	glad_glDrawMeshTasksNV = (PFNGLDRAWMESHTASKSNVPROC)PTR_RESOLVE(glDrawMeshTasksNV);
	glad_glDrawMeshTasksIndirectNV = (PFNGLDRAWMESHTASKSINDIRECTNVPROC)PTR_RESOLVE(glDrawMeshTasksIndirectNV);
	glad_glMultiDrawMeshTasksIndirectNV = (PFNGLMULTIDRAWMESHTASKSINDIRECTNVPROC)PTR_RESOLVE(glMultiDrawMeshTasksIndirectNV);
	glad_glMultiDrawMeshTasksIndirectCountNV = (PFNGLMULTIDRAWMESHTASKSINDIRECTCOUNTNVPROC)PTR_RESOLVE(glMultiDrawMeshTasksIndirectCountNV);
	glad_glUniformMatrix2x3fvNV = (PFNGLUNIFORMMATRIX2X3FVNVPROC)PTR_RESOLVE(glUniformMatrix2x3fvNV);
	glad_glUniformMatrix3x2fvNV = (PFNGLUNIFORMMATRIX3X2FVNVPROC)PTR_RESOLVE(glUniformMatrix3x2fvNV);
	glad_glUniformMatrix2x4fvNV = (PFNGLUNIFORMMATRIX2X4FVNVPROC)PTR_RESOLVE(glUniformMatrix2x4fvNV);
	glad_glUniformMatrix4x2fvNV = (PFNGLUNIFORMMATRIX4X2FVNVPROC)PTR_RESOLVE(glUniformMatrix4x2fvNV);
	glad_glUniformMatrix3x4fvNV = (PFNGLUNIFORMMATRIX3X4FVNVPROC)PTR_RESOLVE(glUniformMatrix3x4fvNV);
	glad_glUniformMatrix4x3fvNV = (PFNGLUNIFORMMATRIX4X3FVNVPROC)PTR_RESOLVE(glUniformMatrix4x3fvNV);
	glad_glGenPathsNV = (PFNGLGENPATHSNVPROC)PTR_RESOLVE(glGenPathsNV);
	glad_glDeletePathsNV = (PFNGLDELETEPATHSNVPROC)PTR_RESOLVE(glDeletePathsNV);
	glad_glIsPathNV = (PFNGLISPATHNVPROC)PTR_RESOLVE(glIsPathNV);
	glad_glPathCommandsNV = (PFNGLPATHCOMMANDSNVPROC)PTR_RESOLVE(glPathCommandsNV);
	glad_glPathCoordsNV = (PFNGLPATHCOORDSNVPROC)PTR_RESOLVE(glPathCoordsNV);
	glad_glPathSubCommandsNV = (PFNGLPATHSUBCOMMANDSNVPROC)PTR_RESOLVE(glPathSubCommandsNV);
	glad_glPathSubCoordsNV = (PFNGLPATHSUBCOORDSNVPROC)PTR_RESOLVE(glPathSubCoordsNV);
	glad_glPathStringNV = (PFNGLPATHSTRINGNVPROC)PTR_RESOLVE(glPathStringNV);
	glad_glPathGlyphsNV = (PFNGLPATHGLYPHSNVPROC)PTR_RESOLVE(glPathGlyphsNV);
	glad_glPathGlyphRangeNV = (PFNGLPATHGLYPHRANGENVPROC)PTR_RESOLVE(glPathGlyphRangeNV);
	glad_glWeightPathsNV = (PFNGLWEIGHTPATHSNVPROC)PTR_RESOLVE(glWeightPathsNV);
	glad_glCopyPathNV = (PFNGLCOPYPATHNVPROC)PTR_RESOLVE(glCopyPathNV);
	glad_glInterpolatePathsNV = (PFNGLINTERPOLATEPATHSNVPROC)PTR_RESOLVE(glInterpolatePathsNV);
	glad_glTransformPathNV = (PFNGLTRANSFORMPATHNVPROC)PTR_RESOLVE(glTransformPathNV);
	glad_glPathParameterivNV = (PFNGLPATHPARAMETERIVNVPROC)PTR_RESOLVE(glPathParameterivNV);
	glad_glPathParameteriNV = (PFNGLPATHPARAMETERINVPROC)PTR_RESOLVE(glPathParameteriNV);
	glad_glPathParameterfvNV = (PFNGLPATHPARAMETERFVNVPROC)PTR_RESOLVE(glPathParameterfvNV);
	glad_glPathParameterfNV = (PFNGLPATHPARAMETERFNVPROC)PTR_RESOLVE(glPathParameterfNV);
	glad_glPathDashArrayNV = (PFNGLPATHDASHARRAYNVPROC)PTR_RESOLVE(glPathDashArrayNV);
	glad_glPathStencilFuncNV = (PFNGLPATHSTENCILFUNCNVPROC)PTR_RESOLVE(glPathStencilFuncNV);
	glad_glPathStencilDepthOffsetNV = (PFNGLPATHSTENCILDEPTHOFFSETNVPROC)PTR_RESOLVE(glPathStencilDepthOffsetNV);
	glad_glStencilFillPathNV = (PFNGLSTENCILFILLPATHNVPROC)PTR_RESOLVE(glStencilFillPathNV);
	glad_glStencilStrokePathNV = (PFNGLSTENCILSTROKEPATHNVPROC)PTR_RESOLVE(glStencilStrokePathNV);
	glad_glStencilFillPathInstancedNV = (PFNGLSTENCILFILLPATHINSTANCEDNVPROC)PTR_RESOLVE(glStencilFillPathInstancedNV);
	glad_glStencilStrokePathInstancedNV = (PFNGLSTENCILSTROKEPATHINSTANCEDNVPROC)PTR_RESOLVE(glStencilStrokePathInstancedNV);
	glad_glPathCoverDepthFuncNV = (PFNGLPATHCOVERDEPTHFUNCNVPROC)PTR_RESOLVE(glPathCoverDepthFuncNV);
	glad_glCoverFillPathNV = (PFNGLCOVERFILLPATHNVPROC)PTR_RESOLVE(glCoverFillPathNV);
	glad_glCoverStrokePathNV = (PFNGLCOVERSTROKEPATHNVPROC)PTR_RESOLVE(glCoverStrokePathNV);
	glad_glCoverFillPathInstancedNV = (PFNGLCOVERFILLPATHINSTANCEDNVPROC)PTR_RESOLVE(glCoverFillPathInstancedNV);
	glad_glCoverStrokePathInstancedNV = (PFNGLCOVERSTROKEPATHINSTANCEDNVPROC)PTR_RESOLVE(glCoverStrokePathInstancedNV);
	glad_glGetPathParameterivNV = (PFNGLGETPATHPARAMETERIVNVPROC)PTR_RESOLVE(glGetPathParameterivNV);
	glad_glGetPathParameterfvNV = (PFNGLGETPATHPARAMETERFVNVPROC)PTR_RESOLVE(glGetPathParameterfvNV);
	glad_glGetPathCommandsNV = (PFNGLGETPATHCOMMANDSNVPROC)PTR_RESOLVE(glGetPathCommandsNV);
	glad_glGetPathCoordsNV = (PFNGLGETPATHCOORDSNVPROC)PTR_RESOLVE(glGetPathCoordsNV);
	glad_glGetPathDashArrayNV = (PFNGLGETPATHDASHARRAYNVPROC)PTR_RESOLVE(glGetPathDashArrayNV);
	glad_glGetPathMetricsNV = (PFNGLGETPATHMETRICSNVPROC)PTR_RESOLVE(glGetPathMetricsNV);
	glad_glGetPathMetricRangeNV = (PFNGLGETPATHMETRICRANGENVPROC)PTR_RESOLVE(glGetPathMetricRangeNV);
	glad_glGetPathSpacingNV = (PFNGLGETPATHSPACINGNVPROC)PTR_RESOLVE(glGetPathSpacingNV);
	glad_glIsPointInFillPathNV = (PFNGLISPOINTINFILLPATHNVPROC)PTR_RESOLVE(glIsPointInFillPathNV);
	glad_glIsPointInStrokePathNV = (PFNGLISPOINTINSTROKEPATHNVPROC)PTR_RESOLVE(glIsPointInStrokePathNV);
	glad_glGetPathLengthNV = (PFNGLGETPATHLENGTHNVPROC)PTR_RESOLVE(glGetPathLengthNV);
	glad_glPointAlongPathNV = (PFNGLPOINTALONGPATHNVPROC)PTR_RESOLVE(glPointAlongPathNV);
	glad_glMatrixLoad3x2fNV = (PFNGLMATRIXLOAD3X2FNVPROC)PTR_RESOLVE(glMatrixLoad3x2fNV);
	glad_glMatrixLoad3x3fNV = (PFNGLMATRIXLOAD3X3FNVPROC)PTR_RESOLVE(glMatrixLoad3x3fNV);
	glad_glMatrixLoadTranspose3x3fNV = (PFNGLMATRIXLOADTRANSPOSE3X3FNVPROC)PTR_RESOLVE(glMatrixLoadTranspose3x3fNV);
	glad_glMatrixMult3x2fNV = (PFNGLMATRIXMULT3X2FNVPROC)PTR_RESOLVE(glMatrixMult3x2fNV);
	glad_glMatrixMult3x3fNV = (PFNGLMATRIXMULT3X3FNVPROC)PTR_RESOLVE(glMatrixMult3x3fNV);
	glad_glMatrixMultTranspose3x3fNV = (PFNGLMATRIXMULTTRANSPOSE3X3FNVPROC)PTR_RESOLVE(glMatrixMultTranspose3x3fNV);
	glad_glStencilThenCoverFillPathNV = (PFNGLSTENCILTHENCOVERFILLPATHNVPROC)PTR_RESOLVE(glStencilThenCoverFillPathNV);
	glad_glStencilThenCoverStrokePathNV = (PFNGLSTENCILTHENCOVERSTROKEPATHNVPROC)PTR_RESOLVE(glStencilThenCoverStrokePathNV);
	glad_glStencilThenCoverFillPathInstancedNV = (PFNGLSTENCILTHENCOVERFILLPATHINSTANCEDNVPROC)PTR_RESOLVE(glStencilThenCoverFillPathInstancedNV);
	glad_glStencilThenCoverStrokePathInstancedNV = (PFNGLSTENCILTHENCOVERSTROKEPATHINSTANCEDNVPROC)PTR_RESOLVE(glStencilThenCoverStrokePathInstancedNV);
	glad_glPathGlyphIndexRangeNV = (PFNGLPATHGLYPHINDEXRANGENVPROC)PTR_RESOLVE(glPathGlyphIndexRangeNV);
	glad_glPathGlyphIndexArrayNV = (PFNGLPATHGLYPHINDEXARRAYNVPROC)PTR_RESOLVE(glPathGlyphIndexArrayNV);
	glad_glPathMemoryGlyphIndexArrayNV = (PFNGLPATHMEMORYGLYPHINDEXARRAYNVPROC)PTR_RESOLVE(glPathMemoryGlyphIndexArrayNV);
	glad_glProgramPathFragmentInputGenNV = (PFNGLPROGRAMPATHFRAGMENTINPUTGENNVPROC)PTR_RESOLVE(glProgramPathFragmentInputGenNV);
	glad_glGetProgramResourcefvNV = (PFNGLGETPROGRAMRESOURCEFVNVPROC)PTR_RESOLVE(glGetProgramResourcefvNV);
	glad_glPathColorGenNV = (PFNGLPATHCOLORGENNVPROC)PTR_RESOLVE(glPathColorGenNV);
	glad_glPathTexGenNV = (PFNGLPATHTEXGENNVPROC)PTR_RESOLVE(glPathTexGenNV);
	glad_glPathFogGenNV = (PFNGLPATHFOGGENNVPROC)PTR_RESOLVE(glPathFogGenNV);
	glad_glGetPathColorGenivNV = (PFNGLGETPATHCOLORGENIVNVPROC)PTR_RESOLVE(glGetPathColorGenivNV);
	glad_glGetPathColorGenfvNV = (PFNGLGETPATHCOLORGENFVNVPROC)PTR_RESOLVE(glGetPathColorGenfvNV);
	glad_glGetPathTexGenivNV = (PFNGLGETPATHTEXGENIVNVPROC)PTR_RESOLVE(glGetPathTexGenivNV);
	glad_glGetPathTexGenfvNV = (PFNGLGETPATHTEXGENFVNVPROC)PTR_RESOLVE(glGetPathTexGenfvNV);
	glad_glMatrixFrustumEXT = (PFNGLMATRIXFRUSTUMEXTPROC)PTR_RESOLVE(glMatrixFrustumEXT);
	glad_glMatrixLoadIdentityEXT = (PFNGLMATRIXLOADIDENTITYEXTPROC)PTR_RESOLVE(glMatrixLoadIdentityEXT);
	glad_glMatrixLoadTransposefEXT = (PFNGLMATRIXLOADTRANSPOSEFEXTPROC)PTR_RESOLVE(glMatrixLoadTransposefEXT);
	glad_glMatrixLoadTransposedEXT = (PFNGLMATRIXLOADTRANSPOSEDEXTPROC)PTR_RESOLVE(glMatrixLoadTransposedEXT);
	glad_glMatrixLoadfEXT = (PFNGLMATRIXLOADFEXTPROC)PTR_RESOLVE(glMatrixLoadfEXT);
	glad_glMatrixLoaddEXT = (PFNGLMATRIXLOADDEXTPROC)PTR_RESOLVE(glMatrixLoaddEXT);
	glad_glMatrixMultTransposefEXT = (PFNGLMATRIXMULTTRANSPOSEFEXTPROC)PTR_RESOLVE(glMatrixMultTransposefEXT);
	glad_glMatrixMultTransposedEXT = (PFNGLMATRIXMULTTRANSPOSEDEXTPROC)PTR_RESOLVE(glMatrixMultTransposedEXT);
	glad_glMatrixMultfEXT = (PFNGLMATRIXMULTFEXTPROC)PTR_RESOLVE(glMatrixMultfEXT);
	glad_glMatrixMultdEXT = (PFNGLMATRIXMULTDEXTPROC)PTR_RESOLVE(glMatrixMultdEXT);
	glad_glMatrixOrthoEXT = (PFNGLMATRIXORTHOEXTPROC)PTR_RESOLVE(glMatrixOrthoEXT);
	glad_glMatrixPopEXT = (PFNGLMATRIXPOPEXTPROC)PTR_RESOLVE(glMatrixPopEXT);
	glad_glMatrixPushEXT = (PFNGLMATRIXPUSHEXTPROC)PTR_RESOLVE(glMatrixPushEXT);
	glad_glMatrixRotatefEXT = (PFNGLMATRIXROTATEFEXTPROC)PTR_RESOLVE(glMatrixRotatefEXT);
	glad_glMatrixRotatedEXT = (PFNGLMATRIXROTATEDEXTPROC)PTR_RESOLVE(glMatrixRotatedEXT);
	glad_glMatrixScalefEXT = (PFNGLMATRIXSCALEFEXTPROC)PTR_RESOLVE(glMatrixScalefEXT);
	glad_glMatrixScaledEXT = (PFNGLMATRIXSCALEDEXTPROC)PTR_RESOLVE(glMatrixScaledEXT);
	glad_glMatrixTranslatefEXT = (PFNGLMATRIXTRANSLATEFEXTPROC)PTR_RESOLVE(glMatrixTranslatefEXT);
	glad_glMatrixTranslatedEXT = (PFNGLMATRIXTRANSLATEDEXTPROC)PTR_RESOLVE(glMatrixTranslatedEXT);
	glad_glPolygonModeNV = (PFNGLPOLYGONMODENVPROC)PTR_RESOLVE(glPolygonModeNV);
	glad_glReadBufferNV = (PFNGLREADBUFFERNVPROC)PTR_RESOLVE(glReadBufferNV);
	glad_glFramebufferSampleLocationsfvNV = (PFNGLFRAMEBUFFERSAMPLELOCATIONSFVNVPROC)PTR_RESOLVE(glFramebufferSampleLocationsfvNV);
	glad_glNamedFramebufferSampleLocationsfvNV = (PFNGLNAMEDFRAMEBUFFERSAMPLELOCATIONSFVNVPROC)PTR_RESOLVE(glNamedFramebufferSampleLocationsfvNV);
	glad_glResolveDepthValuesNV = (PFNGLRESOLVEDEPTHVALUESNVPROC)PTR_RESOLVE(glResolveDepthValuesNV);
	glad_glScissorExclusiveNV = (PFNGLSCISSOREXCLUSIVENVPROC)PTR_RESOLVE(glScissorExclusiveNV);
	glad_glScissorExclusiveArrayvNV = (PFNGLSCISSOREXCLUSIVEARRAYVNVPROC)PTR_RESOLVE(glScissorExclusiveArrayvNV);
	glad_glBindShadingRateImageNV = (PFNGLBINDSHADINGRATEIMAGENVPROC)PTR_RESOLVE(glBindShadingRateImageNV);
	glad_glGetShadingRateImagePaletteNV = (PFNGLGETSHADINGRATEIMAGEPALETTENVPROC)PTR_RESOLVE(glGetShadingRateImagePaletteNV);
	glad_glGetShadingRateSampleLocationivNV = (PFNGLGETSHADINGRATESAMPLELOCATIONIVNVPROC)PTR_RESOLVE(glGetShadingRateSampleLocationivNV);
	glad_glShadingRateImageBarrierNV = (PFNGLSHADINGRATEIMAGEBARRIERNVPROC)PTR_RESOLVE(glShadingRateImageBarrierNV);
	glad_glShadingRateImagePaletteNV = (PFNGLSHADINGRATEIMAGEPALETTENVPROC)PTR_RESOLVE(glShadingRateImagePaletteNV);
	glad_glShadingRateSampleOrderNV = (PFNGLSHADINGRATESAMPLEORDERNVPROC)PTR_RESOLVE(glShadingRateSampleOrderNV);
	glad_glShadingRateSampleOrderCustomNV = (PFNGLSHADINGRATESAMPLEORDERCUSTOMNVPROC)PTR_RESOLVE(glShadingRateSampleOrderCustomNV);
	glad_glTextureBarrierNV = (PFNGLTEXTUREBARRIERNVPROC)PTR_RESOLVE(glTextureBarrierNV);
	glad_glCreateSemaphoresNV = (PFNGLCREATESEMAPHORESNVPROC)PTR_RESOLVE(glCreateSemaphoresNV);
	glad_glSemaphoreParameterivNV = (PFNGLSEMAPHOREPARAMETERIVNVPROC)PTR_RESOLVE(glSemaphoreParameterivNV);
	glad_glGetSemaphoreParameterivNV = (PFNGLGETSEMAPHOREPARAMETERIVNVPROC)PTR_RESOLVE(glGetSemaphoreParameterivNV);
	glad_glViewportArrayvNV = (PFNGLVIEWPORTARRAYVNVPROC)PTR_RESOLVE(glViewportArrayvNV);
	glad_glViewportIndexedfNV = (PFNGLVIEWPORTINDEXEDFNVPROC)PTR_RESOLVE(glViewportIndexedfNV);
	glad_glViewportIndexedfvNV = (PFNGLVIEWPORTINDEXEDFVNVPROC)PTR_RESOLVE(glViewportIndexedfvNV);
	glad_glScissorArrayvNV = (PFNGLSCISSORARRAYVNVPROC)PTR_RESOLVE(glScissorArrayvNV);
	glad_glScissorIndexedNV = (PFNGLSCISSORINDEXEDNVPROC)PTR_RESOLVE(glScissorIndexedNV);
	glad_glScissorIndexedvNV = (PFNGLSCISSORINDEXEDVNVPROC)PTR_RESOLVE(glScissorIndexedvNV);
	glad_glDepthRangeArrayfvNV = (PFNGLDEPTHRANGEARRAYFVNVPROC)PTR_RESOLVE(glDepthRangeArrayfvNV);
	glad_glDepthRangeIndexedfNV = (PFNGLDEPTHRANGEINDEXEDFNVPROC)PTR_RESOLVE(glDepthRangeIndexedfNV);
	glad_glGetFloati_vNV = (PFNGLGETFLOATI_VNVPROC)PTR_RESOLVE(glGetFloati_vNV);
	glad_glEnableiNV = (PFNGLENABLEINVPROC)PTR_RESOLVE(glEnableiNV);
	glad_glDisableiNV = (PFNGLDISABLEINVPROC)PTR_RESOLVE(glDisableiNV);
	glad_glIsEnablediNV = (PFNGLISENABLEDINVPROC)PTR_RESOLVE(glIsEnablediNV);
	glad_glViewportSwizzleNV = (PFNGLVIEWPORTSWIZZLENVPROC)PTR_RESOLVE(glViewportSwizzleNV);
	glad_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)PTR_RESOLVE(glEGLImageTargetTexture2DOES);
	glad_glEGLImageTargetRenderbufferStorageOES = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)PTR_RESOLVE(glEGLImageTargetRenderbufferStorageOES);
	glad_glCopyImageSubDataOES = (PFNGLCOPYIMAGESUBDATAOESPROC)PTR_RESOLVE(glCopyImageSubDataOES);
	glad_glEnableiOES = (PFNGLENABLEIOESPROC)PTR_RESOLVE(glEnableiOES);
	glad_glDisableiOES = (PFNGLDISABLEIOESPROC)PTR_RESOLVE(glDisableiOES);
	glad_glBlendEquationiOES = (PFNGLBLENDEQUATIONIOESPROC)PTR_RESOLVE(glBlendEquationiOES);
	glad_glBlendEquationSeparateiOES = (PFNGLBLENDEQUATIONSEPARATEIOESPROC)PTR_RESOLVE(glBlendEquationSeparateiOES);
	glad_glBlendFunciOES = (PFNGLBLENDFUNCIOESPROC)PTR_RESOLVE(glBlendFunciOES);
	glad_glBlendFuncSeparateiOES = (PFNGLBLENDFUNCSEPARATEIOESPROC)PTR_RESOLVE(glBlendFuncSeparateiOES);
	glad_glColorMaskiOES = (PFNGLCOLORMASKIOESPROC)PTR_RESOLVE(glColorMaskiOES);
	glad_glIsEnablediOES = (PFNGLISENABLEDIOESPROC)PTR_RESOLVE(glIsEnablediOES);
	glad_glDrawElementsBaseVertexOES = (PFNGLDRAWELEMENTSBASEVERTEXOESPROC)PTR_RESOLVE(glDrawElementsBaseVertexOES);
	glad_glDrawRangeElementsBaseVertexOES = (PFNGLDRAWRANGEELEMENTSBASEVERTEXOESPROC)PTR_RESOLVE(glDrawRangeElementsBaseVertexOES);
	glad_glDrawElementsInstancedBaseVertexOES = (PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXOESPROC)PTR_RESOLVE(glDrawElementsInstancedBaseVertexOES);
	glad_glMultiDrawElementsBaseVertexEXT = (PFNGLMULTIDRAWELEMENTSBASEVERTEXEXTPROC)PTR_RESOLVE(glMultiDrawElementsBaseVertexEXT);
	glad_glFramebufferTextureOES = (PFNGLFRAMEBUFFERTEXTUREOESPROC)PTR_RESOLVE(glFramebufferTextureOES);
	glad_glGetProgramBinaryOES = (PFNGLGETPROGRAMBINARYOESPROC)PTR_RESOLVE(glGetProgramBinaryOES);
	glad_glProgramBinaryOES = (PFNGLPROGRAMBINARYOESPROC)PTR_RESOLVE(glProgramBinaryOES);
	glad_glMapBufferOES = (PFNGLMAPBUFFEROESPROC)PTR_RESOLVE(glMapBufferOES);
	glad_glUnmapBufferOES = (PFNGLUNMAPBUFFEROESPROC)PTR_RESOLVE(glUnmapBufferOES);
	glad_glGetBufferPointervOES = (PFNGLGETBUFFERPOINTERVOESPROC)PTR_RESOLVE(glGetBufferPointervOES);
	glad_glPrimitiveBoundingBoxOES = (PFNGLPRIMITIVEBOUNDINGBOXOESPROC)PTR_RESOLVE(glPrimitiveBoundingBoxOES);
	glad_glMinSampleShadingOES = (PFNGLMINSAMPLESHADINGOESPROC)PTR_RESOLVE(glMinSampleShadingOES);
	glad_glPatchParameteriOES = (PFNGLPATCHPARAMETERIOESPROC)PTR_RESOLVE(glPatchParameteriOES);
	glad_glTexImage3DOES = (PFNGLTEXIMAGE3DOESPROC)PTR_RESOLVE(glTexImage3DOES);
	glad_glTexSubImage3DOES = (PFNGLTEXSUBIMAGE3DOESPROC)PTR_RESOLVE(glTexSubImage3DOES);
	glad_glCopyTexSubImage3DOES = (PFNGLCOPYTEXSUBIMAGE3DOESPROC)PTR_RESOLVE(glCopyTexSubImage3DOES);
	glad_glCompressedTexImage3DOES = (PFNGLCOMPRESSEDTEXIMAGE3DOESPROC)PTR_RESOLVE(glCompressedTexImage3DOES);
	glad_glCompressedTexSubImage3DOES = (PFNGLCOMPRESSEDTEXSUBIMAGE3DOESPROC)PTR_RESOLVE(glCompressedTexSubImage3DOES);
	glad_glFramebufferTexture3DOES = (PFNGLFRAMEBUFFERTEXTURE3DOESPROC)PTR_RESOLVE(glFramebufferTexture3DOES);
	glad_glTexParameterIivOES = (PFNGLTEXPARAMETERIIVOESPROC)PTR_RESOLVE(glTexParameterIivOES);
	glad_glTexParameterIuivOES = (PFNGLTEXPARAMETERIUIVOESPROC)PTR_RESOLVE(glTexParameterIuivOES);
	glad_glGetTexParameterIivOES = (PFNGLGETTEXPARAMETERIIVOESPROC)PTR_RESOLVE(glGetTexParameterIivOES);
	glad_glGetTexParameterIuivOES = (PFNGLGETTEXPARAMETERIUIVOESPROC)PTR_RESOLVE(glGetTexParameterIuivOES);
	glad_glSamplerParameterIivOES = (PFNGLSAMPLERPARAMETERIIVOESPROC)PTR_RESOLVE(glSamplerParameterIivOES);
	glad_glSamplerParameterIuivOES = (PFNGLSAMPLERPARAMETERIUIVOESPROC)PTR_RESOLVE(glSamplerParameterIuivOES);
	glad_glGetSamplerParameterIivOES = (PFNGLGETSAMPLERPARAMETERIIVOESPROC)PTR_RESOLVE(glGetSamplerParameterIivOES);
	glad_glGetSamplerParameterIuivOES = (PFNGLGETSAMPLERPARAMETERIUIVOESPROC)PTR_RESOLVE(glGetSamplerParameterIuivOES);
	glad_glTexBufferOES = (PFNGLTEXBUFFEROESPROC)PTR_RESOLVE(glTexBufferOES);
	glad_glTexBufferRangeOES = (PFNGLTEXBUFFERRANGEOESPROC)PTR_RESOLVE(glTexBufferRangeOES);
	glad_glTexStorage3DMultisampleOES = (PFNGLTEXSTORAGE3DMULTISAMPLEOESPROC)PTR_RESOLVE(glTexStorage3DMultisampleOES);
	glad_glTextureViewOES = (PFNGLTEXTUREVIEWOESPROC)PTR_RESOLVE(glTextureViewOES);
	glad_glBindVertexArrayOES = (PFNGLBINDVERTEXARRAYOESPROC)PTR_RESOLVE(glBindVertexArrayOES);
	glad_glDeleteVertexArraysOES = (PFNGLDELETEVERTEXARRAYSOESPROC)PTR_RESOLVE(glDeleteVertexArraysOES);
	glad_glGenVertexArraysOES = (PFNGLGENVERTEXARRAYSOESPROC)PTR_RESOLVE(glGenVertexArraysOES);
	glad_glIsVertexArrayOES = (PFNGLISVERTEXARRAYOESPROC)PTR_RESOLVE(glIsVertexArrayOES);
	glad_glViewportArrayvOES = (PFNGLVIEWPORTARRAYVOESPROC)PTR_RESOLVE(glViewportArrayvOES);
	glad_glViewportIndexedfOES = (PFNGLVIEWPORTINDEXEDFOESPROC)PTR_RESOLVE(glViewportIndexedfOES);
	glad_glViewportIndexedfvOES = (PFNGLVIEWPORTINDEXEDFVOESPROC)PTR_RESOLVE(glViewportIndexedfvOES);
	glad_glScissorArrayvOES = (PFNGLSCISSORARRAYVOESPROC)PTR_RESOLVE(glScissorArrayvOES);
	glad_glScissorIndexedOES = (PFNGLSCISSORINDEXEDOESPROC)PTR_RESOLVE(glScissorIndexedOES);
	glad_glScissorIndexedvOES = (PFNGLSCISSORINDEXEDVOESPROC)PTR_RESOLVE(glScissorIndexedvOES);
	glad_glDepthRangeArrayfvOES = (PFNGLDEPTHRANGEARRAYFVOESPROC)PTR_RESOLVE(glDepthRangeArrayfvOES);
	glad_glDepthRangeIndexedfOES = (PFNGLDEPTHRANGEINDEXEDFOESPROC)PTR_RESOLVE(glDepthRangeIndexedfOES);
	glad_glGetFloati_vOES = (PFNGLGETFLOATI_VOESPROC)PTR_RESOLVE(glGetFloati_vOES);
	glad_glEnableiOES = (PFNGLENABLEIOESPROC)PTR_RESOLVE(glEnableiOES);
	glad_glDisableiOES = (PFNGLDISABLEIOESPROC)PTR_RESOLVE(glDisableiOES);
	glad_glIsEnablediOES = (PFNGLISENABLEDIOESPROC)PTR_RESOLVE(glIsEnablediOES);
	glad_glFramebufferTextureMultiviewOVR = (PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)PTR_RESOLVE(glFramebufferTextureMultiviewOVR);
	glad_glNamedFramebufferTextureMultiviewOVR = (PFNGLNAMEDFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)PTR_RESOLVE(glNamedFramebufferTextureMultiviewOVR);
	glad_glFramebufferTextureMultisampleMultiviewOVR = (PFNGLFRAMEBUFFERTEXTUREMULTISAMPLEMULTIVIEWOVRPROC)PTR_RESOLVE(glFramebufferTextureMultisampleMultiviewOVR);
	glad_glAlphaFuncQCOM = (PFNGLALPHAFUNCQCOMPROC)PTR_RESOLVE(glAlphaFuncQCOM);
	glad_glGetDriverControlsQCOM = (PFNGLGETDRIVERCONTROLSQCOMPROC)PTR_RESOLVE(glGetDriverControlsQCOM);
	glad_glGetDriverControlStringQCOM = (PFNGLGETDRIVERCONTROLSTRINGQCOMPROC)PTR_RESOLVE(glGetDriverControlStringQCOM);
	glad_glEnableDriverControlQCOM = (PFNGLENABLEDRIVERCONTROLQCOMPROC)PTR_RESOLVE(glEnableDriverControlQCOM);
	glad_glDisableDriverControlQCOM = (PFNGLDISABLEDRIVERCONTROLQCOMPROC)PTR_RESOLVE(glDisableDriverControlQCOM);
	glad_glExtGetTexturesQCOM = (PFNGLEXTGETTEXTURESQCOMPROC)PTR_RESOLVE(glExtGetTexturesQCOM);
	glad_glExtGetBuffersQCOM = (PFNGLEXTGETBUFFERSQCOMPROC)PTR_RESOLVE(glExtGetBuffersQCOM);
	glad_glExtGetRenderbuffersQCOM = (PFNGLEXTGETRENDERBUFFERSQCOMPROC)PTR_RESOLVE(glExtGetRenderbuffersQCOM);
	glad_glExtGetFramebuffersQCOM = (PFNGLEXTGETFRAMEBUFFERSQCOMPROC)PTR_RESOLVE(glExtGetFramebuffersQCOM);
	glad_glExtGetTexLevelParameterivQCOM = (PFNGLEXTGETTEXLEVELPARAMETERIVQCOMPROC)PTR_RESOLVE(glExtGetTexLevelParameterivQCOM);
	glad_glExtTexObjectStateOverrideiQCOM = (PFNGLEXTTEXOBJECTSTATEOVERRIDEIQCOMPROC)PTR_RESOLVE(glExtTexObjectStateOverrideiQCOM);
	glad_glExtGetTexSubImageQCOM = (PFNGLEXTGETTEXSUBIMAGEQCOMPROC)PTR_RESOLVE(glExtGetTexSubImageQCOM);
	glad_glExtGetBufferPointervQCOM = (PFNGLEXTGETBUFFERPOINTERVQCOMPROC)PTR_RESOLVE(glExtGetBufferPointervQCOM);
	glad_glExtGetShadersQCOM = (PFNGLEXTGETSHADERSQCOMPROC)PTR_RESOLVE(glExtGetShadersQCOM);
	glad_glExtGetProgramsQCOM = (PFNGLEXTGETPROGRAMSQCOMPROC)PTR_RESOLVE(glExtGetProgramsQCOM);
	glad_glExtIsProgramBinaryQCOM = (PFNGLEXTISPROGRAMBINARYQCOMPROC)PTR_RESOLVE(glExtIsProgramBinaryQCOM);
	glad_glExtGetProgramBinarySourceQCOM = (PFNGLEXTGETPROGRAMBINARYSOURCEQCOMPROC)PTR_RESOLVE(glExtGetProgramBinarySourceQCOM);
	glad_glExtrapolateTex2DQCOM = (PFNGLEXTRAPOLATETEX2DQCOMPROC)PTR_RESOLVE(glExtrapolateTex2DQCOM);
	glad_glFramebufferFoveationConfigQCOM = (PFNGLFRAMEBUFFERFOVEATIONCONFIGQCOMPROC)PTR_RESOLVE(glFramebufferFoveationConfigQCOM);
	glad_glFramebufferFoveationParametersQCOM = (PFNGLFRAMEBUFFERFOVEATIONPARAMETERSQCOMPROC)PTR_RESOLVE(glFramebufferFoveationParametersQCOM);
	glad_glTexEstimateMotionQCOM = (PFNGLTEXESTIMATEMOTIONQCOMPROC)PTR_RESOLVE(glTexEstimateMotionQCOM);
	glad_glTexEstimateMotionRegionsQCOM = (PFNGLTEXESTIMATEMOTIONREGIONSQCOMPROC)PTR_RESOLVE(glTexEstimateMotionRegionsQCOM);
	glad_glFramebufferFetchBarrierQCOM = (PFNGLFRAMEBUFFERFETCHBARRIERQCOMPROC)PTR_RESOLVE(glFramebufferFetchBarrierQCOM);
	glad_glShadingRateQCOM = (PFNGLSHADINGRATEQCOMPROC)PTR_RESOLVE(glShadingRateQCOM);
	glad_glTextureFoveationParametersQCOM = (PFNGLTEXTUREFOVEATIONPARAMETERSQCOMPROC)PTR_RESOLVE(glTextureFoveationParametersQCOM);
	glad_glStartTilingQCOM = (PFNGLSTARTTILINGQCOMPROC)PTR_RESOLVE(glStartTilingQCOM);
	glad_glEndTilingQCOM = (PFNGLENDTILINGQCOMPROC)PTR_RESOLVE(glEndTilingQCOM);

	// [BD-CAP] Splice in texture-cap wrappers.  No-op when textureMaxDim=0.
	bd_symtable_override("glTexStorage2D", (uintptr_t)&bd_glTexStorage2D);
	bd_symtable_override("glTexSubImage2D", (uintptr_t)&bd_glTexSubImage2D);
	bd_symtable_override("glBindTexture", (uintptr_t)&bd_glBindTexture);
	bd_symtable_override("glDeleteTextures", (uintptr_t)&bd_glDeleteTextures);
	// [BD-VIDEO] VideoPlayer: external texture binds are redirected to a
	// GL_TEXTURE_2D the loader fills from the MediaCodec thunk, and the blit
	// shader's samplerExternalOES is rewritten to sampler2D.
	bd_symtable_override("glShaderSource", (uintptr_t)&bd_glShaderSource);
	bd_symtable_override("glAttachShader", (uintptr_t)&bd_glAttachShader);
	bd_symtable_override("glLinkProgram", (uintptr_t)&bd_glLinkProgram);
	bd_symtable_override("glUseProgram", (uintptr_t)&bd_glUseProgram);
	bd_symtable_override("glDrawArrays", (uintptr_t)&bd_glDrawArrays);
	bd_symtable_override("glDrawElements", (uintptr_t)&bd_glDrawElements);
	bd_symtable_override("glDrawArraysInstanced", (uintptr_t)&bd_glDrawArraysInstanced);
	bd_symtable_override("glDrawElementsInstanced", (uintptr_t)&bd_glDrawElementsInstanced);
	bd_symtable_override("glActiveTexture", (uintptr_t)&bd_glActiveTexture);
	bd_symtable_override("glViewport", (uintptr_t)&bd_glViewport);
	bd_symtable_override("glTexParameteri", (uintptr_t)&bd_glTexParameteri);
	bd_symtable_override("glTexParameterf", (uintptr_t)&bd_glTexParameterf);
	bd_symtable_override("glEGLImageTargetTexture2DOES",
	                     (uintptr_t)&bd_glEGLImageTargetTexture2DOES);
	// Wire the bridge's per-frame upload to SurfaceTexture.updateTexImage().
	bd_video::set_upload_hook(&bd_video_present);
}
