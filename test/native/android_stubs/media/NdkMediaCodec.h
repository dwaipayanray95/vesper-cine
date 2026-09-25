#pragma once
#include <media/NdkMediaFormat.h>
#include <sys/types.h>
#include <android/native_window.h>
struct AMediaCodec; typedef struct AMediaCodec AMediaCodec;
struct AMediaCrypto; struct AMediaCodecCryptoInfo;
typedef struct { int32_t offset; int32_t size; int64_t presentationTimeUs; uint32_t flags; } AMediaCodecBufferInfo;
enum { AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG = 2, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4, AMEDIACODEC_CONFIGURE_FLAG_ENCODE = 1,
 AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3, AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2, AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1 };
AMediaCodec* AMediaCodec_createEncoderByType(const char*);
media_status_t AMediaCodec_delete(AMediaCodec*);
media_status_t AMediaCodec_configure(AMediaCodec*, const AMediaFormat*, ANativeWindow*, AMediaCrypto*, uint32_t);
media_status_t AMediaCodec_start(AMediaCodec*); media_status_t AMediaCodec_stop(AMediaCodec*);
AMediaFormat* AMediaCodec_getInputFormat(AMediaCodec*); AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*);
media_status_t AMediaCodec_getName(AMediaCodec*, char**); void AMediaCodec_releaseName(AMediaCodec*, char*);
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t);
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t, size_t*);
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t, size_t*);
media_status_t AMediaCodec_queueInputBuffer(AMediaCodec*, size_t, off_t, size_t, uint64_t, uint32_t);
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo*, int64_t);
media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t, bool);
