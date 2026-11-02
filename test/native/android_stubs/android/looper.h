#pragma once
// Stand-in for NDK <android/looper.h> (signatures from the NDK reference).
extern "C" {
struct ALooper; typedef struct ALooper ALooper;
enum { ALOOPER_PREPARE_ALLOW_NON_CALLBACKS = 1 << 0 };
ALooper* ALooper_prepare(int opts);
int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData);
void ALooper_wake(ALooper* looper);
}
