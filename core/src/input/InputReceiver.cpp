// =============================================================================
// InputReceiver.cpp — cài đặt bộ lọc trùng theo seq.
//
// File rất ngắn, nhưng có hai chi tiết đáng dừng lại:
//
//   1. lastAppliedSeq_ là int64_t chứ không phải uint32_t, để mang được giá trị -1
//      nghĩa là "chưa nhận event nào". Dùng số không dấu thì không có giá trị nào
//      biểu diễn được trạng thái đó — 0 là một seq hợp lệ (seq đầu tiên của phiên).
//      Mọi phép so sánh vì thế cũng làm trên int64_t để tránh so sánh lẫn dấu.
//
//   2. Mảng `events` nằm trên stack, kích thước cố định kMaxInputEvents. Hàm này
//      chạy trên đường nóng của mọi thao tác chuột/phím nên không cấp phát động;
//      kMaxInputEvents được suy từ MTU nên một gói hợp lệ không bao giờ vượt quá.
//
// LIÊN QUAN: deskhub/input/InputReceiver.h (cơ chế + lý do), InputSender.cpp
// =============================================================================
#include "deskhub/input/InputReceiver.h"

namespace deskhub {

// Giải một gói INPUT_EVENT rồi phát từng event chưa từng thấy qua `apply`.
// Trả true nếu gói hợp lệ — KỂ CẢ khi mọi event trong đó đều là bản lặp và không
// có gì được áp dụng. Người gọi (HostSession) dùng giá trị này để nuôi timeout
// phiên, mà gói lặp cũng là bằng chứng client còn sống.
bool InputReceiver::HandlePacket(std::span<const uint8_t> payload, const ApplyFn& apply) {
    InputEvent events[kMaxInputEvents];
    uint32_t firstSeq = 0;
    const size_t n = ParseInputEvents(payload, firstSeq, events);
    if (!n) return false;
    ++stats_.packets;

    for (size_t i = 0; i < n; ++i) {
        const int64_t seq = int64_t(firstSeq) + int64_t(i);
        if (seq <= lastAppliedSeq_) { ++stats_.duplicates; continue; }
        // Lỗ hổng: các seq giữa lastApplied và seq này không gói nào mang tới.
        if (lastAppliedSeq_ >= 0 && seq > lastAppliedSeq_ + 1)
            stats_.lost += uint64_t(seq - lastAppliedSeq_ - 1);
        lastAppliedSeq_ = seq;
        ++stats_.applied;
        if (apply) apply(events[i]);
    }
    return true;
}

void InputReceiver::Reset() {
    lastAppliedSeq_ = -1;
    stats_ = Stats{};
}

} // namespace deskhub
