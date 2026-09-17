#include "media.h"
#include "bd_video.h"
#include "logging.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
constexpr media_status_t AMEDIA_OK = 0;
constexpr media_status_t AMEDIA_ERROR_UNKNOWN = -10000;
constexpr ssize_t AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1;
constexpr ssize_t AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2;
constexpr uint32_t AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4;
constexpr int32_t COLOR_FormatYUV420Planar = 19;
constexpr size_t INPUT_BUFFER_SIZE = 4 * 1024 * 1024;
// A real MediaCodec owns a small, fixed pool of input buffers, and handing one
// out is a promise that it will be given back. That pool - not a wall-clock
// guess and not the output queue - is what bounds how far the producer may run
// ahead of the consumer, and it is what keeps frame indices in lockstep with
// Unity's player clock (see the header comment on AMediaCodec). Four is the
// ACodec default for many devices.
constexpr size_t INPUT_BUFFER_COUNT = 4;
// Ceiling on the output queue, and it BLOCKS input rather than dropping frames:
// a decoded frame the guest has not dequeued yet is one it is still going to
// want, so it must never be thrown away.
//
// The depth is what a real Surface-backed codec gets for free: the decoder can
// only produce into the BufferQueue the Surface owns, which the display drains
// at the clip's own frame rate, so a hardware codec runs a few frames ahead at
// most. Leaving it at 32 frames (a full second of 30 fps video) bought nothing -
// Unity renders whichever frame its clock asks for either way - but it cost
// twice: ~1 s of latency behind the player clock, and a decoder that never
// idles, which is CPU stolen from the game's own renderer on a 4-core SoC.
// Unity's own late-frame tolerance is k_VideoPlaybackFrameOffsetTolerance = 5,
// so the queue only needs to cover that plus jitter.
constexpr size_t MAX_QUEUED_OUTPUTS = 6;
// If the input pool has been empty this long with nothing queued for the guest
// either, the packet/frame pairing has desynced (a packet that produced no
// frame). Give the oldest slot back instead of latching - see
// AMediaCodec::release_stalled_input_slot().
constexpr int64_t INPUT_POOL_STALL_US = 200000;

int64_t monotonic_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct IoState {
    void* userdata{};
    media_data_source_read_at read_at{};
    media_data_source_get_size get_size{};
    media_data_source_close close{};
    int fd{-1};
    int64_t base{};
    int64_t length{-1};
    int64_t position{};

    ~IoState() {
        if (fd >= 0)
            ::close(fd);
    }
};

struct CodecOutput {
    size_t index{};
    std::vector<uint8_t> bytes;
    int64_t pts{};
    uint32_t flags{};
    // Frame geometry of `bytes`. The decoder may hand back a cropped size that
    // differs from the codec context, and the video bridge needs the same
    // numbers the buffer was built with.
    int width{};
    int height{};
};

const char* mime_for_codec(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return "video/avc";
        case AV_CODEC_ID_HEVC: return "video/hevc";
        case AV_CODEC_ID_MPEG4: return "video/mp4v-es";
        case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
        case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
        case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
        case AV_CODEC_ID_MP3: return "audio/mpeg";
        case AV_CODEC_ID_VORBIS: return "audio/vorbis";
        case AV_CODEC_ID_OPUS: return "audio/opus";
        default: return "application/octet-stream";
    }
}

AVCodecID codec_for_mime(const char* mime) {
    if (!mime) return AV_CODEC_ID_NONE;
    if (!strcmp(mime, "video/avc")) return AV_CODEC_ID_H264;
    if (!strcmp(mime, "video/hevc")) return AV_CODEC_ID_HEVC;
    if (!strcmp(mime, "video/mp4v-es")) return AV_CODEC_ID_MPEG4;
    if (!strcmp(mime, "video/x-vnd.on2.vp8")) return AV_CODEC_ID_VP8;
    if (!strcmp(mime, "video/x-vnd.on2.vp9")) return AV_CODEC_ID_VP9;
    if (!strcmp(mime, "audio/mp4a-latm")) return AV_CODEC_ID_AAC;
    if (!strcmp(mime, "audio/mpeg")) return AV_CODEC_ID_MP3;
    if (!strcmp(mime, "audio/vorbis")) return AV_CODEC_ID_VORBIS;
    if (!strcmp(mime, "audio/opus")) return AV_CODEC_ID_OPUS;
    return AV_CODEC_ID_NONE;
}

int io_read(void* opaque, uint8_t* buffer, int size) {
    auto* io = static_cast<IoState*>(opaque);
    if (io->length >= 0 && io->position >= io->length)
        return AVERROR_EOF;
    size_t wanted = static_cast<size_t>(size);
    if (io->length >= 0)
        wanted = std::min<int64_t>(wanted, io->length - io->position);
    ssize_t got = -1;
    if (io->fd >= 0)
        got = pread(io->fd, buffer, wanted, io->base + io->position);
    else if (io->read_at)
        got = io->read_at(io->userdata, io->position, buffer, wanted);
    if (got <= 0)
        return got == 0 ? AVERROR_EOF : AVERROR(EIO);
    io->position += got;
    return static_cast<int>(got);
}

int64_t io_seek(void* opaque, int64_t offset, int whence) {
    auto* io = static_cast<IoState*>(opaque);
    int64_t length = io->length;
    if (length < 0 && io->get_size)
        length = io->get_size(io->userdata);
    if (whence == AVSEEK_SIZE)
        return length;
    int64_t next = offset;
    if ((whence & ~AVSEEK_FORCE) == SEEK_CUR)
        next += io->position;
    else if ((whence & ~AVSEEK_FORCE) == SEEK_END) {
        if (length < 0) return AVERROR(ENOSYS);
        next += length;
    }
    if (next < 0 || (length >= 0 && next > length))
        return AVERROR(EINVAL);
    io->position = next;
    return next;
}
} // namespace

struct AMediaDataSource {
    void* userdata{};
    media_data_source_read_at read_at{};
    media_data_source_get_size get_size{};
    media_data_source_close close{};
};

struct AMediaFormat {
    std::map<std::string, int32_t> ints;
    std::map<std::string, int64_t> longs;
    std::map<std::string, float> floats;
    std::map<std::string, std::string> strings;
    AVCodecParameters* codecpar{};

    ~AMediaFormat() {
        avcodec_parameters_free(&codecpar);
    }
};

struct AMediaExtractor {
    AVFormatContext* format{};
    AVIOContext* avio{};
    std::unique_ptr<IoState> io;
    AVPacket* packet{av_packet_alloc()};
    bool has_packet{};
    std::vector<bool> selected;

