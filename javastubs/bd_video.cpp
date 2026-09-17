#include "bd_video.h"

#include "logging.h"

#include "toml++/toml.hpp"
extern toml::table config;

#include <chrono>
#include <mutex>
#include <set>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

// Implemented by thunks/ndk/media.cpp.
extern "C" void bd_media_dump_state();

namespace {

std::mutex g_mutex;
// Serializes conversions: two codecs can publish at once, but they share the
// scratch buffer below.
std::mutex g_produce_mutex;

// Every SurfaceTexture Unity created, keyed by Unity's own handle (not the GL
// texture name - see bd_video.h).
std::set<unsigned> g_textures;
int g_active_texture = -1;
std::function<void()> g_notify;

std::function<void()> g_upload_hook;

// A decoded frame is waiting to be announced. It must NOT be announced from the
// media thread: Unity's listener runs updateTexImage() (and therefore our upload
// hook) synchronously, and the GL context only lives on the render thread. So
// submit_i420() sets this flag and pump() - called from eglSwapBuffers, i.e. the
// render thread with the context current - does the actual callback.
bool g_frame_ready = false;

// Render-thread liveness, counted in pump(). This is what lets the producer side
// tell "the guest is consuming at its own pace" apart from "the guest stopped
// consuming at all" without ever waiting on the guest.
uint64_t g_swaps = 0;

// Size of the surface the guest actually presents, pushed in by the GL side on
// every swap (egl_sdl.cpp). Logged next to conversion cost so an oversized
// offline encode is obvious; the bridge no longer shrinks frames to this size.
int g_display_w = 0;
int g_display_h = 0;

int64_t monotonic_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// A frame may always be published if the previous one has been uploaded. When it
// has not, publishing is throttled to this interval instead of being refused
// outright - see the comment in submit_i420().
constexpr int64_t PUBLISH_STALL_US = 30000;
int64_t g_last_publish_us = 0;

// Frame hand-off. The producer (media thread) converts into g_scratch and swaps
// it into g_pending; the consumer (GL thread) uploads g_pending and records the
// serial it consumed. g_scratch is producer-only, so the two never alias.
std::vector<uint8_t> g_scratch;
std::vector<uint8_t> g_pending;
uint64_t g_pending_serial = 0;
uint64_t g_uploaded_serial = 0;
int g_pending_width = 0;
int g_pending_height = 0;

unsigned g_backing_texture = 0;

uint64_t g_submitted = 0;
uint64_t g_uploaded = 0;
uint64_t g_dropped = 0;
uint64_t g_callbacks = 0;
// Conversion cost, so the log says where the CPU goes instead of guessing.
uint64_t g_convert_us = 0;
uint64_t g_convert_samples = 0;
// Guest-side step cost and publish pacing - see invoke_listener()/submit_i420().
uint64_t g_step_us = 0;
uint64_t g_step_max_us = 0;
uint64_t g_step_samples = 0;

bool g_flip = true;
bool g_flip_read = false;

bool flip_enabled()
{
    if (!g_flip_read) {
        g_flip = config["video"]["flip"].value_or<bool>(true);
        g_flip_read = true;
        BD_LOG("VIDEO", "video.flip = %d", (int)g_flip);
    }
    return g_flip;
}

// Diagnostic probe: replaces the decoded frame with a pattern that can be read
// back off a screenshot and says, in one glance, exactly which part of the video
// frame the guest's quad samples:
//
//   * a 16x16 grid of uniform cells, R = column * 255/15, G = row * 255/15, so
//     counting the R steps across the screen gives the u range and the G steps
//     the v range. A quad that samples the whole texture shows 16 steps from 0
//     to 255; one that only shows the bottom-left corner stops early (the
//     original "only the bottom-left of the video is visible" report read out as
//     u in 0..0.36, v in 0..0.36 with the old smooth ramp).
//   * B = 255 on the cell diagonal, so the image orientation (and therefore
//     video.flip) is unambiguous - the diagonal must run from one corner to the
//     other, never the mirror.
//   * a white border a few percent thick around the texture edge, so "does the
//     quad reach the texture's edge at all" is answered by the screenshot rather
//     than by guessing from a smooth ramp, which looks identical either way.
//
// Gated by BD_VIDEO_DEBUG_GRADIENT=1 (the name is historical; the pattern is no
// longer a gradient).
bool gradient_probe_enabled()
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* value = getenv("BD_VIDEO_DEBUG_GRADIENT");
        enabled = value && *value && strcmp(value, "0") != 0;
    }
    return enabled == 1;
}

