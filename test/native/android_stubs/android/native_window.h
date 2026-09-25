// Minimal host stand-in for <android/native_window.h>, used only by
// test/native/run_tests.sh to syntax-check platform-independent native code.
#pragma once
#include <cstdint>
extern "C" {
struct ANativeWindow;
typedef struct ANativeWindow ANativeWindow;
struct AHardwareBuffer;
typedef struct ARect { int32_t left, top, right, bottom; } ARect;
typedef struct ANativeWindow_Buffer { int32_t width, height, stride, format; void* bits; uint32_t reserved[6]; } ANativeWindow_Buffer;
enum { WINDOW_FORMAT_RGBA_8888 = 1 };
void ANativeWindow_acquire(ANativeWindow*);
void ANativeWindow_release(ANativeWindow*);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow*, int32_t, int32_t, int32_t);
int32_t ANativeWindow_lock(ANativeWindow*, ANativeWindow_Buffer*, ARect*);
int32_t ANativeWindow_unlockAndPost(ANativeWindow*);

}
