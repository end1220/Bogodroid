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

namespace {
const char* ff_error_text(int code, char (&buffer)[AV_ERROR_MAX_STRING_SIZE])
{
    buffer[0] = '\0';
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}
}

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
                if (last_pts < 0 || length <= 0) break;
                const int64_t seek_target =
                    format->streams[stream]->start_time == AV_NOPTS_VALUE ? 0 :
                        format->streams[stream]->start_time;
                const int seek_result = av_seek_frame(format, stream, seek_target,
                                                       AVSEEK_FLAG_BACKWARD);
                if (seek_result < 0) {
                    char error[AV_ERROR_MAX_STRING_SIZE];
                    BD_LOG("SPLASH", "video seek failed rc=%d (%s) stream=%d target=%lld",
                           seek_result, ff_error_text(seek_result, error), stream,
                           static_cast<long long>(seek_target));
                    break;
                }
                avcodec_flush_buffers(codec);
                loop_base += length;
                draining = false;
                last_pts = -1;
                ++loops;
                BD_LOG("SPLASH", "video loop=%u at=%.0fms", loops, loop_base);
                continue;
            }
            if (result != AVERROR(EAGAIN) || draining) {
                char error[AV_ERROR_MAX_STRING_SIZE];
                BD_LOG("SPLASH", "video receive_frame failed rc=%d (%s) draining=%d attempts=%d",
                       result, ff_error_text(result, error), draining ? 1 : 0, attempts);
                break;
            }
            result = av_read_frame(format, packet);
            if (result == AVERROR_EOF) {
                result = avcodec_send_packet(codec, nullptr);
                draining = result >= 0;
                if (!draining) {
                    char error[AV_ERROR_MAX_STRING_SIZE];
                    BD_LOG("SPLASH", "video drain send failed rc=%d (%s)",
                           result, ff_error_text(result, error));
                    break;
                }
                continue;
            }
            if (result < 0) {
                char error[AV_ERROR_MAX_STRING_SIZE];
                BD_LOG("SPLASH", "video read_frame failed rc=%d (%s)",
                       result, ff_error_text(result, error));
                break;
            }
            if (packet->stream_index == stream) {
                result = avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                // receive_frame was EAGAIN, so the codec must accept input.
                if (result < 0) {
                    char error[AV_ERROR_MAX_STRING_SIZE];
                    BD_LOG("SPLASH", "video send_packet failed rc=%d (%s)",
                           result, ff_error_text(result, error));
                    break;
                }
            } else {
                av_packet_unref(packet);
            }
        }
        failed = true;
        BD_LOG("SPLASH", "video decode stopped after %u frames; retaining last picture",
               frames);
        return false;
    }

    bool convert(const AVFrame* source) {
        if (source->width != width || source->height != height) {
            BD_LOG("SPLASH", "video frame dimensions mismatch frame=%dx%d codec=%dx%d format=%d",
                   source->width, source->height, width, height, source->format);
            return false;
        }
        sws = sws_getCachedContext(sws, width, height,
            static_cast<AVPixelFormat>(source->format), width, height,
            AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            BD_LOG("SPLASH", "video sws_getCachedContext failed %dx%d format=%d -> RGB24",
                   width, height, source->format);
            return false;
        }
        uint8_t* dst[4] = {pixels.data(), nullptr, nullptr, nullptr};
        int stride[4] = {width * 3, 0, 0, 0};
        const int rows = sws_scale(sws, source->data, source->linesize,
                                   0, height, dst, stride);
        if (rows != height) {
            BD_LOG("SPLASH", "video sws_scale returned %d rows expected=%d format=%d linesize=%d,%d,%d,%d",
                   rows, height, source->format, source->linesize[0],
                   source->linesize[1], source->linesize[2], source->linesize[3]);
            return false;
        }
        return true;
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
    int result = avformat_open_input(&s->format, path, nullptr, nullptr);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        BD_LOG("SPLASH", "video avformat_open_input failed path=%s rc=%d (%s)",
               path, result, ff_error_text(result, error));
        return false;
    }
    result = avformat_find_stream_info(s->format, nullptr);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        BD_LOG("SPLASH", "video avformat_find_stream_info failed path=%s rc=%d (%s)",
               path, result, ff_error_text(result, error));
        return false;
    }
    s->stream = av_find_best_stream(s->format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s->stream < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        BD_LOG("SPLASH", "video stream selection failed rc=%d (%s) streams=%u",
               s->stream, ff_error_text(s->stream, error), s->format->nb_streams);
        return false;
    }
    AVStream* stream = s->format->streams[s->stream];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        BD_LOG("SPLASH", "video decoder unavailable codec_id=%d stream=%d",
               stream->codecpar->codec_id, s->stream);
        return false;
    }
    s->codec = avcodec_alloc_context3(decoder);
    if (!s->codec) {
        BD_LOG("SPLASH", "video decoder context allocation failed codec=%s", decoder->name);
        return false;
    }
    result = avcodec_parameters_to_context(s->codec, stream->codecpar);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        BD_LOG("SPLASH", "video parameters_to_context failed rc=%d (%s) codec=%s",
               result, ff_error_text(result, error), decoder->name);
        return false;
    }
    // Leave CPU capacity for Unity's simultaneous initialization and avoid
    // automatic frame threading accumulating a large startup delay.
    s->codec->thread_count = 2;
    s->codec->thread_type = FF_THREAD_SLICE;
    result = avcodec_open2(s->codec, decoder, nullptr);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        BD_LOG("SPLASH", "video avcodec_open2 failed rc=%d (%s) codec=%s %dx%d pix_fmt=%d profile=%d level=%d",
               result, ff_error_text(result, error), decoder->name,
               s->codec->width, s->codec->height, s->codec->pix_fmt,
               s->codec->profile, s->codec->level);
        return false;
    }
    s->width = s->codec->width;
    s->height = s->codec->height;
    if (s->width <= 0 || s->height <= 0 || s->width > 4096 || s->height > 4096) {
        BD_LOG("SPLASH", "video invalid dimensions %dx%d codec=%s",
               s->width, s->height, decoder->name);
        return false;
    }
    s->frame = av_frame_alloc();
    s->selected = av_frame_alloc();
    s->packet = av_packet_alloc();
    if (!s->frame || !s->selected || !s->packet) {
        BD_LOG("SPLASH", "video frame/packet allocation failed frame=%p selected=%p packet=%p",
               s->frame, s->selected, s->packet);
        return false;
    }
    // FFmpeg 4.2 swscale returns no rows for YUV420P -> RGBA on TrimUI.
    // RGB24 works and the splash does not use per-pixel alpha.
    s->pixels.resize(static_cast<size_t>(s->width) * s->height * 3);
    const AVRational rate = av_guess_frame_rate(s->format, stream, nullptr);
    if (rate.num > 0 && rate.den > 0) s->interval = 1000.0 * rate.den / rate.num;
    if (stream->duration != AV_NOPTS_VALUE)
        s->duration = stream->duration * 1000.0 * stream->time_base.num / stream->time_base.den;
    if (stream->start_time != AV_NOPTS_VALUE)
        s->origin = stream->start_time * 1000.0 * stream->time_base.num / stream->time_base.den;
    if (!s->next()) {
        BD_LOG("SPLASH", "video first frame decode failed codec=%s %dx%d pix_fmt=%d profile=%d level=%d",
               decoder->name, s->width, s->height, s->codec->pix_fmt,
               s->codec->profile, s->codec->level);
        return false;
    }
    if (!s->convert(s->frame)) {
        BD_LOG("SPLASH", "video first frame conversion failed codec=%s %dx%d frame_fmt=%d codec_fmt=%d linesize=%d,%d,%d,%d",
               decoder->name, s->width, s->height, s->frame->format,
               s->codec->pix_fmt, s->frame->linesize[0], s->frame->linesize[1],
               s->frame->linesize[2], s->frame->linesize[3]);
        return false;
    }
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