    ~AMediaExtractor() {
        av_packet_free(&packet);
        if (format)
            avformat_close_input(&format);
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }

    bool ensure_packet() {
        if (has_packet) return true;
        while (format && av_read_frame(format, packet) >= 0) {
            if (packet->stream_index >= 0 &&
                static_cast<size_t>(packet->stream_index) < selected.size() &&
                selected[packet->stream_index]) {
                has_packet = true;
                return true;
            }
            av_packet_unref(packet);
        }
        return false;
    }
};

struct AMediaCodec {
    AVCodecID codec_id{AV_CODEC_ID_NONE};    const AVCodec* decoder{};
    AVCodecContext* context{};
    SwsContext* sws{};
    SwrContext* swr{};
    AMediaFormat* output_format{};
    // Android's input-buffer contract, which is also the flow control for the
    // whole pipeline: the app dequeues one of INPUT_BUFFER_COUNT slots, fills
    // it, queues it, and only gets that slot back when the decoder has turned
    // the packet into a frame. Unity feeds input buffers as fast as the codec
    // accepts them, so without this pool it hands us an entire 40 s clip in the
    // first seconds of playback; the frames that pile up here are then useless
    // by the time the player clock reaches their PTS, and Unity's own frame
    // index bookkeeping (AndroidVideoMedia::ConsumeOutputBuffers) classifies
    // them as "too late" and refuses to render them into the Surface at all -
    // black video, healthy audio, no GL work involved. So: never drop a decoded
    // frame, stall the input side instead.
    std::vector<std::vector<uint8_t>> input =
        std::vector<std::vector<uint8_t>>(
            INPUT_BUFFER_COUNT, std::vector<uint8_t>(INPUT_BUFFER_SIZE));
    std::vector<bool> input_free =
        std::vector<bool>(INPUT_BUFFER_COUNT, true);
    std::deque<size_t> input_in_flight;   // in queue order == frame output order
    int64_t pool_empty_since_us{};
    size_t stalled_releases{};
    std::deque<CodecOutput> pending;
    std::map<size_t, CodecOutput> outstanding;
    std::mutex mutex;
    std::condition_variable ready;
    size_t next_output_index{};
    size_t decoded_frames{};
    bool format_changed{};
    bool started{};
    // Unity's AndroidVideoMedia attaches a Surface (from its SurfaceTexture) to
    // the decoder. The I420 we decode in its place has to be published to the
    // video texture by hand - see javastubs/bd_video.h.
    bool surface_mode{};
    size_t get_buffer_calls{};
    size_t render_calls{};
    size_t input_dequeues{};
    size_t input_try_again{};
    size_t output_dequeues{};
    size_t output_try_again{};
    size_t drain_calls{};
    // Where a video frame's time actually goes, measured on the device:
    // send/queue costs the FFmpeg bitstream work, drain() the reconstruction and
    // colour conversion into I420. Together with bd_video's convert/upload and
    // the guest's own video step these decide whether "stutter" is decode-bound
    // (needs the VPU, no amount of texture work helps) or bridge-bound.
    uint64_t send_us{};
    size_t send_samples{};
    uint64_t drain_us{};
    size_t drain_samples{};
    // High-water mark of the output queue. Anything much above MAX_QUEUED_OUTPUTS
    // means the guest is not dequeuing (its own renderer is stalled), which is
    // worth seeing in the state dump.
    size_t peak_pending{};
    int64_t last_output_pts{};
    int64_t last_input_pts{};

    // ---- input pool (call all of these with `mutex` held) ----------------
    size_t free_input_slots() const
    {
        size_t free_slots = 0;
        for (bool free : input_free)
            free_slots += free ? 1 : 0;
        return free_slots;
    }

    ssize_t take_input_slot()
    {
        for (size_t i = 0; i < input_free.size(); ++i) {
            if (input_free[i]) {
                input_free[i] = false;
                input_in_flight.push_back(i);
                pool_empty_since_us = free_input_slots() == 0 ? monotonic_us() : 0;
                return static_cast<ssize_t>(i);
            }
        }
        return -1;
    }

    // One frame came out of the decoder: the oldest packet in flight is done
    // with its buffer. Paired this way (not on queueInputBuffer) because FFmpeg
    // may still be reading the packet until the frame that consumes it exists.
    void release_input_for_frame()
    {
        if (input_in_flight.empty()) {
            pool_empty_since_us = 0;
            return;
        }
        const size_t slot = input_in_flight.front();
        input_in_flight.pop_front();
        if (slot < input_free.size())
            input_free[slot] = true;
        if (free_input_slots() > 0)
            pool_empty_since_us = 0;
        ready.notify_all();
    }

    // Liveness net for the (should not happen) case of a packet that never
    // produces a frame, which would leave the pool permanently empty and the
    // guest waiting forever.
    bool release_stalled_input_slot()
    {
        if (pool_empty_since_us == 0 || input_in_flight.empty())
            return false;
        if (monotonic_us() - pool_empty_since_us < INPUT_POOL_STALL_US)
            return false;
        const size_t slot = input_in_flight.front();
        input_in_flight.pop_front();
        if (slot < input_free.size())
            input_free[slot] = true;
        ++stalled_releases;
        pool_empty_since_us = free_input_slots() == 0 ? monotonic_us() : 0;
        BD_LOG("MEDIA",
               "input pool stalled %lld us, releasing slot %zu (stalled=%zu)",
               (long long)INPUT_POOL_STALL_US, slot, stalled_releases);
        return true;
    }

    void reset_input_pool()
    {
        for (size_t i = 0; i < input_free.size(); ++i)
            input_free[i] = true;
        input_in_flight.clear();
        pool_empty_since_us = 0;
        ready.notify_all();
    }

    ~AMediaCodec() {
        delete output_format;
        sws_freeContext(sws);
        swr_free(&swr);
        avcodec_free_context(&context);
    }

    void clear_output() {
        std::lock_guard<std::mutex> lock(mutex);
        pending.clear();
        outstanding.clear();
        format_changed = false;
        next_output_index = 0;
        reset_input_pool();
    }

    void make_output_format() {
        delete output_format;
        output_format = new AMediaFormat;
        output_format->strings["mime"] = mime_for_codec(codec_id);
        if (context->codec_type == AVMEDIA_TYPE_VIDEO) {
            output_format->ints["width"] = context->width;
            output_format->ints["height"] = context->height;
            output_format->ints["stride"] = context->width;
            output_format->ints["slice-height"] = context->height;
            output_format->ints["color-format"] = COLOR_FormatYUV420Planar;
        } else {
            output_format->ints["sample-rate"] = context->sample_rate;
            output_format->ints["channel-count"] = context->channels;
        }
    }

