#pragma once
// =============================================================================
// RetransmitCache.h — bộ nhớ đệm datagram video gần đây để GỬI LẠI theo NACK, phía HOST.
//
// NHIỆM VỤ
//   Khi client phát hiện thiếu mảnh của một frame, nó gửi NACK xin gửi lại đúng các
//   mảnh đó (xem Wire.h MsgType::Nack). Host không giữ lại encoder output để dựng lại
//   được, nên phải nhớ NGUYÊN VĂN các datagram vừa phát. Lớp này là cái kho đó: mỗi
//   datagram video đi ra được chép vào đây, và tra lại được bằng (frameId, pktIndex).
//
// VÌ SAO NACK BÙ CHO FEC
//   FEC XOR chỉ cứu 1 mảnh mỗi nhóm; chùm mất ≥2 mảnh cùng nhóm thì chịu. Nếu RTT đủ
//   nhỏ so với hạn ghép frame (LAN/Wi-Fi tốt: RTT vài ms, hạn ~33ms @60fps), gửi lại
//   đúng mảnh thiếu cứu được MỌI kiểu mất mà chỉ tốn băng thông khi thật sự mất — khác
//   FEC luôn tốn 1/8. Hai cơ chế bù nhau: FEC lo mất lẻ không thêm trễ, NACK lo phần
//   FEC không gánh nổi khi RTT cho phép.
//
// GIỚI HẠN BỘ NHỚ
//   Chỉ giữ kCacheFrames frame gần nhất (ring theo thứ tự nạp). Mảnh cũ hơn thế thì
//   frame của nó đã quá hạn ghép ở client rồi, gửi lại vô nghĩa. Mỗi frame giữ tối đa
//   số mảnh của nó; toàn bộ là vector tái dùng, không cấp phát trên đường nóng sau khi
//   đã "nóng máy".
//
// MÔ HÌNH LUỒNG
//   Store() gọi từ thread phát video (send callback của Packetizer). Find() gọi từ
//   thread Recv (xử lý NACK). Hai thread KHÁC nhau → caller phải tự khoá quanh cặp
//   Store/Find, hoặc gọi cả hai trên cùng một thread. Lớp này KHÔNG tự khoá (giữ đúng
//   kỷ luật "core không biết mô hình luồng của nền tảng").
//
// LIÊN QUAN: deskhub/wire/Wire.h (MsgType::Nack, ParseNack), Packetizer.h (nguồn datagram)
// =============================================================================
#include "deskhub/wire/Wire.h"

#include <cstdint>
#include <span>
#include <vector>

namespace deskhub {

class RetransmitCache {
public:
    // Số frame gần nhất được giữ lại. 8 frame ≈ 0.13s @60fps — dài hơn hạn ghép của
    // client (2 khoảng frame) nên phủ trọn cửa sổ mà NACK còn có ý nghĩa.
    static constexpr size_t kCacheFrames = 8;

    // Chép nguyên văn một datagram VIDEO_PACKET vào kho. Bỏ qua nếu không phải gói
    // video hợp lệ (gói FEC/control không cần gửi lại: parity dựng lại được, control
    // đã có cơ chế phát lại riêng).
    void Store(std::span<const uint8_t> datagram);

    // Tra datagram của mảnh (frameId, pktIndex). Rỗng nếu không còn giữ (đã bị đẩy ra
    // khỏi ring, hoặc chưa từng phát). Span trỏ vào bộ đệm nội bộ — chép/gửi ngay,
    // đừng giữ qua lần Store kế tiếp.
    std::span<const uint8_t> Find(uint32_t frameId, uint16_t pktIndex) const;

    void Reset(); // phiên mới

private:
    struct FrameSlot {
        uint32_t frameId = 0;
        bool used = false;
        std::vector<std::vector<uint8_t>> packets; // theo pktIndex; rỗng = chưa nạp
    };

    // Tìm slot đang giữ frameId; nullptr nếu không có.
    FrameSlot* FindSlot(uint32_t frameId);
    const FrameSlot* FindSlot(uint32_t frameId) const;

    FrameSlot slots_[kCacheFrames];
    size_t next_ = 0; // slot sẽ bị ghi đè khi gặp frame mới (ring)
};

} // namespace deskhub
