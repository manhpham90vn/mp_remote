#pragma once
// =============================================================================
// Log.h — ghi log ra stderr (Xcode console), bản iOS. Đối ứng Log.h của Android.
//
// NHIỆM VỤ
//   iOS không có logcat; stderr chảy thẳng vào console của Xcode và vào Console.app
//   khi chạy trên thiết bị. Ba macro dưới đây là toàn bộ cơ chế log của phần C++.
//
// VÌ SAO fprintf CHỨ KHÔNG os_log
//   Call site giữ nguyên chuỗi định dạng kiểu printf từ bản Android (LOGI("...%d")).
//   os_log dùng chuỗi định dạng riêng, không tương thích trực tiếp — dùng fprintf
//   giữ được toàn bộ call site không sửa, đổi lại mất phần lọc theo subsystem.
//
// LƯU Ý KHI DÙNG
//   Kiểu có độ rộng thay đổi (uint64_t, size_t) phải dùng macro <cinttypes> (PRIu64)
//   chứ không viết thẳng %llu — iOS build cho arm64 (thiết bị) lẫn x86_64/arm64
//   (Simulator). Đây là đường log, KHÔNG phải đường báo lỗi cho người dùng: việc gì
//   người dùng cần biết phải đi qua ClientLoop::EndReason/StatusLine rồi lên UI.
//
// LIÊN QUAN: ClientLoop.cpp, VtDecoder.mm, net/*.cpp (nơi dùng),
//            client/android/.../Log.h (bản song song trên logcat)
// =============================================================================
#include <cstdio>

#define DESKHUB_TAG "[Deskhub] "
#define LOGI(...)                          \
    do {                                   \
        std::fprintf(stderr, DESKHUB_TAG); \
        std::fprintf(stderr, __VA_ARGS__); \
        std::fprintf(stderr, "\n");        \
    } while (0)
#define LOGW(...) LOGI(__VA_ARGS__)
#define LOGE(...) LOGI(__VA_ARGS__)
