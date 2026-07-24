// =============================================================================
// RetransmitCache.cpp — cài đặt kho datagram video để gửi lại theo NACK.
//
// Lớp này chỉ chép byte và tra byte, nhưng có hai điểm đáng nói:
//
//   1. Nó TỰ PHÂN TÍCH header của datagram được đưa vào (ParseCommonHeader +
//      ParseVideoPacket) để lấy frameId/pktIndex/pktCount, thay vì bắt caller truyền
//      kèm. Nhờ vậy send callback của Packetizer chỉ cần chuyển thẳng byte đã phát,
//      không phải bóc tách gì — và kho luôn nhất quán với đúng thứ đã lên đường.
//
//   2. Ring theo frame, KHÔNG theo gói: mọi mảnh của cùng một frameId dồn vào một
//      slot; chỉ khi gặp frameId MỚI mà kho đã đầy thì slot cũ nhất mới bị ghi đè.
//      Thế nên một frame nhiều mảnh không tự đẩy chính các mảnh trước của nó ra.
//
// LIÊN QUAN: deskhub/transport/RetransmitCache.h (thiết kế + lý do), Wire.cpp
// =============================================================================
#include "deskhub/transport/RetransmitCache.h"

namespace deskhub {

RetransmitCache::FrameSlot* RetransmitCache::FindSlot(uint32_t frameId) {
    for (FrameSlot& s : slots_)
        if (s.used && s.frameId == frameId) return &s;
    return nullptr;
}

const RetransmitCache::FrameSlot* RetransmitCache::FindSlot(uint32_t frameId) const {
    for (const FrameSlot& s : slots_)
        if (s.used && s.frameId == frameId) return &s;
    return nullptr;
}

void RetransmitCache::Store(std::span<const uint8_t> datagram) {
    const auto h = ParseCommonHeader(datagram);
    if (!h || h->type != MsgType::VideoPacket) return; // chỉ giữ gói dữ liệu video
    const auto v = ParseVideoPacket(*h, PayloadOf(datagram));
    if (!v) return;

    FrameSlot* slot = FindSlot(v->hdr.frameId);
    if (!slot) {
        // Frame mới: chiếm slot cũ nhất trong ring và dọn sạch nó.
        slot = &slots_[next_];
        next_ = (next_ + 1) % kCacheFrames;
        slot->frameId = v->hdr.frameId;
        slot->used = true;
        slot->packets.assign(v->hdr.pktCount, {});
    } else if (slot->packets.size() < v->hdr.pktCount) {
        slot->packets.resize(v->hdr.pktCount); // pktCount lẽ ra cố định, phòng gói hỏng
    }

    if (v->hdr.pktIndex < slot->packets.size())
        slot->packets[v->hdr.pktIndex].assign(datagram.begin(), datagram.end());
}

std::span<const uint8_t> RetransmitCache::Find(uint32_t frameId, uint16_t pktIndex) const {
    const FrameSlot* slot = FindSlot(frameId);
    if (!slot || pktIndex >= slot->packets.size()) return {};
    const std::vector<uint8_t>& d = slot->packets[pktIndex];
    return std::span<const uint8_t>(d.data(), d.size());
}

void RetransmitCache::Reset() {
    for (FrameSlot& s : slots_) {
        s.used = false;
        s.frameId = 0;
        s.packets.clear();
    }
    next_ = 0;
}

} // namespace deskhub
