#pragma once
#include <camera/NdkCameraDevice.h>
typedef void (*ACameraCaptureSession_captureCallback_result)(void*, ACameraCaptureSession*, ACaptureRequest*, const ACameraMetadata*);
typedef struct { void* context; void* onCaptureStarted; void* onCaptureProgressed; ACameraCaptureSession_captureCallback_result onCaptureCompleted; void* a; void* b; void* c; void* d; } ACameraCaptureSession_captureCallbacks;
camera_status_t ACameraCaptureSession_setRepeatingRequest(ACameraCaptureSession*, ACameraCaptureSession_captureCallbacks*, int, ACaptureRequest**, int*);
camera_status_t ACameraCaptureSession_stopRepeating(ACameraCaptureSession*);
camera_status_t ACameraCaptureSession_abortCaptures(ACameraCaptureSession*);
void ACameraCaptureSession_close(ACameraCaptureSession*);
