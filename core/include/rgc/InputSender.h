#pragma once
// InputSender — gom event input phía client thành gói INPUT_EVENT (§6 của
// docs/04-protocol.md), đánh seq liên tục và GỬI LẶP để chống kẹt phím.
//
// Vì sao lặp: kênh input không có ACK. Mất gói chứa event "nhả phím" là lỗi
// nghiêm trọng nhất (nhân vật chạy mãi). Chính sách v1: mỗi datagram kèm theo
// đuôi các event đã gửi gần nhất (redundancy), và sau khi hết event mới thì
// phát lại đuôi đó thêm vài lần cách nhau kRepeatIntervalUs. Mỗi event vì thế
// đi qua đường truyền ~3 lần trong ~50 ms — mất lẻ tẻ không còn ảnh hưởng.
// Bên nhận (InputReceiver) khử trùng bằng seq nên gửi lặp là vô hại.
//
// Thuần C++20: không socket, không đồng hồ (thời gian bơm từ ngoài).
// Dùng trên MỘT thread (thread Recv của client); không tự khoá.
#include "rgc/Wire.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <span>

namespace rgc {

inline constexpr uint64_t kInputRepeatIntervalUs = 25'000; // giãn cách phát lại đuôi
inline constexpr uint32_t kInputRepeatCount      = 2;      // số lần phát lại sau lần đầu
inline constexpr size_t   kInputRedundancy       = 8;      // số event cũ kèm mỗi gói
inline constexpr size_t   kInputBatchMax         = 24;     // event mới tối đa mỗi gói

class InputSender {
public:
    using SendFn = std::function<void(std::span<const uint8_t>)>;

    void SetSessionId(uint32_t id) { sessionId_ = id; }

    // Xếp một event vào hàng đợi (chưa gửi). Gán seq tăng dần.
    void Queue(const InputEvent& e);

    // Gửi event đang chờ; nếu không có, phát lại đuôi tối đa kInputRepeatCount lần.
    // Trả về số datagram đã gửi.
    size_t Flush(uint64_t nowUs, const SendFn& send);

    // Quên toàn bộ lịch sử (phiên mới).
    void Reset();

    uint32_t nextSeq() const { return nextSeq_; }
    bool     pending() const { return unsent_ > 0; }

private:
    // Gửi một datagram gồm các event history_[from .. to) (chỉ số trong history_).
    size_t SendRange(size_t from, size_t to, const SendFn& send);

    std::deque<InputEvent> history_; // event đã gán seq, cũ → mới
    uint32_t nextSeq_    = 0;        // seq cấp cho event kế tiếp
    uint32_t firstSeq_   = 0;        // seq của history_.front()
    size_t   unsent_     = 0;        // số event ở CUỐI history_ chưa từng gửi
    uint32_t repeatsLeft_ = 0;
    uint64_t lastSendUs_  = 0;
    uint32_t sessionId_   = 0;
    uint8_t  buf_[kMaxDatagram] = {};
};

} // namespace rgc