void fill_gradient(uint8_t* rgba, int width, int height)
{
    const int cell = 16;
    int border = width < height ? width / 40 : height / 40;
    if (border < 2)
        border = 2;
    for (int y = 0; y < height; y++) {
        const int cell_y = y * cell / height;
        for (int x = 0; x < width; x++) {
            const int cell_x = x * cell / width;
            uint8_t* pixel = rgba + ((size_t)y * width + (size_t)x) * 4u;
            const bool edge = x < border || y < border || x >= width - border ||
                              y >= height - border;
            if (edge) {
                pixel[0] = 255;
                pixel[1] = 255;
                pixel[2] = 255;
            } else {
                pixel[0] = (uint8_t)(cell_x * 255 / (cell - 1));
                pixel[1] = (uint8_t)(cell_y * 255 / (cell - 1));
                pixel[2] = (cell_x == cell_y) ? 255 : 0;
            }
            pixel[3] = 255;
        }
    }
}

// I420 -> tightly packed RGBA, BT.601 limited range, integer math.
//
// Deliberately not libswscale: that library is built differently per platform
// and refuses conversions it has no converter for. Its own arm64 build on the
// handheld accepts yuv420p->rgba, while the Ubuntu 20.04 build in the container
// rejects it outright:
//   [swscaler] No accelerated colorspace conversion found from yuv420p to rgba.
//   [BD-VIDEO] sws_scale failed for 1280x720
// and on the handheld the same library segfaulted on a negative source stride.
// A 30-line converter has none of that variance, and it can flip vertically for
// free (video frames arrive bottom-up relative to GL's texture origin), which
// removes the in-place plane flip the swscale path needed.
void i420_to_rgba(const uint8_t* y_plane, const uint8_t* u_plane,
                  const uint8_t* v_plane, int width, int height, uint8_t* dst,
                  bool flip)
{
    const int half_width = width / 2;
    for (int y = 0; y < height; y++) {
        const uint8_t* src_y = y_plane + (size_t)(flip ? height - 1 - y : y) * width;
        const uint8_t* src_u = u_plane + (size_t)(flip ? height - 1 - y : y) / 2 * half_width;
        const uint8_t* src_v = v_plane + (size_t)(flip ? height - 1 - y : y) / 2 * half_width;
        uint8_t* out = dst + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            const int c = src_y[x] - 16;
            const int d = src_u[x / 2] - 128;
            const int e = src_v[x / 2] - 128;
            const int luma = 298 * c;
            int r = (luma + 409 * e + 128) >> 8;
            int g = (luma - 100 * d - 208 * e + 128) >> 8;
            int b = (luma + 516 * d + 128) >> 8;
            out[x * 4 + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            out[x * 4 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            out[x * 4 + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
            out[x * 4 + 3] = 255;
        }
    }
}

// libswscale's unscaled fast path does not honour a negative source stride: it
// still walks `src + stride * row` and runs off the start of the plane (SIGSEGV
// in libswscale.so). i420_to_rgba() flips while reading instead, so no plane
// manipulation happens here at all.

void invoke_listener()
{
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        notify = g_notify;
    }
    if (!notify)
        return;
    ++g_callbacks;
    if (g_callbacks <= 3 || (g_callbacks % 300) == 0)
        BD_LOG("VIDEO", "onFrameAvailable #%llu (texture=%d) [render thread]",
               (unsigned long long)g_callbacks, g_active_texture);
    // Outside the lock: the guest handler re-enters the loader (updateTexImage ->
    // our upload hook -> glBindTexture/glTexImage2D) and must not see a held
    // frame lock.
    //
    // This call is the guest's whole per-frame video step (Java listener ->
    // updateTexImage -> upload -> Unity's blit), and it runs on the render
    // thread, so its duration is also the guest's video step cost. Time it:
    // "the video is laggy" is either this being slow or the guest simply not
    // asking for frames, and the two need different fixes.
    const int64_t start = monotonic_us();
    notify();
    const int64_t cost = monotonic_us() - start;
    g_step_us += (uint64_t)cost;
    if (cost > (int64_t)g_step_max_us)
        g_step_max_us = (uint64_t)cost;
    if (++g_step_samples % 30 == 0) {
        BD_LOG("VIDEO",
               "guest video step: %.1f ms avg, %.1f ms max over %llu callbacks",
               (double)g_step_us / (double)g_step_samples / 1000.0,
               (double)g_step_max_us / 1000.0,
               (unsigned long long)g_step_samples);
        g_step_us = 0;
        g_step_max_us = 0;
        g_step_samples = 0;
    }
}

} // namespace

