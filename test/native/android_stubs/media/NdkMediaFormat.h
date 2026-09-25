#pragma once
#include <cstdint>
#include <media/NdkMediaError.h>
struct AMediaFormat; typedef struct AMediaFormat AMediaFormat;
AMediaFormat* AMediaFormat_new(); media_status_t AMediaFormat_delete(AMediaFormat*);
void AMediaFormat_setString(AMediaFormat*, const char*, const char*);
void AMediaFormat_setInt32(AMediaFormat*, const char*, int32_t);
void AMediaFormat_setFloat(AMediaFormat*, const char*, float);
bool AMediaFormat_getInt32(AMediaFormat*, const char*, int32_t*);
extern const char* AMEDIAFORMAT_KEY_MIME; extern const char* AMEDIAFORMAT_KEY_WIDTH; extern const char* AMEDIAFORMAT_KEY_HEIGHT;
extern const char* AMEDIAFORMAT_KEY_SAMPLE_RATE; extern const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT;
