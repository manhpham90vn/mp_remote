#pragma once
// =============================================================================
// Packetizer.h — cắt frame đã mã hoá thành các datagram UDP, phía HOST.
//
// NHIỆM VỤ
//   Encoder đẻ ra một frame H.264 dạng Annex-B có thể nặng vài chục KB; UDP chỉ
//   chở an toàn ~1200 byte mỗi datagram. Packetizer cắt frame đó thành N mảnh,
//   gắn header video (frameId, timestamp, pktIndex, pktCount) cho từng mảnh, và
//   tuỳ chọn phát thêm gói parity FEC. Đối tác ở đầu kia là Reassembler.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   WindowCapture → IVideoEncoder → **Packetizer** → UDP ~~~> Reassembler
//                                                              → IVideoDecoder → Renderer
//
// VÌ SAO KHÔNG SỞ HỮU SOCKET
//   Từng datagram được giao ra ngoài qua callback `send`. Nhờ vậy cùng một mã
//   chạy được trên winsock, BSD socket, NWConnection..., và quan trọng hơn là
//   test được offline: CoreTests nối thẳng `send` của Packetizer vào Push của
//   Reassembler, thậm chí cố tình bỏ vài gói giữa chừng để kiểm chứng FEC.
//
// CÁCH CẮT MẢNH
//   Mọi mảnh trừ mảnh cuối mang ĐÚNG kMaxVideoPayload byte. Nhờ quy tắc cố định
//   này, bên nhận suy được vị trí của mảnh trong frame chỉ từ pktIndex, không cần
//   trường offset trên wire. Mảnh cuối mang cờ FrameEnd.
//
// MÔ HÌNH LUỒNG
//   Dùng trên MỘT thread (thread encode của Agent) và KHÔNG tự khoá. Bộ đệm
//   buf_/parity_ là thành viên chứ không phải biến cục bộ để tránh cấp phát trên
//   đường nóng — cái giá là mỗi thread phải có Packetizer riêng của nó.
//
// LIÊN QUAN: deskhub/transport/Reassembler.h (đầu kia), deskhub/wire/Wire.h,
//            docs/06-phase3-transport.md
// =============================================================================
#include "deskhub/wire/Wire.h"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace deskhub {

class Packetizer {
public:
    using SendFn = std::function<void(std::span<const uint8_t>)>;

    void SetSessionId(uint32_t id) {
        sessionId_ = id;
    }
    uint32_t sessionId() const {
        return sessionId_;
    }

    // GĐ5: bật/tắt gói parity FEC XEN KẼ (mặc định TẮT). Chi phí 1/kFecGroupSize băng
    // thông nên chỉ bật khi đường truyền đang thực sự mất gói — Agent bật/tắt theo FEEDBACK.
    void SetFecEnabled(bool on) {
        fec_ = on;
    }
    bool fecEnabled() const {
        return fec_;
    }

    // Cắt `nal` thành các gói ≤ kMaxVideoPayload byte payload: mọi gói trừ gói cuối
    // mang ĐÚNG kMaxVideoPayload byte (offset suy được từ pktIndex). Gọi `send` cho
    // từng datagram theo thứ tự pktIndex tăng dần; gói cuối mang cờ FrameEnd.
    // Trả về số gói đã gửi; 0 nếu frame rỗng hoặc quá lớn (> 65535 mảnh).
    size_t SendFrame(std::span<const uint8_t> nal, uint32_t frameId, uint64_t timestampUs,
        bool idr, const SendFn& send);

    // Sải bước một bộ tích luỹ parity: 2 byte lenXor + dữ liệu XOR đệm 0 tới hết nhóm.
    static constexpr size_t kParityStride = kFecLenPrefix + kMaxVideoPayload;

private:
    uint32_t sessionId_ = 0;
    bool fec_ = false;
    uint8_t buf_[kMaxDatagram] = {};
    // Nhóm xen kẽ: gói thứ i thuộc nhóm (i % numGroups), nên các nhóm chỉ đóng lại ở
    // CUỐI frame → phải giữ numGroups bộ tích luỹ song song, mỗi bộ kParityStride byte.
    // Là vector (không mảng cố định) vì numGroups phụ thuộc cỡ frame; assign() tái dùng
    // sức chứa nên đường nóng chỉ cấp phát khi gặp frame lớn hơn mọi frame trước đó.
    std::vector<uint8_t> parity_;
};

} // namespace deskhub
