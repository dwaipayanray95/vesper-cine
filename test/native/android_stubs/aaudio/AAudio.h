#pragma once
#include <cstdint>
#include <ctime>
typedef int32_t aaudio_result_t; typedef int32_t aaudio_data_callback_result_t;
struct AAudioStream; typedef struct AAudioStream AAudioStream; struct AAudioStreamBuilder; typedef struct AAudioStreamBuilder AAudioStreamBuilder;
enum { AAUDIO_OK = 0, AAUDIO_DIRECTION_INPUT = 1, AAUDIO_FORMAT_PCM_I16 = 1, AAUDIO_INPUT_PRESET_CAMCORDER = 5, AAUDIO_SHARING_MODE_SHARED = 1, AAUDIO_CALLBACK_RESULT_CONTINUE = 0 };
typedef aaudio_data_callback_result_t (*AAudioStream_dataCallback)(AAudioStream*, void*, void*, int32_t);
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder**);
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*, int32_t); void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int32_t);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int32_t); void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, int32_t);
void AAudioStreamBuilder_setInputPreset(AAudioStreamBuilder*, int32_t); void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*, int32_t);
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder*, AAudioStream_dataCallback, void*);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder*, AAudioStream**); aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder*);
int32_t AAudioStream_getSampleRate(AAudioStream*); int32_t AAudioStream_getChannelCount(AAudioStream*);
aaudio_result_t AAudioStream_requestStart(AAudioStream*); aaudio_result_t AAudioStream_requestStop(AAudioStream*); aaudio_result_t AAudioStream_close(AAudioStream*);
aaudio_result_t AAudioStream_getTimestamp(AAudioStream*, clockid_t, int64_t*, int64_t*);
