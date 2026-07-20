#pragma once
// Logcat thay cho printf của client Windows — `adb logcat -s RemoteGame`.
#include <android/log.h>

#define RGC_TAG "RemoteGame"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, RGC_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, RGC_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, RGC_TAG, __VA_ARGS__)
