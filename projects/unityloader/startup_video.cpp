#include "startup_video.h"
#include "logging.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#include "ffmpeg_optional.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <vector>

struct StartupVideo::State {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* selected = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int stream = -1;
    int width = 0, height = 0;
    bool draining = false, pending = false, failed = false;
    double interval = 1000.0 / 24, duration = 0, origin = 0;
    double last_pts = -1, pending_pts = 0, loop_base = 0;
    unsigned frames = 0, loops = 0;
    std::vector<uint8_t> pixels;

    ~State() {
        if (sws) sws_freeContext(sws);
        if (packet) av_packet_free(&packet);
        if (frame) av_frame_free(&frame);
        if (selected) av_frame_free(&selected);
        if (codec) avcodec_free_context(&codec);
        if (format) avformat_close_input(&format);
    }

    // Drain all delayed frames before seeking, including codecs with B frames.
    bool next() {
        for (int attempts = 0; attempts < 4096; ++attempts) {
            int result = avcodec_receive_frame(codec, frame);
            if (result == 0) {
                double pts = last_pts < 0 ? 0 : last_pts + interval;
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    const AVRational tb = format->streams[stream]->time_base;
                    pts = frame->best_effort_timestamp * 1000.0 * tb.num / tb.den - origin;
                }
                last_pts = std::max(0.0, pts);
                pending_pts = loop_base + last_pts;
                pending = true;
                return true;
            }
            if (result == AVERROR_EOF) {
                const double length = std::max(duration, last_pts + interval);
                if (last_pts < 0 || length <= 0 ||
                    av_seek_frame(format, stream,
                        format->streams[stream]->start_time == AV_NOPTS_VALUE ? 0 :
                            format->streams[stream]->start_time, AVSEEK_FLAG_BACKWARD) < 0)
                    break;
                avcodec_flush_buffers(codec);
                loop_base += length;
                draining = false;
                last_pts = -1;
                ++loops;
                BD_LOG("SPLASH", "video loop=%u at=%.0fms", loops, loop_base);
                continue;
            }
            if (result != AVERROR(EAGAIN) || draining) break;
            result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                result = avcodec_send_packet(codec, nullptr);
                draining = result >= 0;
                if (!draining) break;
                continue;
            }
            if (result < 0) break;
            if (packet->stream_index == stream) {
                result = avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                // receive_frame was EAGAIN, so the codec must accept input.
                if (result < 0) break;
            } else {
                av_packet_unref(packet);
            }
        }
        failed = true;
        BD_LOG("SPLASH", "video decode stopped after %u frames; retaining last picture", frames);
        return false;
    }

    bool convert(const AVFrame* source) {
        if (source->width != width || source->height != height) return false;
        sws = sws_getCachedContext(sws, width, height,
            static_cast<AVPixelFormat>(source->format), width, height,
            AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) return false;
        uint8_t* dst[4] = {pixels.data(), nullptr, nullptr, nullptr};
        int stride[4] = {width * 4, 0, 0, 0};
        return sws_scale(sws, source->data, source->linesize, 0, height, dst, stride) == height;
    }
};

StartupVideo::StartupVideo() = default;
StartupVideo::~StartupVideo() {
    if (!state) return;
    const unsigned frames = state->frames;
    state.reset();
    BD_LOG("SPLASH", "video resources released after %u frames (decoder, buffers, file)", frames);
}

bool StartupVideo::open(const char* path) {
    state.reset();
    if (!bd_ffmpeg::ensure_loaded()) {
        BD_LOG("SPLASH", "FFmpeg runtime unavailable; continuing game startup");
        return false;
    }
    auto s = std::make_unique<State>();
    if (avformat_open_input(&s->format, path, nullptr, nullptr) < 0 ||
        avformat_find_stream_info(s->format, nullptr) < 0) return false;
    s->stream = av_find_best_stream(s->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s->stream < 0) return false;
    AVStream* stream = s->format->streams[s->stream];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) return false;
    s->codec = avcodec_alloc_context3(decoder);
    if (!s->codec || avcodec_parameters_to_context(s->codec, stream->codecpar) < 0)
        return false;
    // Leave CPU capacity for Unity's simultaneous initialization and avoid
    // automatic frame threading accumulating a large startup delay.
    s->codec->thread_count = 2;
    s->codec->thread_type = FF_THREAD_SLICE;
    if (avcodec_open2(s->codec, decoder, nullptr) < 0) return false;
    s->width = s->codec->width;
    s->height = s->codec->height;
    if (s->width <= 0 || s->height <= 0 || s->width > 4096 || s->height > 4096)
        return false;
    s->frame = av_frame_alloc();
    s->selected = av_frame_alloc();
    s->packet = av_packet_alloc();
    if (!s->frame || !s->selected || !s->packet) return false;
    s->pixels.resize(static_cast<size_t>(s->width) * s->height * 4);
    const AVRational rate = av_guess_frame_rate(s->format, stream, nullptr);
    if (rate.num > 0 && rate.den > 0) s->interval = 1000.0 * rate.den / rate.num;
    if (stream->duration != AV_NOPTS_VALUE)
        s->duration = stream->duration * 1000.0 * stream->time_base.num / stream->time_base.den;
    if (stream->start_time != AV_NOPTS_VALUE)
        s->origin = stream->start_time * 1000.0 * stream->time_base.num / stream->time_base.den;
    if (!s->next() || !s->convert(s->frame)) return false;
    s->pending = false;
    ++s->frames;
    BD_LOG("SPLASH", "video opened %s codec=%s %dx%d interval=%.3fms duration=%.0fms silent=1",
           path, decoder->name, s->width, s->height, s->interval, s->duration);
    state = std::move(s);
    return true;
}

bool StartupVideo::update(uint32_t elapsed_ms) {
    if (!state || state->failed) return false;
    bool changed = false;
    // Bound catch-up work so scene handoff never waits for a whole video.
    for (int i = 0; i < 8; ++i) {
        if (!state->pending && !state->next()) break;
        if (state->pending_pts > elapsed_ms) break;
        // Keep the latest due frame while decoding ahead into a different
        // AVFrame. Convert only the picture we will actually upload, rather
        // than every skipped frame when startup CPU contention makes us late.
        std::swap(state->frame, state->selected);
        av_frame_unref(state->frame);
        state->pending = false;
        ++state->frames;
        changed = true;
    }
    if (changed && !state->convert(state->selected)) {
        state->failed = true;
        return false;
    }
    return changed;
}
int StartupVideo::width() const { return state ? state->width : 0; }
int StartupVideo::height() const { return state ? state->height : 0; }
const uint8_t* StartupVideo::pixels() const { return state ? state->pixels.data() : nullptr; }
