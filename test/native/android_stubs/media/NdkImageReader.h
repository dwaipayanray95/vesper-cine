#pragma once
#include <media/NdkImage.h>
#include <android/native_window.h>
struct AImageReader; typedef struct AImageReader AImageReader;
typedef void (*AImageReader_ImageCallback)(void*, AImageReader*);
typedef struct { void* context; AImageReader_ImageCallback onImageAvailable; } AImageReader_ImageListener;
media_status_t AImageReader_newWithUsage(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**);
media_status_t AImageReader_setImageListener(AImageReader*, AImageReader_ImageListener*);
media_status_t AImageReader_getWindow(AImageReader*, ANativeWindow**);
media_status_t AImageReader_acquireNextImage(AImageReader*, AImage**);
void AImageReader_delete(AImageReader*);