    AMediaFormat* clone_output_format() const {
        if (!output_format) return nullptr;
        auto* copy = new AMediaFormat;
        copy->ints = output_format->ints;
        copy->longs = output_format->longs;
        copy->floats = output_format->floats;
        copy->strings = output_format->strings;
        return copy;
    }

    bool append_video(AVFrame* frame, CodecOutput& output) {
        const int width = frame->width;
        const int height = frame->height;
        const int bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                                   width, height, 1);
        if (bytes <= 0) return false;
        output.bytes.resize(bytes);
        uint8_t* dst[4]{};
        int linesize[4]{};
        av_image_fill_arrays(dst, linesize, output.bytes.data(),
                             AV_PIX_FMT_YUV420P, width, height, 1);
        sws = sws_getCachedContext(
            sws, width, height, static_cast<AVPixelFormat>(frame->format),
            width, height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR,
            nullptr, nullptr, nullptr);
        if (!sws || sws_scale(sws, frame->data, frame->linesize, 0, height,
                              dst, linesize) != height)
            return false;
        // The video bridge consumes this as tightly packed I420, so publish the
        // geometry it was packed with rather than the codec context size.
        output.width = width;
        output.height = height;
        return true;
    }

    bool append_audio(AVFrame* frame, CodecOutput& output) {
        int64_t in_layout = frame->channel_layout;
        if (!in_layout)
            in_layout = av_get_default_channel_layout(frame->channels);
        int64_t out_layout = in_layout;
        swr = swr_alloc_set_opts(
            swr, out_layout, AV_SAMPLE_FMT_S16, frame->sample_rate,
            in_layout, static_cast<AVSampleFormat>(frame->format),
            frame->sample_rate, 0, nullptr);
        if (!swr || swr_init(swr) < 0) return false;
        int out_samples = swr_get_out_samples(swr, frame->nb_samples);
        int bytes = av_samples_get_buffer_size(
            nullptr, frame->channels, out_samples, AV_SAMPLE_FMT_S16, 1);
        if (bytes <= 0) return false;
        output.bytes.resize(bytes);
        uint8_t* dst[] = {output.bytes.data()};
        int converted = swr_convert(swr, dst, out_samples,
                                    const_cast<const uint8_t**>(frame->extended_data),
                                    frame->nb_samples);
        if (converted < 0) return false;
        output.bytes.resize(av_samples_get_buffer_size(
            nullptr, frame->channels, converted, AV_SAMPLE_FMT_S16, 1));
        return true;
    }

    void drain() {
        AVFrame* frame = av_frame_alloc();
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++drain_calls;
        }
        const int64_t drain_start = monotonic_us();
        size_t produced = 0;
        while (frame && avcodec_receive_frame(context, frame) == 0) {
            CodecOutput output;
            output.index = next_output_index++;
            output.pts = frame->best_effort_timestamp;
            if (context->pkt_timebase.num && context->pkt_timebase.den)
                output.pts = av_rescale_q(output.pts, context->pkt_timebase,
                                          AVRational{1, 1000000});
            bool ok = context->codec_type == AVMEDIA_TYPE_VIDEO
                ? append_video(frame, output) : append_audio(frame, output);
            if (ok) {
                if (decoded_frames < 3) {
                    auto range = std::minmax_element(
                        output.bytes.begin(), output.bytes.end());
                    BD_LOG("MEDIA",
                           "decoded frame=%zu format=%d bytes=%zu pts=%lld range=%u..%u",
                           decoded_frames, frame->format, output.bytes.size(),
                           static_cast<long long>(output.pts),
                           output.bytes.empty() ? 0 : *range.first,
                           output.bytes.empty() ? 0 : *range.second);
                }
                ++decoded_frames;
                ++produced;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    // A decoded frame is exactly the event that frees the input
                    // buffer that produced it, so this is where the guest's
                    // producer side gets throttled. Nothing is ever dropped
                    // here: the frame goes to the guest even if it is late.
                    release_input_for_frame();
                    pending.push_back(std::move(output));
                    if (pending.size() > peak_pending)
                        peak_pending = pending.size();
                    ready.notify_all();
                }
            }
            av_frame_unref(frame);
        }
        av_frame_free(&frame);
        if (produced) {
            const int64_t cost = monotonic_us() - drain_start;
            std::lock_guard<std::mutex> lock(mutex);
            drain_us += (uint64_t)cost;
            drain_samples += produced;
        }
    }
};

///// Diagnostics registry
//
// Bringing up the codec path means knowing why a track stopped feeding, and the
// per-codec log lines are throttled. Keep every live codec reachable so the
// render thread can dump the whole state on a timer.
namespace {

std::mutex g_codecs_mutex;
std::vector<AMediaCodec*> g_codecs;

void bd_media_register(AMediaCodec* codec)
{
    std::lock_guard<std::mutex> lock(g_codecs_mutex);
    g_codecs.push_back(codec);
}

void bd_media_unregister(AMediaCodec* codec)
{
    std::lock_guard<std::mutex> lock(g_codecs_mutex);
    g_codecs.erase(std::remove(g_codecs.begin(), g_codecs.end(), codec),
                   g_codecs.end());
}

} // namespace

// Called from the render thread (bd_video::pump) every couple of seconds.
extern "C" void bd_media_dump_state()
{
    std::lock_guard<std::mutex> registry_lock(g_codecs_mutex);
    for (AMediaCodec* codec : g_codecs) {
        if (!codec || !codec->context)
            continue;
        std::lock_guard<std::mutex> lock(codec->mutex);
        const char* kind =
            codec->context->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "audio";
        size_t oldest_pending = codec->pending.empty()
            ? 0 : (size_t)codec->pending.front().pts;
        // Per-frame decode cost: send_us covers the bitstream/packet work,
        // drain_us the reconstruction + I420 conversion. Reported in ms/frame so
        // it can be compared directly with bd_video's convert/upload numbers.
        const double send_ms = codec->send_samples
            ? (double)codec->send_us / (double)codec->send_samples / 1000.0 : 0.0;
        const double drain_ms = codec->drain_samples
            ? (double)codec->drain_us / (double)codec->drain_samples / 1000.0 : 0.0;
        BD_LOG("MEDIA",
               "%s mime=%s surface=%d started=%d inpool=%zu/%zu pend=%zu peak=%zu out=%zu "
               "decoded=%zu stalled=%zu drains=%zu in(deq=%zu/try=%zu) "
               "out(deq=%zu/try=%zu) buf=%zu rel=%zu last_out=%lld last_in=%lld oldest=%zu "
               "decode=%.1fms(send %.1f + drain %.1f, n=%zu/%zu)",
               kind, mime_for_codec(codec->codec_id), (int)codec->surface_mode,
               (int)codec->started, codec->free_input_slots(),
               codec->input_free.size(),
               codec->pending.size(), codec->peak_pending,
               codec->outstanding.size(),
               codec->decoded_frames, codec->stalled_releases, codec->drain_calls,
               codec->input_dequeues, codec->input_try_again,
               codec->output_dequeues, codec->output_try_again,
               codec->get_buffer_calls, codec->render_calls,
               (long long)codec->last_output_pts,
               (long long)codec->last_input_pts, oldest_pending,
               send_ms + drain_ms, send_ms, drain_ms,
               codec->send_samples, codec->drain_samples);
        codec->send_us = codec->drain_us = 0;
        codec->send_samples = codec->drain_samples = 0;
    }
}