namespace bd_video {

void pump()
{
    // Render-thread heartbeat. See PUBLISH_STALL_US: the producer does not wait
    // on the guest, it just stops converting frames once the guest has clearly
    // stopped consuming them.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_swaps;
    }

    // Every 2 s: how often the guest actually presents, next to the producer's
    // counters. "The video is laggy" is almost never the decoder - it is the
    // guest calling updateTexImage() (and therefore taking a new frame) a
    // handful of times per second while FFmpeg runs at 30. Without a rate on
    // this line the two look identical in the log.
    {
        static int64_t last_report_us = 0;
        const int64_t now = monotonic_us();
        if (last_report_us == 0)
            last_report_us = now;
        if (now - last_report_us >= 2000000) {
            last_report_us = now;
            if (bd_video::video_texture_name() >= 0)
                log_state("swap");
        }
    }

    // Periodic codec state dump (thunks/ndk/media.cpp registers live codecs).
    static uint64_t pumps = 0;
    if ((++pumps % 90) == 0) {
        ::bd_media_dump_state();
    }

    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ready = g_frame_ready;
        g_frame_ready = false;
    }
    if (!ready)
        return;
    invoke_listener();
}

void surface_texture_created(int texture_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_textures.insert((unsigned)texture_id);
    g_active_texture = texture_id;
    BD_LOG("VIDEO", "SurfaceTexture(texture=%d) registered (sinks=%zu)",
           texture_id, g_textures.size());
}

void register_sink(int texture_id, std::function<void()> notify)
{
    bool ready = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_textures.insert((unsigned)texture_id);
        g_active_texture = texture_id;
        g_notify = std::move(notify);
        ready = (bool)g_notify;
    }
    BD_LOG("VIDEO", "frame sink ready: texture=%d notify=%s", texture_id,
           ready ? "yes" : "no");
}

void clear_sink(int texture_id)
{
    bool was = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_active_texture == texture_id && g_notify) {
            g_notify = nullptr;
            was = true;
            g_frame_ready = false;
        }
    }
    if (was)
        BD_LOG("VIDEO", "frame sink cleared: texture=%d (listener gone)", texture_id);
}

void surface_texture_buffer_size(int texture_id, int width, int height)
{
    BD_LOG("VIDEO", "setDefaultBufferSize(%d, %d) texture=%d",
           width, height, texture_id);
}

void surface_texture_attached(int texture_id)
{
    BD_LOG("VIDEO", "Surface attached to texture=%d (codec output Surface)",
           texture_id);
}

void surface_texture_released(int texture_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_textures.erase((unsigned)texture_id);
    if (g_active_texture == texture_id) {
        g_active_texture = -1;
        g_notify = nullptr;
    }
    BD_LOG("VIDEO", "SurfaceTexture(%d) released (%zu sinks left)",
           texture_id, g_textures.size());
}

void surface_texture_update_tex_image(int texture_id)
{
    // This is where a real SurfaceTexture picks up the newest buffer from its
    // BufferQueue and binds it to the texture (glEGLImageTargetTexture2DOES).
    // It runs on the guest render thread, so the GL context is current - and it
    // is the only reliable per-frame moment to push our decoded frame into the
    // video texture, because Unity keeps its external-texture binding
    // persistent and does not re-bind every frame.
    std::function<void()> hook;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        hook = g_upload_hook;
    }
    if (hook)
        hook();

    static uint64_t calls = 0;
    ++calls;
    if (calls <= 3 || (calls % 600) == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        BD_LOG("VIDEO",
               "updateTexImage #%llu texture=%d frames=%llu/%llu drop=%llu cb=%llu",
               (unsigned long long)calls, texture_id,
               (unsigned long long)g_submitted, (unsigned long long)g_uploaded,
               (unsigned long long)g_dropped, (unsigned long long)g_callbacks);
    }
}

