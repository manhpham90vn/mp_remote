#pragma once
// =============================================================================
// InputSender.h — gom và gửi event bàn phím/chuột, phía CLIENT.
//
// NHIỆM VỤ
//   Nhận event thô từ tầng bắt input của client, đánh số thứ tự (seq) liên tục,
//   gom thành lô rồi phát thành gói INPUT_EVENT. Đối tác ở đầu kia là InputReceiver.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   InputCapture (client) → **InputSender** → UDP ~~~> InputReceiver (host)
//                                                        → InputInjector → SendInput()
//
// BÀI TOÁN CỐT LÕI: KẸT PHÍM
//   Kênh input không có ACK, mà mất gói chứa event "nhả phím" là lỗi nghiêm trọng
//   nhất trong toàn hệ thống — nhân vật trong game sẽ chạy mãi không dừng, và
//   người dùng không có cách nào sửa ngoài việc ngắt kết nối. Một khung hình mất
//   thì chớp mắt là qua; một phím kẹt thì hỏng cả phiên chơi.
//
// CHÍNH SÁCH v1: DƯ THỪA + PHÁT LẠI
//   Mỗi datagram kèm theo đuôi kInputRedundancy event đã gửi gần nhất. Sau khi hết
//   event mới, đuôi đó còn được phát lại kInputRepeatCount lần nữa, cách nhau
//   kInputRepeatIntervalUs. Mỗi event vì thế đi qua đường truyền khoảng 3 lần
//   trong ~50 ms — mất lẻ tẻ không còn ảnh hưởng gì.
//   Chi phí rất rẻ: một event chỉ 19 byte, và người ta không bấm phím nhanh tới
//   mức lấp đầy được băng thông.
//   Gửi lặp là VÔ HẠI vì InputReceiver khử trùng theo seq.
//
// MÔ HÌNH LUỒNG
//   Thuần C++20: không socket, không đồng hồ (thời gian bơm từ ngoài).
//   Dùng trên MỘT thread (thread Recv của client); không tự khoá.
//
// LIÊN QUAN: deskhub/input/InputReceiver.h (đầu kia), deskhub/session/ClientSession.h
//            (nơi gọi Queue/Flush), docs/04-protocol.md §6
// =============================================================================
//
#include "deskhub/wire/Wire.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <span>

namespace deskhub {

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

} // namespace deskhub
