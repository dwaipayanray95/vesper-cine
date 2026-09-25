#pragma once
#include <media/NdkMediaCodec.h>
struct AMediaMuxer; typedef struct AMediaMuxer AMediaMuxer;
enum { AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4 = 0 }; typedef int OutputFormat;
AMediaMuxer* AMediaMuxer_new(int, OutputFormat); media_status_t AMediaMuxer_delete(AMediaMuxer*);
ssize_t AMediaMuxer_addTrack(AMediaMuxer*, const AMediaFormat*);
media_status_t AMediaMuxer_start(AMediaMuxer*); media_status_t AMediaMuxer_stop(AMediaMuxer*);
media_status_t AMediaMuxer_writeSampleData(AMediaMuxer*, size_t, const uint8_t*, const AMediaCodecBufferInfo*);
