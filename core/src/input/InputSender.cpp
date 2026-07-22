// =============================================================================
// InputSender.cpp — cài đặt hàng đợi event và chính sách gửi dư thừa.
//
// CẤU TRÚC DỮ LIỆU
//   history_ là một deque chứa các event ĐÃ ĐƯỢC GÁN SEQ, xếp từ cũ tới mới. Nó
//   vừa là hàng đợi chờ gửi, vừa là bộ nhớ đệm cho phần dư thừa — hai vai trò này
//   phân định bằng con trỏ `unsent_`:
//
//       history_:  [ ... đã gửi ... | ... chưa gửi ... ]
//                                   └── unsent_ phần tử cuối
//
//   Phần "đã gửi" không bị vứt ngay, mà giữ lại kInputRedundancy phần tử để kèm
//   vào gói kế tiếp. Phần cũ hơn nữa mới bị cắt bỏ ở Queue().
//
// QUY ƯỚC SEQ
//   firstSeq_ là seq của history_.front(). Vì deque luôn liên tục và seq cấp tăng
//   đều, seq của phần tử thứ i luôn là firstSeq_ + i — không cần lưu seq trong
//   từng InputEvent. Mọi phép tính chỉ số trong file này đều dựa vào bất biến đó,
//   nên mỗi lần pop_front phải ++firstSeq_ đi kèm.
//
// LIÊN QUAN: deskhub/input/InputSender.h (chính sách + lý do), InputReceiver.cpp
// =============================================================================
#include "deskhub/input/InputSender.h"

#include <algorithm>

namespace deskhub {

namespace {
// Giữ đủ lịch sử cho redundancy + một batch; cắt bớt để hàng đợi không phình.
constexpr size_t kHistoryMax = kInputRedundancy + kInputBatchMax * 2;
} // namespace

// Xếp một event vào hàng đợi. KHÔNG gửi gì cả — việc gửi dồn hết vào Flush để một
// datagram chở được nhiều event, thay vì mỗi cái một gói (header 8 byte cho event
// 19 byte là quá phí, mà chuột di chuyển sinh event dày đặc).
void InputSender::Queue(const InputEvent& e) {
    // Hàng đợi rỗng: phần tử sắp thêm sẽ là front, nên nó định nghĩa firstSeq_ mới.
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

// Đóng gói và phát các event history_[from .. to). Cả hai đều là CHỈ SỐ trong
// history_, không phải seq — seq tương ứng được suy ra là firstSeq_ + from.
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

// Gọi mỗi vòng lặp của client. Hai chế độ tách bạch: có event mới thì gửi chúng
// đi, không có thì phát lại đuôi lịch sử (nhưng chỉ vài lần rồi im, không phát lại
// vô hạn — quota nằm ở repeatsLeft_).
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
    //
    // Chỉ số trong một vòng lặp (đều là chỉ số trong history_):
    //
    //     [ ......... đã gửi ......... | ....... chưa gửi ....... ]
    //                    ^from          ^newFrom      ^newFrom+batch
    //                    └─ dư thừa ────┘└──── lô mới ───┘
    //                       (≤ kInputRedundancy)  (≤ kInputBatchMax)
    //
    // Gói phát đi phủ [from, newFrom+batch): phần đầu là bản lặp để bù gói mất,
    // phần sau là event chưa ai thấy bao giờ.
    size_t sent = 0;
    while (unsent_ > 0) {
        const size_t total    = history_.size();
        const size_t newFrom  = total - unsent_;              // ranh giới đã/chưa gửi
        const size_t batch    = std::min(unsent_, kInputBatchMax);
        // std::min trước khi trừ: newFrom có thể nhỏ hơn kInputRedundancy (lúc mới
        // bắt đầu phiên, chưa gửi gì), và đây là số không dấu nên trừ thẳng sẽ tràn
        // xuống một con số khổng lồ.
        const size_t from     = newFrom - std::min(newFrom, kInputRedundancy);
        sent += SendRange(from, newFrom + batch, send);
        unsent_ -= batch;
    }
    // Đã có dữ liệu mới đi ra → nạp lại quota phát lại, tính từ mốc thời gian này.
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

} // namespace deskhub