extern "C" {
ABI_ATTR const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
ABI_ATTR const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
ABI_ATTR const char* AMEDIAFORMAT_KEY_COLOR_RANGE = "color-range";
ABI_ATTR const char* AMEDIAFORMAT_KEY_COLOR_STANDARD = "color-standard";
ABI_ATTR const char* AMEDIAFORMAT_KEY_DURATION = "durationUs";
ABI_ATTR const char* AMEDIAFORMAT_KEY_ENCODER_DELAY = "encoder-delay";
ABI_ATTR const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
ABI_ATTR const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
ABI_ATTR const char* AMEDIAFORMAT_KEY_LANGUAGE = "language";
ABI_ATTR const char* AMEDIAFORMAT_KEY_MIME = "mime";
ABI_ATTR const char* AMEDIAFORMAT_KEY_ROTATION = "rotation-degrees";
ABI_ATTR const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
ABI_ATTR const char* AMEDIAFORMAT_KEY_SLICE_HEIGHT = "slice-height";
ABI_ATTR const char* AMEDIAFORMAT_KEY_STRIDE = "stride";
ABI_ATTR const char* AMEDIAFORMAT_KEY_WIDTH = "width";

ABI_ATTR AMediaDataSource* AMediaDataSource_new() {
    BD_LOG("MEDIA", "data source new");
    return new AMediaDataSource;
}
ABI_ATTR void AMediaDataSource_delete(AMediaDataSource* source) {
    BD_LOG("MEDIA", "data source delete userdata=%p", source ? source->userdata : nullptr);
    if (source && source->close)
        source->close(source->userdata);
    delete source;
}
ABI_ATTR void AMediaDataSource_setUserdata(AMediaDataSource* source, void* data) {
    if (source) source->userdata = data;
    BD_LOG("MEDIA", "data source userdata=%p", data);
}
ABI_ATTR void AMediaDataSource_setReadAt(
    AMediaDataSource* source, media_data_source_read_at callback) {
    if (source) source->read_at = callback;
    BD_LOG("MEDIA", "data source readAt=%p", reinterpret_cast<void*>(callback));
}
ABI_ATTR void AMediaDataSource_setGetSize(
    AMediaDataSource* source, media_data_source_get_size callback) {
    if (source) source->get_size = callback;
    BD_LOG("MEDIA", "data source getSize=%p", reinterpret_cast<void*>(callback));
}
ABI_ATTR void AMediaDataSource_setClose(
    AMediaDataSource* source, media_data_source_close callback) {
    if (source) source->close = callback;
    BD_LOG("MEDIA", "data source close=%p", reinterpret_cast<void*>(callback));
}

static media_status_t extractor_open_io(
    AMediaExtractor* extractor, std::unique_ptr<IoState> io) {
    if (!extractor || !io) return AMEDIA_ERROR_UNKNOWN;
    constexpr int buffer_size = 64 * 1024;
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(buffer_size));
    if (!buffer) return AMEDIA_ERROR_UNKNOWN;
    AVIOContext* avio = avio_alloc_context(
        buffer, buffer_size, 0, io.get(), io_read, nullptr, io_seek);
    if (!avio) {
        av_free(buffer);
        return AMEDIA_ERROR_UNKNOWN;
    }
    AVFormatContext* format = avformat_alloc_context();
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
    if (result >= 0)
        result = avformat_find_stream_info(format, nullptr);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, error, sizeof(error));
        BD_LOG("MEDIA", "extractor open failed: %s (%d), length=%lld",
               error, result, static_cast<long long>(io->length));
        if (format) avformat_close_input(&format);
        av_freep(&avio->buffer);
        avio_context_free(&avio);
        return AMEDIA_ERROR_UNKNOWN;
    }
    extractor->format = format;
    extractor->avio = avio;
    extractor->io = std::move(io);
    extractor->selected.assign(format->nb_streams, false);
    BD_LOG("MEDIA", "extractor opened tracks=%u", format->nb_streams);
    return AMEDIA_OK;
}

