#pragma once

#ifdef __OHOS__

// LOG_DOMAIN/LOG_TAG must precede <hilog/log.h> (it falls back to LOG_TAG NULL).
#define LOG_DOMAIN 0xC001
#define LOG_TAG "CSGO"
#include <hilog/log.h>
#include <stdarg.h>
#include <stdio.h>

// 接收 va_list（调用方 android_main 的 log() 已 va_start）。
// OH_LOG_Print 本身是变参函数，无法转发 va_list，用 OH_LOG_PrintMsg 输出预格式化消息。
static inline void ohos_log(const char* format, va_list args) {
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), format, args);
    OH_LOG_PrintMsg(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, buffer);
}

#define PLATFORM_LOG_VPRINT(format, args) ohos_log(format, args)
#define PLATFORM_LOG_INFO(tag, format, ...) ohos_log(format, ##__VA_ARGS__)

#define PLATFORM_INTERNAL_STORAGE SDL_GetOpenHarmonyInternalStoragePath()
#define PLATFORM_WINDOW_POINTER SDL_PROP_WINDOW_OPENHARMONY_WINDOW_POINTER

#define PLATFORM_HINT_BLOCK_ON_PAUSE ""

#else

#include <android/log.h>

#define PLATFORM_LOG_VPRINT(format, args) __android_log_vprint(ANDROID_LOG_INFO, "CSGO", format, args)
#define PLATFORM_LOG_INFO(tag, format, ...) __android_log_print(ANDROID_LOG_INFO, tag, format, ##__VA_ARGS__)

#define PLATFORM_INTERNAL_STORAGE SDL_GetAndroidInternalStoragePath()
#define PLATFORM_WINDOW_POINTER SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER

#define PLATFORM_HINT_BLOCK_ON_PAUSE SDL_HINT_ANDROID_BLOCK_ON_PAUSE

#endif