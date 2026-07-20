#include "rgc/InputSender.h"

#include <algorithm>

namespace rgc {

namespace {
// Giữ đủ lịch sử cho redundancy + một batch; cắt bớt để hàng đợi không phình.
constexpr size_t kHistoryMax = kInputRedundancy + kInputBatchMax * 2;
} // namespace

void InputSender::Queue(const InputEvent& e) {
    if (history_.empty()) firstSeq_ = nextSeq_;
    history_.push_back(e);
    ++nextSeq_;
    ++unsent_;
    // Cắt phần đầu đã gửi và không còn dùng cho redundancy nữa.
    while (history_.size() > kHistoryMax && history_.size() - unsent_ > kInputRedundancy) {
        history_.pop_front();
        ++firstSeq_;
    }
}

size_t InputSender::SendRange(size_t from, size_t to, const SendFn& send) {
    if (from >= to) return 0;
    const size_t n = std::min(to - from, kMaxInputEvents);
    // deque không liên tục trong bộ nhớ → copy sang mảng tạm trên stack.
    InputEvent tmp[kMaxInputEvents];
    for (size_t i = 0; i < n; ++i) tmp[i] = history_[from + i];
    const size_t bytes = BuildInputEvents(buf_, sessionId_, firstSeq_ + uint32_t(from),
                                          std::span<const InputEvent>(tmp, n));
    if (!bytes) return 0;
    send(std::span<const uint8_t>(buf_, bytes));
    return 1;
}

size_t InputSender::Flush(uint64_t nowUs, const SendFn& send) {
    if (history_.empty() || !send) return 0;

    if (unsent_ == 0) {
        // Không có gì mới: phát lại đuôi để bù gói mất (chỉ vài lần rồi thôi).
        if (!repeatsLeft_ || nowUs - lastSendUs_ < kInputRepeatIntervalUs) return 0;
        --repeatsLeft_;
        lastSendUs_ = nowUs;
        const size_t to = history_.size();
        return SendRange(to - std::min(to, kInputRedundancy), to, send);
    }

    // Có event mới: gửi theo lô, mỗi lô kèm đuôi event đã gửi (redundancy).
    size_t sent = 0;
    while (unsent_ > 0) {
        const size_t total    = history_.size();
        const size_t newFrom  = total - unsent_;
        const size_t batch    = std::min(unsent_, kInputBatchMax);
        const size_t from     = newFrom - std::min(newFrom, kInputRedundancy);
        sent += SendRange(from, newFrom + batch, send);
        unsent_ -= batch;
    }
    repeatsLeft_ = kInputRepeatCount;
    lastSendUs_  = nowUs;
    return sent;
}

void InputSender::Reset() {
    history_.clear();
    nextSeq_ = 0;
    firstSeq_ = 0;
    unsent_ = 0;
    repeatsLeft_ = 0;
    lastSendUs_ = 0;
}

} // namespace rgc