// One-shot dump of the RGBA frame the bridge is about to publish, so the
// conversion can be inspected independently of whatever the GPU does with it.
// Gated by BD_VIDEO_DUMP_FRAME=<path prefix> + BD_VIDEO_DUMP_AT=<frame index>.
void dump_rgba_frame(const uint8_t* rgba, int width, int height, uint64_t index)
{
    const char* prefix = getenv("BD_VIDEO_DUMP_FRAME");
    if (!prefix || !*prefix || index == 0)
        return;
    const char* at_env = getenv("BD_VIDEO_DUMP_AT");
    uint64_t at = at_env && *at_env ? (uint64_t)strtoull(at_env, nullptr, 10) : 30;
    if (at == 0)
        at = 30;
    if (index != at)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s.rgba.%llu.ppm", prefix,
             (unsigned long long)index);
    FILE* file = fopen(path, "wb");
    if (!file) {
        BD_LOG("VIDEO", "rgba dump open %s failed errno=%d", path, errno);
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::vector<uint8_t> row((size_t)width * 3u);
    for (int y = 0; y < height; y++) {
        const uint8_t* src = rgba + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            row[(size_t)x * 3u + 0] = src[x * 4 + 0];
            row[(size_t)x * 3u + 1] = src[x * 4 + 1];
            row[(size_t)x * 3u + 2] = src[x * 4 + 2];
        }
        fwrite(row.data(), 1, row.size(), file);
    }
    fclose(file);
    BD_LOG("VIDEO", "rgba dump frame #%llu -> %s (%dx%d)",
           (unsigned long long)index, path, width, height);
}

// Publishes are the only frames the guest can ever see, so the interval between
// them is the video's real frame rate - and "submits per publish" is how many
// decoded frames were thrown away to hold that rate. A big ratio means FFmpeg is
// running far ahead of the guest, i.e. the CPU is being spent on frames that are
// discarded (and taken away from the audio thread).
static void report_publish()
{
    static int64_t previous_us = 0;
    static uint64_t published = 0;
    static uint64_t submits = 0;
    const int64_t now = monotonic_us();
    ++submits;
    if (previous_us == 0) {
        previous_us = now;
        return;
    }
    ++published;
    if (published % 30 != 0)
        return;
    const double fps = now > previous_us
        ? (double)published * 1000000.0 / (double)(now - previous_us)
        : 0.0;
    BD_LOG("VIDEO",
           "publish: %.1f fps over %llu frames, %.1f submits per publish",
           fps, (unsigned long long)published,
           (double)submits / (double)published);
    previous_us = now;
    published = 0;
    submits = 0;
}