ABI_ATTR AMediaExtractor* AMediaExtractor_new() {
    BD_LOG("MEDIA", "extractor new");
    return new AMediaExtractor;
}
ABI_ATTR media_status_t AMediaExtractor_delete(AMediaExtractor* extractor) {
    BD_LOG("MEDIA", "extractor delete");
    delete extractor;
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaExtractor_setDataSource(
    AMediaExtractor* extractor, const char* path) {
    BD_LOG("MEDIA", "extractor path source=%s", path ? path : "(null)");
    if (!extractor || !path) return AMEDIA_ERROR_UNKNOWN;
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format, nullptr) < 0) {
        if (format) avformat_close_input(&format);
        return AMEDIA_ERROR_UNKNOWN;
    }
    extractor->format = format;
    extractor->selected.assign(format->nb_streams, false);
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaExtractor_setDataSourceFd(
    AMediaExtractor* extractor, int fd, int64_t offset, int64_t length) {
    BD_LOG("MEDIA", "extractor fd source=%d offset=%lld length=%lld", fd,
           static_cast<long long>(offset), static_cast<long long>(length));
    auto io = std::make_unique<IoState>();
    io->fd = dup(fd);
    io->base = offset;
    io->length = length;
    return io->fd < 0 ? AMEDIA_ERROR_UNKNOWN
                      : extractor_open_io(extractor, std::move(io));
}
ABI_ATTR media_status_t AMediaExtractor_setDataSourceCustom(
    AMediaExtractor* extractor, AMediaDataSource* source) {
    BD_LOG("MEDIA", "extractor custom source=%p userdata=%p readAt=%p getSize=%p",
           static_cast<void*>(source), source ? source->userdata : nullptr,
           source ? reinterpret_cast<void*>(source->read_at) : nullptr,
           source ? reinterpret_cast<void*>(source->get_size) : nullptr);
    if (!source || !source->read_at) return AMEDIA_ERROR_UNKNOWN;
    auto io = std::make_unique<IoState>();
    io->userdata = source->userdata;
    io->read_at = source->read_at;
    io->get_size = source->get_size;
    io->length = source->get_size ? source->get_size(source->userdata) : -1;
    BD_LOG("MEDIA", "custom source length=%lld",
           static_cast<long long>(io->length));
    return extractor_open_io(extractor, std::move(io));
}
ABI_ATTR size_t AMediaExtractor_getTrackCount(AMediaExtractor* extractor) {
    size_t count =
        extractor && extractor->format ? extractor->format->nb_streams : 0;
    BD_LOG("MEDIA", "extractor track count=%zu", count);
    return count;
}
ABI_ATTR AMediaFormat* AMediaExtractor_getTrackFormat(
    AMediaExtractor* extractor, size_t index) {
    if (!extractor || !extractor->format ||
        index >= extractor->format->nb_streams) return nullptr;
    AVStream* stream = extractor->format->streams[index];
    AVCodecParameters* par = stream->codecpar;
    auto* format = new AMediaFormat;
    format->codecpar = avcodec_parameters_alloc();
    avcodec_parameters_copy(format->codecpar, par);
    format->strings["mime"] = mime_for_codec(par->codec_id);
    format->ints["width"] = par->width;
    format->ints["height"] = par->height;
    format->ints["sample-rate"] = par->sample_rate;
    format->ints["channel-count"] = par->channels;
    AVRational rate = av_guess_frame_rate(extractor->format, stream, nullptr);
    if (rate.den) format->ints["frame-rate"] = av_q2d(rate);
    if (stream->duration != AV_NOPTS_VALUE)
        format->longs["durationUs"] =
            av_rescale_q(stream->duration, stream->time_base,
                         AVRational{1, 1000000});
    else if (extractor->format->duration != AV_NOPTS_VALUE)
        format->longs["durationUs"] = extractor->format->duration;
    AVDictionaryEntry* language =
        av_dict_get(stream->metadata, "language", nullptr, 0);
    format->strings["language"] = language ? language->value : "und";
    BD_LOG("MEDIA", "track %zu mime=%s size=%dx%d rate=%d channels=%d",
           index, format->strings["mime"].c_str(), par->width, par->height,
           par->sample_rate, par->channels);
    return format;
}
ABI_ATTR media_status_t AMediaExtractor_selectTrack(
    AMediaExtractor* extractor, size_t index) {
    if (!extractor || index >= extractor->selected.size())
        return AMEDIA_ERROR_UNKNOWN;
    extractor->selected[index] = true;
    return AMEDIA_OK;
}
ABI_ATTR int AMediaExtractor_getSampleTrackIndex(AMediaExtractor* extractor) {
    return extractor && extractor->ensure_packet()
        ? extractor->packet->stream_index : -1;
}
ABI_ATTR ssize_t AMediaExtractor_readSampleData(
    AMediaExtractor* extractor, uint8_t* buffer, size_t capacity) {
    if (!extractor || !buffer || !extractor->ensure_packet()) return -1;
    size_t size = std::min<size_t>(capacity, extractor->packet->size);
    memcpy(buffer, extractor->packet->data, size);
    return size;
}
ABI_ATTR int64_t AMediaExtractor_getSampleTime(AMediaExtractor* extractor) {
    if (!extractor || !extractor->ensure_packet()) return -1;
    AVPacket* packet = extractor->packet;
    int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (pts == AV_NOPTS_VALUE) return -1;
    return av_rescale_q(pts,
        extractor->format->streams[packet->stream_index]->time_base,
        AVRational{1, 1000000});
}
ABI_ATTR bool AMediaExtractor_advance(AMediaExtractor* extractor) {
    if (!extractor) return false;
    if (extractor->has_packet) {
        av_packet_unref(extractor->packet);
        extractor->has_packet = false;
    }
    return extractor->ensure_packet();
}
ABI_ATTR media_status_t AMediaExtractor_seekTo(
    AMediaExtractor* extractor, int64_t time_us, int) {
    if (!extractor || !extractor->format) return AMEDIA_ERROR_UNKNOWN;
    if (extractor->has_packet) {
        av_packet_unref(extractor->packet);
        extractor->has_packet = false;
    }
    return av_seek_frame(extractor->format, -1, time_us,
                         AVSEEK_FLAG_BACKWARD) >= 0
        ? AMEDIA_OK : AMEDIA_ERROR_UNKNOWN;
}

ABI_ATTR AMediaFormat* AMediaFormat_new() { return new AMediaFormat; }
ABI_ATTR media_status_t AMediaFormat_delete(AMediaFormat* format) {
    delete format;
    return AMEDIA_OK;
}
ABI_ATTR bool AMediaFormat_getInt32(
    AMediaFormat* format, const char* key, int32_t* value) {
    if (!format || !key || !value) return false;
    auto found = format->ints.find(key);
    if (found == format->ints.end()) return false;
    *value = found->second;
    // Unity derives its video path from what it reads back here; a handful of
    // lines is enough to see whether it follows the Surface or the YUV route.
    static int logged = 0;
    if (logged < 30) {
        ++logged;
        BD_LOG("MEDIA", "format getInt32 %s = %d", key, *value);
    }
    return true;
}
ABI_ATTR bool AMediaFormat_getInt64(
    AMediaFormat* format, const char* key, int64_t* value) {
    if (!format || !key || !value) return false;
    auto found = format->longs.find(key);
    if (found == format->longs.end()) return false;
    *value = found->second;
    return true;
}
ABI_ATTR bool AMediaFormat_getFloat(
    AMediaFormat* format, const char* key, float* value) {
    if (!format || !key || !value) return false;
    auto found = format->floats.find(key);
    if (found == format->floats.end()) return false;
    *value = found->second;
    return true;
}
ABI_ATTR bool AMediaFormat_getString(
    AMediaFormat* format, const char* key, const char** value) {
    if (!format || !key || !value) return false;
    auto found = format->strings.find(key);
    if (found == format->strings.end()) return false;
    *value = found->second.c_str();
    return true;
}
ABI_ATTR void AMediaFormat_setInt32(
    AMediaFormat* format, const char* key, int32_t value) {
    if (format && key) format->ints[key] = value;
}

