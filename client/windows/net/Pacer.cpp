// =============================================================================
// Pacer.cpp — cài đặt việc rải gói: đồng hồ tín dụng + giấc ngủ độ phân giải cao.
//
// MÔ HÌNH: MỘT MỐC THỜI GIAN DUY NHẤT
//   Toàn bộ trạng thái là `nextUs_` — thời điểm sớm nhất được phép gửi gói kế tiếp.
//   Mỗi lần Gate():
//     1. Nếu nextUs_ đã lùi vào quá khứ thì kéo nó về hiện tại (chống tích tín dụng).
//     2. Nếu còn phải chờ đủ lâu thì ngủ.
//     3. Cộng vào nextUs_ thời gian mà `bytes` byte đáng lẽ chiếm ở tốc độ rateBps_.
//   Không có hàng đợi, không có token bucket, không cấp phát gì — chỉ một số u64.
//
// HAI QUYẾT ĐỊNH ĐÁNG CHÚ Ý, cả hai đều được giải thích tại chỗ bên dưới:
//   - kMinSleepUs = 500: không rải từng gói mà phát thành chùm nhỏ ~500 µs.
//   - Dùng waitable timer độ phân giải cao thay cho Sleep().
//
// LIÊN QUAN: net/Pacer.h (vấn đề nó giải quyết + số đo thật + cảnh báo về thread)
// =============================================================================
#include "net/Pacer.h"

#include "deskhubp/Clock.h"

namespace {

// Ngưỡng mới thèm ngủ. Chờ ngắn hơn thì cứ gửi và ghi nợ vào nextUs_, nợ dồn đủ lâu
// mới ngủ một lần — tức là ta phát thành những chùm nhỏ ~500µs thay vì rải từng gói.
// Cố rải từng gói (cỡ 300µs/gói ở 60 Mbps) chỉ tổ đốt CPU vào syscall mà chỗ thắt
// hàng đợi không phân biệt nổi.
constexpr uint64_t kMinSleepUs = 500;

} // namespace

Pacer::~Pacer() {
    if (timer_) CloseHandle(timer_);
}

// Ngủ chính xác `us` micro-giây. Timer được tạo LƯỜI và giữ lại dùng tiếp — tạo
// handle mới mỗi lần ngủ sẽ tốn hơn cả giấc ngủ.
void Pacer::SleepUs(uint64_t us) {
    // Sleep() thường của Windows bám theo tick scheduler (tệ nhất ~15.6ms) — dùng nó
    // ở đây thì pacing hỏng hoàn toàn vì ta cần độ chính xác dưới mili giây.
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+) cho đúng thứ cần mà không
    // phải bật timeBeginPeriod(1) — thứ tác động lên toàn hệ thống, không chỉ tiến
    // trình này.
    if (!timer_) {
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
    }
    if (timer_) {
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(us) * 10; // âm = thời gian tương đối, đơn vị 100ns
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer_, INFINITE);
            return;
        }
    }
    // Máy cũ không có timer độ phân giải cao: ngủ thô còn hơn không rải gì.
    Sleep(DWORD((us + 999) / 1000));
}

// Chặn tới khi được phép gửi thêm `bytes` byte. Gọi ngay trước sendto().
void Pacer::Gate(size_t bytes) {
    if (!rateBps_ || !bytes) return; // rate 0 = tắt pacing

    const uint64_t now = NowUs();

    // Đứng im một lúc (giữa hai frame, hoặc phiên vừa bắt đầu) KHÔNG được tích luỹ
    // tín dụng — nếu không, frame kế tiếp lại được bắn nguyên chùm, đúng thứ ta đang
    // tìm cách dẹp.
    if (nextUs_ < now) nextUs_ = now;

    if (const uint64_t waitUs = nextUs_ - now; waitUs >= kMinSleepUs) SleepUs(waitUs);

    // Ghi nợ: `bytes` byte ở tốc độ rateBps_ chiếm bấy nhiêu micro-giây.
    //   bytes * 8       -> bit
    //   * 1'000'000     -> đổi giây sang micro-giây
    //   / rateBps_      -> thời gian truyền
    // Nhân TRƯỚC chia để không mất phần lẻ; với gói ≤1200 byte thì tử số cỡ 1e10,
    // còn rất xa ngưỡng tràn u64.
    nextUs_ += uint64_t(bytes) * 8 * 1'000'000ull / rateBps_;
}