bool submit_i420(const uint8_t* packed, size_t size, int width, int height,
                 int64_t pts_us)
{
    if (!packed || width <= 0 || height <= 0 || (width & 1) || (height & 1))
        return false;
    // Guard the plane offsets below: a cropped/odd frame that does not match the
    // packed I420 size would make the flip and conversion walk off the buffer.
    const size_t expected = (size_t)width * (size_t)height * 3 / 2;
    if (size < expected) {
        BD_LOG("VIDEO", "I420 %dx%d needs %zu bytes, got %zu - dropped",
               width, height, expected, size);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_textures.empty() || !g_notify) {
            ++g_dropped;
            if (g_dropped <= 3)
                BD_LOG("VIDEO",
                       "I420 %dx%d pts=%lld dropped: no SurfaceTexture sink",
                       width, height, (long long)pts_us);
            return false;
        }
        // Rate limit, NOT a flow-control gate.
        //
        // The pending frame is only consumed when the guest calls
        // updateTexImage(), and that happens at render rate. Converting every
        // decoded frame regardless burns a core of CPU on 720p RGBA and starves
        // the audio thread - the audio used to stutter because of exactly that.
        //
        // So when the previous frame has not been uploaded yet, skip this one -
        // but only while the last publish is recent, and never indefinitely. A
        // hard `if (unconsumed) drop` latches: no publish -> no callback -> the
        // guest, which consumes in response to the callback, never consumes ->
        // no publish. That is what left the handheld with a frozen INTRO video
        // and a counter stuck at 91/90 for the rest of the run. With the
        // deadline below, publishing resumes on its own even if the guest goes
        // quiet, and the guest only ever sees the newest frame.
        const int64_t now = monotonic_us();
        if (g_pending_serial != g_uploaded_serial &&
            g_last_publish_us != 0 &&
            now - g_last_publish_us < PUBLISH_STALL_US) {
            ++g_dropped;
            if (g_dropped <= 5 || (g_dropped % 300) == 0)
                BD_LOG("VIDEO",
                       "I420 %dx%d pts=%lld dropped: renderer behind (%llu unconsumed), drop=%llu",
                       width, height, (long long)pts_us,
                       (unsigned long long)(g_pending_serial - g_uploaded_serial),
                       (unsigned long long)g_dropped);
            // The pixels are skipped, the NOTIFICATION is not. A real
            // BufferQueue still fires onFrameAvailable for this slot; the
            // consumer just reads the newest image, i.e. the one already in
            // g_pending. Unity counts those notifications to advance its own
            // surface frame index (AndroidVideoMedia::UpdateSurface), and a
            // swallowed one makes the index drift until its blit refuses to run
            // at all (AndroidVideoMedia::VideoDecoder::Blit, "too much behind").
            g_frame_ready = true;
            return true;
        }
        // Remember the deadline even when the guest keeps up, so a stall is
        // measured from the last frame the guest actually saw.
        g_last_publish_us = now;
    }

    std::lock_guard<std::mutex> produce_lock(g_produce_mutex);

    // Upload at the source resolution. Resolution and bitrate belong in the
    // offline encode before the APK is built — the loader used to box-downscale
    // to ~drawable size (e.g. 1280x720 -> 640x360 on a 640x480 panel), which
    // blurred cover-fit video and hid oversized assets. Keep convert/publish
    // timing so a heavy encode still shows up in the log.
    const int out_width = width;
    const int out_height = height;
    const size_t rgba_size = (size_t)out_width * (size_t)out_height * 4;
    if (g_scratch.size() != rgba_size)
        g_scratch.assign(rgba_size, 0);

    // Brightness of the source luma plane, sampled sparsely. A video that is
    // genuinely black (fade-in, title card) and one that decoded to black look
    // identical on screen, so record which one this is.
    {
        const size_t y_plane = (size_t)width * (size_t)height;
        uint64_t sum = 0;
        size_t samples = 0;
        for (size_t i = 0; i < y_plane; i += 128) {
            sum += packed[i];
            ++samples;
        }
        static uint64_t logged = 0;
        if (++logged <= 3 || (logged % 60) == 0)
            BD_LOG("VIDEO", "frame #%llu mean luma = %llu (samples=%zu)",
                   (unsigned long long)g_submitted + 1,
                   (unsigned long long)(samples ? sum / samples : 0), samples);
    }

    {
        static bool oversized_warned = false;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!oversized_warned && g_display_w > 0 && g_display_h > 0 &&
            (width > g_display_w * 5 / 4 || height > g_display_h * 5 / 4)) {
            BD_LOG("VIDEO",
                   "source %dx%d is larger than drawable %dx%d — convert+upload "
                   "pay full cost; shrink the MP4 offline before the APK build",
                   width, height, g_display_w, g_display_h);
            oversized_warned = true;
        }
    }

    const int64_t convert_start = monotonic_us();
    const size_t plane_y = (size_t)width * (size_t)height;
    const size_t plane_uv = plane_y / 4;
    if (gradient_probe_enabled())
        fill_gradient(g_scratch.data(), out_width, out_height);
    else
        i420_to_rgba(packed, packed + plane_y, packed + plane_y + plane_uv,
                     width, height, g_scratch.data(), flip_enabled());
    const int64_t convert_us = monotonic_us() - convert_start;
    dump_rgba_frame(g_scratch.data(), out_width, out_height, g_submitted + 1);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_submitted;
        g_pending.swap(g_scratch);
        g_pending_width = out_width;
        g_pending_height = out_height;
        ++g_pending_serial;
        g_frame_ready = true;
        report_publish();
        // Where the CPU actually goes on the handheld: decoding is FFmpeg's
        // business and invisible here, but a conversion that costs more than a
        // frame interval means the offline encode is too large for this panel.
        g_convert_us += (uint64_t)convert_us;
        if (++g_convert_samples % 60 == 0) {
            static uint64_t previous_total = 0;
            static uint64_t previous_samples = 0;
            const uint64_t frames = g_convert_samples - previous_samples;
            const uint64_t total = g_convert_us - previous_total;
            BD_LOG("VIDEO",
                   "convert: %.2f ms/frame avg over %llu frames (%dx%d RGBA)",
                   frames ? (double)total / (double)frames / 1000.0 : 0.0,
                   (unsigned long long)frames, width, height);
            previous_total = g_convert_us;
            previous_samples = g_convert_samples;
        }
        // Which thread pays for this is the whole question: if submit_i420() runs
        // on the guest's own update thread (Unity calls
        // AMediaCodec_releaseOutputBuffer from AndroidVideoMedia::UpdateTexture),
        // then this convert is stolen from the game's frame budget and must
        // move to a worker. Log the tid next to the cost so that is a fact, not
        // an assumption.
        if (g_submitted <= 3 || (g_submitted % 300) == 0)
            BD_LOG("VIDEO",
                   "frame #%llu I420 %dx%d pts=%lld -> RGBA %dx%d (%lld us, tid=%d), uploads=%llu",
                   (unsigned long long)g_submitted, width, height,
                   (long long)pts_us, out_width, out_height,
                   (long long)convert_us, (int)syscall(__NR_gettid),
                   (unsigned long long)g_uploaded);
    }

    // Do not call the listener here - see g_frame_ready. The render thread picks
    // the frame up from pump().
    return true;
}

