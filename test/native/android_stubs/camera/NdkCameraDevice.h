#pragma once
#include <camera/NdkCameraMetadata.h>
#include <android/native_window.h>
struct ACameraDevice; typedef struct ACameraDevice ACameraDevice;
struct ACaptureRequest; typedef struct ACaptureRequest ACaptureRequest;
struct ACameraOutputTarget; typedef struct ACameraOutputTarget ACameraOutputTarget;
struct ACaptureSessionOutput; typedef struct ACaptureSessionOutput ACaptureSessionOutput;
struct ACaptureSessionOutputContainer; typedef struct ACaptureSessionOutputContainer ACaptureSessionOutputContainer;
struct ACameraCaptureSession; typedef struct ACameraCaptureSession ACameraCaptureSession;
typedef void (*ACameraDevice_StateCallback)(void*, ACameraDevice*);
typedef void (*ACameraDevice_ErrorStateCallback)(void*, ACameraDevice*, int);
typedef struct { void* context; ACameraDevice_StateCallback onDisconnected; ACameraDevice_ErrorStateCallback onError; } ACameraDevice_StateCallbacks;
typedef void (*ACameraCaptureSession_stateCallback)(void*, ACameraCaptureSession*);
typedef struct { void* context; ACameraCaptureSession_stateCallback onClosed, onReady, onActive; } ACameraCaptureSession_stateCallbacks;
enum { TEMPLATE_MANUAL = 6 };
typedef int ACameraDevice_request_template;
camera_status_t ACameraDevice_close(ACameraDevice*);
camera_status_t ACameraDevice_createCaptureRequest(const ACameraDevice*, ACameraDevice_request_template, ACaptureRequest**);
camera_status_t ACameraDevice_createCaptureSession(ACameraDevice*, const ACaptureSessionOutputContainer*, const ACameraCaptureSession_stateCallbacks*, ACameraCaptureSession**);
camera_status_t ACaptureSessionOutputContainer_create(ACaptureSessionOutputContainer**);
void ACaptureSessionOutputContainer_free(ACaptureSessionOutputContainer*);
camera_status_t ACaptureSessionOutput_create(ANativeWindow*, ACaptureSessionOutput**);
void ACaptureSessionOutput_free(ACaptureSessionOutput*);
camera_status_t ACaptureSessionOutputContainer_add(ACaptureSessionOutputContainer*, const ACaptureSessionOutput*);
camera_status_t ACaptureSessionOutputContainer_remove(ACaptureSessionOutputContainer*, const ACaptureSessionOutput*);
camera_status_t ACameraOutputTarget_create(ANativeWindow*, ACameraOutputTarget**);
void ACameraOutputTarget_free(ACameraOutputTarget*);
camera_status_t ACaptureRequest_addTarget(ACaptureRequest*, const ACameraOutputTarget*);
void ACaptureRequest_free(ACaptureRequest*);
camera_status_t ACaptureRequest_setEntry_u8(ACaptureRequest*, uint32_t, uint32_t, const uint8_t*);
camera_status_t ACaptureRequest_setEntry_i32(ACaptureRequest*, uint32_t, uint32_t, const int32_t*);
camera_status_t ACaptureRequest_setEntry_i64(ACaptureRequest*, uint32_t, uint32_t, const int64_t*);
camera_status_t ACaptureRequest_setEntry_float(ACaptureRequest*, uint32_t, uint32_t, const float*);
