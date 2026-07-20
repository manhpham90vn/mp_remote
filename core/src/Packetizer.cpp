#include "rgc/Packetizer.h"

namespace rgc {

size_t Packetizer::SendFrame(std::span<const uint8_t> nal, uint32_t frameId,
                             uint64_t timestampUs, bool idr, const SendFn& send) {
    if (nal.empty() || !send) return 0;
    const size_t count = (nal.size() + kMaxVideoPayload - 1) / kMaxVideoPayload;
    if (count > 0xFFFF) return 0;

    VideoHeader vh;
    vh.frameId     = frameId;
    vh.timestampUs = timestampUs;
    vh.pktCount    = uint16_t(count);

    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * kMaxVideoPayload;
        const size_t len = (nal.size() - off < kMaxVideoPayload) ? nal.size() - off
                                                                 : kMaxVideoPayload;
        vh.pktIndex = uint16_t(i);
        const bool frameEnd = (i + 1 == count);
        const size_t n = BuildVideoPacket(buf_, sessionId_, vh, idr, frameEnd,
                                          nal.subspan(off, len));
        if (!n) return 0;
        send(std::span<const uint8_t>(buf_, n));
    }
    return count;
}

} // namespace rgc