bool has_sink()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_textures.empty() && (bool)g_notify;
}

int video_texture_name()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_active_texture;
}

void set_upload_hook(UploadHook hook)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_upload_hook = hook;
    BD_LOG("VIDEO", "upload hook installed (%p)", (void*)hook);
}

unsigned backing_texture()
{
    return g_backing_texture;
}

void set_backing_texture(unsigned texture_name)
{
    g_backing_texture = texture_name;
    BD_LOG("VIDEO", "backing GL_TEXTURE_2D for video = %u", texture_name);
}

const uint8_t* begin_upload(int* width, int* height, uint64_t* serial)
{
    g_mutex.lock();
    if (g_pending.empty() || g_pending_serial == g_uploaded_serial ||
        g_pending_width <= 0 || g_pending_height <= 0) {
        g_mutex.unlock();
        return nullptr;
    }
    if (width) *width = g_pending_width;
    if (height) *height = g_pending_height;
    if (serial) *serial = g_pending_serial;
    return g_pending.data();
}

void end_upload()
{
    g_uploaded_serial = g_pending_serial;
    ++g_uploaded;
    g_mutex.unlock();
}

void log_state(const char* where)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    BD_LOG("VIDEO",
           "%s: sinks=%zu active=%d pending=%llu uploaded=%llu frames=%llu/%llu drop=%llu cb=%llu swaps=%llu display=%dx%d",
           where ? where : "state", g_textures.size(), g_active_texture,
           (unsigned long long)g_pending_serial,
           (unsigned long long)g_uploaded_serial,
           (unsigned long long)g_submitted, (unsigned long long)g_uploaded,
           (unsigned long long)g_dropped, (unsigned long long)g_callbacks,
           (unsigned long long)g_swaps, g_display_w, g_display_h);
}

// Called by the GL side (egl_sdl.cpp) on every swap, before pump(): recorded for
// diagnostics (oversized-source warning, log_state) — not used to resize frames.
void set_display_size(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_display_w == width && g_display_h == height)
        return;
    g_display_w = width;
    g_display_h = height;
    BD_LOG("VIDEO", "drawable size %dx%d (frames upload at source resolution)",
           width, height);
}

int display_width()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_display_w;
}

int display_height()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_display_h;
}

} // namespace bd_video
