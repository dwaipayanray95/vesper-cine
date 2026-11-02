#pragma once
// Stand-in for NDK <android/sensor.h> (signatures from the NDK reference).
#include <android/looper.h>
#include <stdint.h>
#include <sys/types.h>
extern "C" {
enum { ASENSOR_TYPE_ACCELEROMETER = 1, ASENSOR_TYPE_GYROSCOPE = 4, ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED = 16 };
struct ASensorManager; typedef struct ASensorManager ASensorManager;
struct ASensorEventQueue; typedef struct ASensorEventQueue ASensorEventQueue;
struct ASensor; typedef struct ASensor ASensor;
typedef ASensor const* ASensorRef;
typedef int (*ALooper_callbackFunc)(int fd, int events, void* data);
typedef struct ASensorEvent {
    int32_t version;
    int32_t sensor;
    int32_t type;
    int32_t reserved0;
    int64_t timestamp;
    union { float data[16]; } ;
    int32_t flags;
    int32_t reserved1[3];
} ASensorEvent;
ASensorManager* ASensorManager_getInstanceForPackage(const char* packageName);
ASensor const* ASensorManager_getDefaultSensor(ASensorManager* manager, int type);
ASensorEventQueue* ASensorManager_createEventQueue(ASensorManager* manager, ALooper* looper, int ident,
                                                   ALooper_callbackFunc callback, void* data);
int ASensorManager_destroyEventQueue(ASensorManager* manager, ASensorEventQueue* queue);
int ASensorEventQueue_registerSensor(ASensorEventQueue* queue, ASensor const* sensor, int32_t samplingPeriodUs,
                                     int64_t maxBatchReportLatencyUs);
int ASensorEventQueue_flush(ASensorEventQueue* queue);
int ASensorEventQueue_disableSensor(ASensorEventQueue* queue, ASensor const* sensor);
ssize_t ASensorEventQueue_getEvents(ASensorEventQueue* queue, ASensorEvent* events, size_t count);
const char* ASensor_getName(ASensor const* sensor);
int ASensor_getType(ASensor const* sensor);
int ASensor_getMinDelay(ASensor const* sensor);
}
