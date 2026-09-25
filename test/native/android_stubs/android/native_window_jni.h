#pragma once
#include <android/native_window.h>
#include <jni.h>
ANativeWindow* ANativeWindow_fromSurface(JNIEnv*, jobject);
