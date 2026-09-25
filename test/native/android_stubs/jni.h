#pragma once
typedef int jint; typedef void* jobject; struct JNIEnv_; typedef JNIEnv_ JNIEnv;
#define JNIEXPORT __attribute__((visibility("default")))
#define JNICALL
