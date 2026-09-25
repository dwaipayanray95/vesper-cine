#pragma once
#include <cstdint>
#include <media/NdkMediaError.h>
struct AImage; typedef struct AImage AImage;
enum { AIMAGE_FORMAT_RAW10 = 0x25 };
media_status_t AImage_getTimestamp(const AImage*, int64_t*);
media_status_t AImage_getWidth(const AImage*, int32_t*);
media_status_t AImage_getHeight(const AImage*, int32_t*);
media_status_t AImage_getPlaneData(const AImage*, int, uint8_t**, int*);
media_status_t AImage_getPlaneRowStride(const AImage*, int, int32_t*);
void AImage_delete(AImage*);
