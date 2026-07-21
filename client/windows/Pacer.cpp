#include "Pacer.h"

#include "TimeUs.h"

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

void Pacer::Gate(size_t bytes) {
    if (!rateBps_ || !bytes) return;

    const uint64_t now = QpcUs();

    // Đứng im một lúc (giữa hai frame, hoặc phiên vừa bắt đầu) KHÔNG được tích luỹ
    // tín dụng — nếu không, frame kế tiếp lại được bắn nguyên chùm, đúng thứ ta đang
    // tìm cách dẹp.
    if (nextUs_ < now) nextUs_ = now;

    if (const uint64_t waitUs = nextUs_ - now; waitUs >= kMinSleepUs) SleepUs(waitUs);

    nextUs_ += uint64_t(bytes) * 8 * 1'000'000ull / rateBps_;
}
