#pragma once
// Packetizer — cắt một NAL frame (Annex-B) thành N gói VIDEO_PACKET ≤ kMaxDatagram.
// Thuần C++20, không sở hữu socket: từng datagram được giao ra ngoài qua callback
// `send` (caller quyết định gửi đi đâu — winsock, BSD socket, NWConnection...).
// Dùng trên MỘT thread (thread encode của Agent); không tự khóa.
#include "rgc/Wire.h"

#include <cstdint>
#include <functional>
#include <span>

namespace rgc {

class Packetizer {
public:
    using SendFn = std::function<void(std::span<const uint8_t>)>;

    void SetSessionId(uint32_t id) { sessionId_ = id; }
    uint32_t sessionId() const { return sessionId_; }

    // Cắt `nal` thành các gói ≤ kMaxVideoPayload byte payload: mọi gói trừ gói cuối
    // mang ĐÚNG kMaxVideoPayload byte (offset suy được từ pktIndex). Gọi `send` cho
    // từng datagram theo thứ tự pktIndex tăng dần; gói cuối mang cờ FrameEnd.
    // Trả về số gói đã gửi; 0 nếu frame rỗng hoặc quá lớn (> 65535 mảnh).
    size_t SendFrame(std::span<const uint8_t> nal, uint32_t frameId, uint64_t timestampUs,
                     bool idr, const SendFn& send);

private:
    uint32_t sessionId_ = 0;
    uint8_t  buf_[kMaxDatagram] = {};
};

} // namespace rgc