ABI_ATTR AMediaCodec* AMediaCodec_createDecoderByType(const char* mime) {
    AVCodecID id = codec_for_mime(mime);
    const AVCodec* decoder = avcodec_find_decoder(id);
    BD_LOG("MEDIA", "create decoder mime=%s codec=%d found=%s",
           mime ? mime : "(null)", id, decoder ? decoder->name : "(none)");
    if (!decoder) return nullptr;
    auto* codec = new AMediaCodec;
    codec->codec_id = id;
    codec->decoder = decoder;
    bd_media_register(codec);
    return codec;
}
ABI_ATTR media_status_t AMediaCodec_delete(AMediaCodec* codec) {
    bd_media_unregister(codec);
    delete codec;
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaCodec_configure(
    AMediaCodec* codec, AMediaFormat* format, void* window, void* crypto,
    uint32_t flags) {
    (void)crypto;
    (void)flags;
    if (!codec || !format || !codec->decoder) return AMEDIA_ERROR_UNKNOWN;
    codec->surface_mode = window != nullptr;
    codec->context = avcodec_alloc_context3(codec->decoder);
    if (!codec->context) return AMEDIA_ERROR_UNKNOWN;
    if (format->codecpar &&
        avcodec_parameters_to_context(codec->context, format->codecpar) < 0)
        return AMEDIA_ERROR_UNKNOWN;
    codec->context->pkt_timebase = AVRational{1, 1000000};
    // Decode tuning. The defaults keep FFmpeg's own choices, so what runs is the
    // "baseline" row of BD_MEDIA_BENCH unless a knob says otherwise - start with
    // BD_MEDIA_BENCH on the device, then enable the winner here.
    {
        const char* threads = getenv("BD_MEDIA_THREADS");
        if (threads)
            codec->context->thread_count = atoi(threads);
        if (const char* fast = getenv("BD_MEDIA_FAST"))
            if (atoi(fast) != 0)
                codec->context->flags2 |= AV_CODEC_FLAG2_FAST;
        if (const char* skip = getenv("BD_MEDIA_SKIP_LOOP"))
            if (atoi(skip) != 0)
                codec->context->skip_loop_filter = AVDISCARD_ALL;
    }
    int32_t color_format = -1;
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format);
    codec->make_output_format();
    BD_LOG("MEDIA",
           "codec configured mime=%s %dx%d rate=%d channels=%d window=%p surface=%d in-color-format=%d",
           mime_for_codec(codec->codec_id), codec->context->width,
           codec->context->height, codec->context->sample_rate,
           codec->context->channels, window, (int)codec->surface_mode,
           color_format);
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaCodec_start(AMediaCodec* codec) {
    if (!codec || !codec->context)
        return AMEDIA_ERROR_UNKNOWN;
    int result = avcodec_open2(codec->context, codec->decoder, nullptr);
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, error, sizeof(error));
        BD_LOG("MEDIA", "codec start failed: %s (%d)", error, result);
        return AMEDIA_ERROR_UNKNOWN;
    }
    BD_LOG("MEDIA", "codec started type=%d", codec->context->codec_type);
    {
        std::lock_guard<std::mutex> lock(codec->mutex);
        codec->started = true;
        codec->format_changed = true;
        codec->reset_input_pool();
    }
    codec->make_output_format();
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaCodec_stop(AMediaCodec* codec) {
    if (!codec) return AMEDIA_ERROR_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(codec->mutex);
        codec->started = false;
        codec->reset_input_pool();
    }
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaCodec_flush(AMediaCodec* codec) {
    if (!codec || !codec->context) return AMEDIA_ERROR_UNKNOWN;
    avcodec_flush_buffers(codec->context);
    codec->clear_output();
    return AMEDIA_OK;
}
ABI_ATTR ssize_t AMediaCodec_dequeueInputBuffer(
    AMediaCodec* codec, int64_t timeout_us) {
    if (!codec) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    std::unique_lock<std::mutex> lock(codec->mutex);
    // Availability is exactly "one of the input buffers came back". A buffer
    // comes back when the decoder has turned its packet into a frame
    // (AMediaCodec::release_input_for_frame), so the guest can only ever be
    // INPUT_BUFFER_COUNT packets ahead of the frames it has consumed. That is
    // also the only flow control that keeps our frame indices in step with
    // Unity's player clock.
    //
    // Two things this deliberately does NOT depend on:
    //  - the output queue. Unity's AndroidVideoMedia feeds and drains from one
    //    loop (dequeueInputBuffer -> ... -> dequeueOutputBuffer), so gating input
    //    on `pending.size() < cap` made the guest itself the only one who could
    //    empty the queue and the loop spun on TRY_AGAIN forever (handheld INTRO:
    //    decoded=5 pend=4 in(deq=7/try=35673), frozen 0.27 s in).
    //  - wall-clock pacing. With the pool above, a producer that runs ahead
    //    simply runs out of buffers.
    auto available = [&] {
        return !codec->started || codec->free_input_slots() > 0;
    };
    // A nominal zero timeout is legal but Unity polls it continuously. A 1 ms
    // floor preserves responsiveness while preventing a failed player from
    // consuming an entire CPU core.
    auto wait = std::chrono::microseconds(
        timeout_us > 0 ? std::min<int64_t>(timeout_us, 100000) : 1000);
    if (!available())
        codec->ready.wait_for(lock, wait, available);
    if (!codec->started) {
        ++codec->input_try_again;
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    if (codec->free_input_slots() == 0 &&
        !codec->release_stalled_input_slot()) {
        ++codec->input_try_again;
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    // The output queue is a memory guard only; when it is hit we hold input
    // back rather than throwing away frames the guest has not seen yet.
    if (codec->pending.size() >= MAX_QUEUED_OUTPUTS) {
        ++codec->input_try_again;
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    const ssize_t slot = codec->take_input_slot();
    if (slot < 0) {
        ++codec->input_try_again;
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    ++codec->input_dequeues;
    return slot;
}
ABI_ATTR uint8_t* AMediaCodec_getInputBuffer(
    AMediaCodec* codec, size_t index, size_t* size) {
    if (!codec || index >= codec->input.size()) return nullptr;
    if (codec->input[index].size() < INPUT_BUFFER_SIZE)
        codec->input[index].resize(INPUT_BUFFER_SIZE);
    if (size) *size = codec->input[index].size();
    return codec->input[index].data();
}
ABI_ATTR media_status_t AMediaCodec_queueInputBuffer(
    AMediaCodec* codec, size_t index, off_t offset, size_t size,
    uint64_t presentation_time_us, uint32_t flags) {
    if (!codec || !codec->context || index >= codec->input.size() ||
        offset < 0 ||
        static_cast<size_t>(offset) + size > codec->input[index].size())
        return AMEDIA_ERROR_UNKNOWN;
    {
        std::lock_guard<std::mutex> lock(codec->mutex);
        // The slot must have been handed out by dequeueInputBuffer.
        if (codec->input_free[index])
            return AMEDIA_ERROR_UNKNOWN;
        codec->last_input_pts = (int64_t)presentation_time_us;
    }
    AVPacket* packet = nullptr;
    AVPacket local{};
    if (!(flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
        av_init_packet(&local);
        local.data = codec->input[index].data() + offset;
        local.size = size;
        local.pts = local.dts = presentation_time_us;
        packet = &local;
    }
    const int64_t send_start = monotonic_us();
    int result = avcodec_send_packet(codec->context, packet);
    if (result == AVERROR(EAGAIN)) {
        codec->drain();
        result = avcodec_send_packet(codec->context, packet);
    }
    if (codec->context->codec_type == AVMEDIA_TYPE_VIDEO) {
        const int64_t send_cost = monotonic_us() - send_start;
        std::lock_guard<std::mutex> lock(codec->mutex);
        codec->send_us += (uint64_t)send_cost;
        ++codec->send_samples;
    }
    if (result < 0 && result != AVERROR_EOF) {
        char error[AV_ERROR_MAX_STRING_SIZE]{};
        av_strerror(result, error, sizeof(error));
        BD_LOG("MEDIA", "queue input failed: %s (%d), size=%zu flags=0x%x",
               error, result, size, flags);
        return AMEDIA_ERROR_UNKNOWN;
    }
    codec->drain();
    if (flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
        CodecOutput eos;
        eos.index = codec->next_output_index++;
        eos.pts = presentation_time_us;
        eos.flags = AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        std::lock_guard<std::mutex> lock(codec->mutex);
        // EOS has to be delivered whatever the queue looks like: it is the
        // guest's only signal that the stream is over, and a player that never
        // sees it waits forever. It also carries no frame, so the whole pool
        // goes back at once (frames still buffered in the decoder keep coming
        // out and no longer need to free a slot).
        codec->reset_input_pool();
        codec->pending.push_back(std::move(eos));
        codec->ready.notify_all();
    }
    return AMEDIA_OK;
}
ABI_ATTR ssize_t AMediaCodec_dequeueOutputBuffer(
    AMediaCodec* codec, AMediaCodecBufferInfo* info, int64_t timeout_us) {
    if (!codec || !info) return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    std::unique_lock<std::mutex> lock(codec->mutex);
    if (codec->format_changed) {
        codec->format_changed = false;
        return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    }
    auto available = [&] {
        return !codec->started || codec->format_changed ||
               !codec->pending.empty();
    };
    auto wait = std::chrono::microseconds(
        timeout_us > 0 ? std::min<int64_t>(timeout_us, 100000) : 1000);
    if (codec->pending.empty())
        codec->ready.wait_for(lock, wait, available);
    if (codec->format_changed) {
        codec->format_changed = false;
        return AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED;
    }
    if (!codec->started || codec->pending.empty()) {
        ++codec->output_try_again;
        return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
    }
    CodecOutput output = std::move(codec->pending.front());
    codec->pending.pop_front();
    info->offset = 0;
    info->size = output.bytes.size();
    info->presentationTimeUs = output.pts;
    info->flags = output.flags;
    size_t index = output.index;
    codec->last_output_pts = output.pts;
    ++codec->output_dequeues;
    codec->outstanding.emplace(index, std::move(output));
    return index;
}
ABI_ATTR uint8_t* AMediaCodec_getOutputBuffer(
    AMediaCodec* codec, size_t index, size_t* size) {
    if (!codec) return nullptr;
    std::lock_guard<std::mutex> lock(codec->mutex);
    auto found = codec->outstanding.find(index);
    if (found == codec->outstanding.end()) return nullptr;
    if (size) *size = found->second.bytes.size();
    ++codec->get_buffer_calls;
    if (codec->get_buffer_calls <= 3 || (codec->get_buffer_calls % 300) == 0)
        BD_LOG("MEDIA", "getOutputBuffer #%zu index=%zu size=%zu surface=%d",
               codec->get_buffer_calls, index, found->second.bytes.size(),
               (int)codec->surface_mode);
    return found->second.bytes.data();
}
ABI_ATTR AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* codec) {
    // NDK contract: caller owns the returned format and must
    // AMediaFormat_delete it. Hand out a clone so our destructor is safe.
    return codec ? codec->clone_output_format() : nullptr;
}
ABI_ATTR media_status_t AMediaCodec_releaseOutputBuffer(
    AMediaCodec* codec, size_t index, bool render) {
    if (!codec) return AMEDIA_ERROR_UNKNOWN;
    // Hand the decoded frame to the video bridge. Whichever path Unity uses
    // (release-with-render for a Surface, getOutputBuffer/release for CPU
    // buffers), this is the last point where the pixels exist, so publish here.
    //
    // On the Surface path `render` is Unity's own decision
    // (AndroidVideoMedia::ConsumeOutputBuffers), and it says so for exactly the
    // frames whose index is in step with its player clock. Publishing the ones
    // it declined would put the bridge one full frame-count out of phase with
    // the guest, so follow it: render==false means "throw this frame away", and
    // that is what a real codec does too (ACodec::releaseOutputBuffer).
    // A codec configured without a Surface decodes into byte buffers that the
    // app reads with getOutputBuffer/AMediaCodec_getOutputBuffer itself; those
    // pixels have nothing to do with the video texture. Publishing them is not
    // just wasted work: Unity's CPU-path probe (it decodes two frames of a clip
    // through a windowless codec before creating the real one) runs exactly when
    // the previous clip's SurfaceTexture is being torn down, so the frame would
    // be handed to a listener proxy whose Java object is already gone.
    const bool publish = render && codec->surface_mode;
    std::vector<uint8_t> frame;
    int width = 0;
    int height = 0;
    int64_t pts = 0;
    size_t bytes = 0;
    size_t calls = 0;
    {
        std::lock_guard<std::mutex> lock(codec->mutex);
        auto found = codec->outstanding.find(index);
        if (found != codec->outstanding.end()) {
            if (publish && codec->context &&
                codec->context->codec_type == AVMEDIA_TYPE_VIDEO) {
                bytes = found->second.bytes.size();
                frame = std::move(found->second.bytes);
                width = found->second.width;
                height = found->second.height;
                pts = found->second.pts;
            }
            codec->outstanding.erase(found);
            codec->ready.notify_all();
        }
        calls = ++codec->render_calls;
    }
    if (calls <= 3 || (calls % 300) == 0)
        BD_LOG("MEDIA", "releaseOutputBuffer #%zu index=%zu render=%d surface=%d",
               calls, index, (int)render, (int)codec->surface_mode);
    if (!frame.empty())
        bd_video::submit_i420(frame.data(), bytes, width, height, pts);
    return AMEDIA_OK;
}
ABI_ATTR media_status_t AMediaCodec_setOutputSurface(
    AMediaCodec* codec, void* window) {
    if (codec)
        codec->surface_mode = window != nullptr;
    BD_LOG("MEDIA", "setOutputSurface window=%p surface=%d", window,
           codec ? (int)codec->surface_mode : -1);
    return AMEDIA_OK;
}

ABI_ATTR media_status_t AImageReader_newWithUsage(
    int32_t width, int32_t height, int32_t format, uint64_t usage,
    int32_t max_images, AImageReader** reader) {
    BD_LOG("MEDIA",
           "ImageReader unsupported %dx%d format=%d usage=0x%llx max=%d "
           "(force YUV byte-buffer path)",
           width, height, format, static_cast<unsigned long long>(usage),
           max_images);
    if (reader) *reader = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR media_status_t AImageReader_setImageListener(
    AImageReader*, AImageReader_ImageListener*) {
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR media_status_t AImageReader_setBufferRemovedListener(
    AImageReader*, AImageReader_BufferRemovedListener*) {
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR media_status_t AImageReader_getWindow(AImageReader*, void** window) {
    if (window) *window = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR media_status_t AImageReader_acquireLatestImage(
    AImageReader*, AImage** image) {
    if (image) *image = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR void AImageReader_delete(AImageReader*) {}
ABI_ATTR media_status_t AImage_getHardwareBuffer(
    const AImage*, AHardwareBuffer** buffer) {
    if (buffer) *buffer = nullptr;
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR media_status_t AImage_getWidth(const AImage*, int32_t* width) {
    if (width) *width = 0;
    return AMEDIA_ERROR_UNKNOWN;
}
ABI_ATTR void AImage_delete(AImage*) {}
ABI_ATTR void AImage_deleteAsync(AImage*, int) {}
ABI_ATTR void AHardwareBuffer_acquire(AHardwareBuffer*) {}
ABI_ATTR void AHardwareBuffer_release(AHardwareBuffer*) {}
ABI_ATTR void AHardwareBuffer_describe(
    const AHardwareBuffer*, AHardwareBuffer_Desc* desc) {
    if (desc) memset(desc, 0, sizeof(*desc));
}
} // extern "C"

// Standalone decode benchmark - BD_MEDIA_BENCH=<file> [BD_MEDIA_BENCH_FRAMES=N].
//
// "The video stutters" is decided by one number: milliseconds of CPU per frame.
// This measures it with the loader's own FFmpeg build on this device's CPU,
// before Unity starts, sweeping the knobs that are actually available to us
// (threading, AV_CODEC_FLAG2_FAST, skipping the deblocking filter), so "can
// software decode ever keep up here, or do we need the VPU?" has a measured
// answer instead of a guess. The same flags are applied to the live decoder in
// AMediaCodec_configure() so the winner can be adopted directly.
namespace {

struct BenchResult {
    double per_frame_ms{};
    int frames{};
};

BenchResult bench_decode_once(const char* path, int frames, int threads,
                              int fast, int skip_loop_filter)
{
    BenchResult result;
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path, nullptr, nullptr) < 0)
        return result;
    if (avformat_find_stream_info(format, nullptr) < 0) {
        avformat_close_input(&format);
        return result;
    }
    const int stream_index =
        av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        avformat_close_input(&format);
        return result;
    }
    const AVCodec* decoder =
        avcodec_find_decoder(format->streams[stream_index]->codecpar->codec_id);
    AVCodecContext* context =
        decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!context ||
        avcodec_parameters_to_context(
            context, format->streams[stream_index]->codecpar) < 0) {
        if (context) avcodec_free_context(&context);
        avformat_close_input(&format);
        return result;
    }
    context->thread_count = threads;  // 0 = FFmpeg's automatic choice
    if (fast)
        context->flags2 |= AV_CODEC_FLAG2_FAST;
    if (skip_loop_filter)
        context->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(context, decoder, nullptr) < 0) {
        avcodec_free_context(&context);
        avformat_close_input(&format);
        return result;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const int64_t start = monotonic_us();
    bool stop = false;
    while (packet && frame && !stop && result.frames < frames) {
        if (av_read_frame(format, packet) < 0)
            break;
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet);
            continue;
        }
        const int sent = avcodec_send_packet(context, packet);
        av_packet_unref(packet);
        if (sent < 0)
            break;
        while (avcodec_receive_frame(context, frame) == 0) {
            ++result.frames;
            av_frame_unref(frame);
            if (result.frames >= frames) {
                stop = true;
                break;
            }
        }
    }
    const int64_t cost = monotonic_us() - start;
    if (result.frames)
        result.per_frame_ms = (double)cost / (double)result.frames / 1000.0;

    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    avcodec_free_context(&context);
    avformat_close_input(&format);
    return result;
}

void bench_report(const char* label, const char* path, int frames, int threads,
                  int fast, int skip_loop_filter)
{
    const BenchResult r =
        bench_decode_once(path, frames, threads, fast, skip_loop_filter);
    // fprintf, not BD_LOG: the benchmark has to answer questions in Release
    // builds too, where BD_ENABLE_LOG is off and every BD_LOG line is gone.
    fprintf(stderr,
            "BD-BENCH %-18s threads=%-2d fast=%d skip_loop=%d  %6.2f ms/frame  "
            "(%5.1f fps, %d frames)\n",
            label, threads, fast, skip_loop_filter, r.per_frame_ms,
            r.per_frame_ms > 0 ? 1000.0 / r.per_frame_ms : 0.0, r.frames);
    fflush(stderr);
}

} // namespace

// A 30 fps clip has 33.3 ms per frame in total, and the bridge needs roughly
// 10 ms of that for I420->RGBA plus the texture upload, so only the top rows of
// this table can ever play smoothly.
extern "C" void bd_media_bench(const char* path, int frames)
{
    if (!path)
        return;
    if (frames <= 0)
        frames = 200;
    BD_LOG("BENCH", "--- %s, %d frames per row, libavcodec %u ---", path, frames,
           (unsigned)avcodec_version());
    fprintf(stderr, "BD-BENCH --- %s, %d frames per row, libavcodec %u ---\n",
            path, frames, (unsigned)avcodec_version());
    bench_report("baseline", path, frames, 0, 0, 0);
    bench_report("fast", path, frames, 0, 1, 0);
    bench_report("skip_loop_filter", path, frames, 0, 0, 1);
    bench_report("fast+skip_loop", path, frames, 0, 1, 1);
    bench_report("fast+skip+t1", path, frames, 1, 1, 1);
    bench_report("fast+skip+t2", path, frames, 2, 1, 1);
    bench_report("fast+skip+t4", path, frames, 4, 1, 1);
}

