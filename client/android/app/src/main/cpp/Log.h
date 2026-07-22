#pragma once
// =============================================================================
// Log.h — ghi log ra logcat, thay cho printf của client Windows.
//
// NHIỆM VỤ
//   Trên Android không có console để printf chảy vào, nên mọi thứ đi qua logcat.
//   Ba macro dưới đây là toàn bộ cơ chế log của phần C++ trong app.
//
// CÁCH XEM
//   adb logcat -s Deskhub
//   Cờ -s lọc theo tag, bỏ hết log của hệ thống và app khác.
//
// VÌ SAO LÀ MACRO CHỨ KHÔNG PHẢI HÀM
//   Cần chuyển tiếp danh sách tham số biến đổi (kiểu printf) xuống
//   __android_log_print. Viết bằng hàm thì phải qua va_list và mất luôn khả năng
//   kiểm tra định dạng của trình dịch. Macro giữ nguyên được cả hai.
//
// LƯU Ý KHI DÙNG
//   Chuỗi định dạng theo chuẩn printf, nên các kiểu có độ rộng thay đổi giữa 32 và
//   64 bit (uint64_t, size_t) phải dùng macro của <cinttypes> — ví dụ PRIu64 —
//   chứ không viết thẳng %llu. Android build cho cả arm64 lẫn armeabi-v7a, viết
//   cứng sẽ sai trên một trong hai kiến trúc.
//
//   Đây là đường log, KHÔNG phải đường báo lỗi cho người dùng. Việc gì người dùng
//   cần biết thì phải đi qua ClientLoop::EndReason/StatusLine rồi lên UI.
//
// LIÊN QUAN: ClientLoop.cpp, decode/MediaCodecDecoder.cpp, net/*.cpp (nơi dùng)
// =============================================================================
#include <android/log.h>

#define DESKHUB_TAG "Deskhub"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, DESKHUB_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, DESKHUB_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, DESKHUB_TAG, __VA_ARGS__)
