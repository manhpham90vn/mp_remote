#include "rgc/InputReceiver.h"

namespace rgc {

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

} // namespace rgc
