#pragma once

#include "platform.h"
#include <cstddef>
#include <cstdint>
#include <sys/types.h>

struct AMediaDataSource;
struct AMediaExtractor;
struct AMediaFormat;
struct AMediaCodec;

using media_status_t = int32_t;
using media_data_source_read_at = ssize_t (*)(void*, int64_t, void*, size_t);
using media_data_source_get_size = ssize_t (*)(void*);
using media_data_source_close = void (*)(void*);

struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};

extern "C" {
ABI_ATTR AMediaDataSource* AMediaDataSource_new();
ABI_ATTR void AMediaDataSource_delete(AMediaDataSource*);
ABI_ATTR void AMediaDataSource_setUserdata(AMediaDataSource*, void*);
ABI_ATTR void AMediaDataSource_setReadAt(AMediaDataSource*, media_data_source_read_at);
ABI_ATTR void AMediaDataSource_setGetSize(AMediaDataSource*, media_data_source_get_size);
ABI_ATTR void AMediaDataSource_setClose(AMediaDataSource*, media_data_source_close);

ABI_ATTR AMediaExtractor* AMediaExtractor_new();
ABI_ATTR media_status_t AMediaExtractor_delete(AMediaExtractor*);
ABI_ATTR media_status_t AMediaExtractor_setDataSource(
    AMediaExtractor*, const char*);
ABI_ATTR media_status_t AMediaExtractor_setDataSourceFd(
    AMediaExtractor*, int, int64_t, int64_t);
ABI_ATTR media_status_t AMediaExtractor_setDataSourceCustom(
    AMediaExtractor*, AMediaDataSource*);
ABI_ATTR size_t AMediaExtractor_getTrackCount(AMediaExtractor*);
ABI_ATTR AMediaFormat* AMediaExtractor_getTrackFormat(AMediaExtractor*, size_t);
ABI_ATTR media_status_t AMediaExtractor_selectTrack(AMediaExtractor*, size_t);
ABI_ATTR int AMediaExtractor_getSampleTrackIndex(AMediaExtractor*);
ABI_ATTR ssize_t AMediaExtractor_readSampleData(
    AMediaExtractor*, uint8_t*, size_t);
ABI_ATTR int64_t AMediaExtractor_getSampleTime(AMediaExtractor*);
ABI_ATTR bool AMediaExtractor_advance(AMediaExtractor*);
ABI_ATTR media_status_t AMediaExtractor_seekTo(
    AMediaExtractor*, int64_t, int);

ABI_ATTR AMediaFormat* AMediaFormat_new();
ABI_ATTR media_status_t AMediaFormat_delete(AMediaFormat*);
ABI_ATTR bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t*);
ABI_ATTR bool AMediaFormat_getInt64(AMediaFormat*, const char*, int64_t*);
ABI_ATTR bool AMediaFormat_getFloat(AMediaFormat*, const char*, float*);
ABI_ATTR bool AMediaFormat_getString(AMediaFormat*, const char*, const char**);
ABI_ATTR void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);

ABI_ATTR AMediaCodec* AMediaCodec_createDecoderByType(const char*);
ABI_ATTR media_status_t AMediaCodec_delete(AMediaCodec*);
ABI_ATTR media_status_t AMediaCodec_configure(
    AMediaCodec*, AMediaFormat*, void*, void*, uint32_t);
ABI_ATTR media_status_t AMediaCodec_start(AMediaCodec*);
ABI_ATTR media_status_t AMediaCodec_stop(AMediaCodec*);
ABI_ATTR media_status_t AMediaCodec_flush(AMediaCodec*);
ABI_ATTR ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t);
ABI_ATTR uint8_t* AMediaCodec_getInputBuffer(
    AMediaCodec*, size_t, size_t*);
ABI_ATTR media_status_t AMediaCodec_queueInputBuffer(
    AMediaCodec*, size_t, off_t, size_t, uint64_t, uint32_t);
ABI_ATTR ssize_t AMediaCodec_dequeueOutputBuffer(
    AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
ABI_ATTR uint8_t* AMediaCodec_getOutputBuffer(
    AMediaCodec*, size_t, size_t*);
ABI_ATTR AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*);
ABI_ATTR media_status_t AMediaCodec_releaseOutputBuffer(
    AMediaCodec*, size_t, bool);
ABI_ATTR media_status_t AMediaCodec_setOutputSurface(
    AMediaCodec*, void*);

// ImageReader / HardwareBuffer: intentionally unsupported. Returning an
// error here forces Unity VideoPlayer onto the YUV byte-buffer path that
// our FFmpeg AMediaCodec implementation actually fills.
struct AImageReader;
struct AImage;
struct AHardwareBuffer;
struct AImageReader_ImageListener;
struct AImageReader_BufferRemovedListener;
struct AHardwareBuffer_Desc {
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t rfu0;
    uint64_t rfu1;
};

ABI_ATTR media_status_t AImageReader_newWithUsage(
    int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**);
ABI_ATTR media_status_t AImageReader_setImageListener(
    AImageReader*, AImageReader_ImageListener*);
ABI_ATTR media_status_t AImageReader_setBufferRemovedListener(
    AImageReader*, AImageReader_BufferRemovedListener*);
ABI_ATTR media_status_t AImageReader_getWindow(AImageReader*, void**);
ABI_ATTR media_status_t AImageReader_acquireLatestImage(
    AImageReader*, AImage**);
ABI_ATTR void AImageReader_delete(AImageReader*);
ABI_ATTR media_status_t AImage_getHardwareBuffer(
    const AImage*, AHardwareBuffer**);
ABI_ATTR media_status_t AImage_getWidth(const AImage*, int32_t*);
ABI_ATTR void AImage_delete(AImage*);
ABI_ATTR void AImage_deleteAsync(AImage*, int);
ABI_ATTR void AHardwareBuffer_acquire(AHardwareBuffer*);
ABI_ATTR void AHardwareBuffer_release(AHardwareBuffer*);
ABI_ATTR void AHardwareBuffer_describe(
    const AHardwareBuffer*, AHardwareBuffer_Desc*);
}

