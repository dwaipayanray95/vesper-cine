#pragma once
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraDevice.h>
struct ACameraManager; typedef struct ACameraManager ACameraManager;
typedef struct { int numCameras; const char** cameraIds; } ACameraIdList;
ACameraManager* ACameraManager_create(); void ACameraManager_delete(ACameraManager*);
camera_status_t ACameraManager_getCameraIdList(ACameraManager*, ACameraIdList**);
void ACameraManager_deleteCameraIdList(ACameraIdList*);
camera_status_t ACameraManager_getCameraCharacteristics(ACameraManager*, const char*, ACameraMetadata**);
camera_status_t ACameraManager_openCamera(ACameraManager*, const char*, ACameraDevice_StateCallbacks*, ACameraDevice**);
