#ifndef BOGODROID_FFMPEG_OPTIONAL_H
#define BOGODROID_FFMPEG_OPTIONAL_H

// Keep FFmpeg out of the loader's DT_NEEDED list. The media bridge loads the
// matching runtime only when a game actually asks for an Android media object.
#define BD_FFMPEG_SYMBOLS(X) \
    X(av_dict_get) X(av_find_best_stream) X(av_frame_alloc) X(av_frame_free) \
    X(av_frame_unref) X(av_free) X(av_freep) X(av_get_default_channel_layout) \
    X(av_guess_frame_rate) X(av_image_fill_arrays) X(av_image_get_buffer_size) \
    X(av_init_packet) X(av_malloc) X(av_packet_alloc) X(av_packet_free) \
    X(av_packet_unref) X(av_read_frame) X(av_rescale_q) \
    X(av_samples_get_buffer_size) X(av_seek_frame) X(av_strerror) \
    X(avcodec_alloc_context3) X(avcodec_find_decoder) X(avcodec_flush_buffers) \
    X(avcodec_free_context) X(avcodec_open2) X(avcodec_parameters_alloc) \
    X(avcodec_parameters_copy) X(avcodec_parameters_free) \
    X(avcodec_parameters_to_context) X(avcodec_receive_frame) \
    X(avcodec_send_packet) X(avcodec_version) X(avformat_alloc_context) \
    X(avformat_close_input) X(avformat_find_stream_info) X(avformat_open_input) \
    X(avio_alloc_context) X(avio_context_free) X(swr_alloc_set_opts) \
    X(swr_convert) X(swr_free) X(swr_get_out_samples) X(swr_init) \
    X(sws_freeContext) X(sws_getCachedContext) X(sws_scale)

namespace bd_ffmpeg {
#define BD_FFMPEG_DECLARE(name) extern decltype(&::name) name;
BD_FFMPEG_SYMBOLS(BD_FFMPEG_DECLARE)
#undef BD_FFMPEG_DECLARE
bool ensure_loaded();
}

#ifndef BD_FFMPEG_OPTIONAL_IMPLEMENTATION
#define av_dict_get bd_ffmpeg::av_dict_get
#define av_find_best_stream bd_ffmpeg::av_find_best_stream
#define av_frame_alloc bd_ffmpeg::av_frame_alloc
#define av_frame_free bd_ffmpeg::av_frame_free
#define av_frame_unref bd_ffmpeg::av_frame_unref
#define av_free bd_ffmpeg::av_free
#define av_freep bd_ffmpeg::av_freep
#define av_get_default_channel_layout bd_ffmpeg::av_get_default_channel_layout
#define av_guess_frame_rate bd_ffmpeg::av_guess_frame_rate
#define av_image_fill_arrays bd_ffmpeg::av_image_fill_arrays
#define av_image_get_buffer_size bd_ffmpeg::av_image_get_buffer_size
#define av_init_packet bd_ffmpeg::av_init_packet
#define av_malloc bd_ffmpeg::av_malloc
#define av_packet_alloc bd_ffmpeg::av_packet_alloc
#define av_packet_free bd_ffmpeg::av_packet_free
#define av_packet_unref bd_ffmpeg::av_packet_unref
#define av_read_frame bd_ffmpeg::av_read_frame
#define av_rescale_q bd_ffmpeg::av_rescale_q
#define av_samples_get_buffer_size bd_ffmpeg::av_samples_get_buffer_size
#define av_seek_frame bd_ffmpeg::av_seek_frame
#define av_strerror bd_ffmpeg::av_strerror
#define avcodec_alloc_context3 bd_ffmpeg::avcodec_alloc_context3
#define avcodec_find_decoder bd_ffmpeg::avcodec_find_decoder
#define avcodec_flush_buffers bd_ffmpeg::avcodec_flush_buffers
#define avcodec_free_context bd_ffmpeg::avcodec_free_context
#define avcodec_open2 bd_ffmpeg::avcodec_open2
#define avcodec_parameters_alloc bd_ffmpeg::avcodec_parameters_alloc
#define avcodec_parameters_copy bd_ffmpeg::avcodec_parameters_copy
#define avcodec_parameters_free bd_ffmpeg::avcodec_parameters_free
#define avcodec_parameters_to_context bd_ffmpeg::avcodec_parameters_to_context
#define avcodec_receive_frame bd_ffmpeg::avcodec_receive_frame
#define avcodec_send_packet bd_ffmpeg::avcodec_send_packet
#define avcodec_version bd_ffmpeg::avcodec_version
#define avformat_alloc_context bd_ffmpeg::avformat_alloc_context
#define avformat_close_input bd_ffmpeg::avformat_close_input
#define avformat_find_stream_info bd_ffmpeg::avformat_find_stream_info
#define avformat_open_input bd_ffmpeg::avformat_open_input
#define avio_alloc_context bd_ffmpeg::avio_alloc_context
#define avio_context_free bd_ffmpeg::avio_context_free
#define swr_alloc_set_opts bd_ffmpeg::swr_alloc_set_opts
#define swr_convert bd_ffmpeg::swr_convert
#define swr_free bd_ffmpeg::swr_free
#define swr_get_out_samples bd_ffmpeg::swr_get_out_samples
#define swr_init bd_ffmpeg::swr_init
#define sws_freeContext bd_ffmpeg::sws_freeContext
#define sws_getCachedContext bd_ffmpeg::sws_getCachedContext
#define sws_scale bd_ffmpeg::sws_scale
#endif

#endif
