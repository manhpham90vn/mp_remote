#include "rgc/Reassembler.h"

#include <iterator>

namespace rgc {

void Reassembler::Push(const VideoPacketView& pkt, uint64_t nowUs) {
    ++stats_.packetsReceived;
    if (pkt.payload.empty()) return; // Packetizer không bao giờ phát mảnh rỗng
    const uint32_t id = pkt.hdr.frameId;
    if (haveBarrier_ && id <= barrierId_) return; // gói muộn của frame đã phát/bỏ

    auto it = pending_.find(id);
    if (it == pending_.end()) {
        // Frame mới trong khi đã đầy chỗ: frame già nhất đang chặn hàng — bỏ nó.
        while (pending_.size() >= kMaxPendingFrames)
            Drop(pending_.begin(), true);
        it = pending_.emplace(id, Pending{}).first;
        Pending& f = it->second;
        f.pktCount = pkt.hdr.pktCount;
        f.pieces.resize(pkt.hdr.pktCount);
        f.timestampUs = pkt.hdr.timestampUs;
        f.firstSeenUs = nowUs;
    }

    Pending& f = it->second;
    if (pkt.hdr.pktCount != f.pktCount || pkt.hdr.pktIndex >= f.pktCount) return; // gói hỏng
    auto& slot = f.pieces[pkt.hdr.pktIndex];
    if (!slot.empty()) return; // trùng
    slot.assign(pkt.payload.begin(), pkt.payload.end());
    f.bytes += slot.size();
    f.idr = f.idr || pkt.idr;
    ++f.received;
}

std::optional<Reassembler::Frame> Reassembler::PopReady(uint64_t nowUs) {
    while (!pending_.empty()) {
        auto head = pending_.begin();
        Pending& f = head->second;

        if (f.Complete()) {
            if (waitingForIdr_ && !f.idr) { // nuốt non-IDR khi chờ IDR
                Drop(head, false);
                continue;
            }
            Frame out;
            out.frameId     = head->first;
            out.timestampUs = f.timestampUs;
            out.idr         = f.idr;
            out.nal.reserve(f.bytes);
            for (const auto& p : f.pieces)
                out.nal.insert(out.nal.end(), p.begin(), p.end());
            haveBarrier_ = true;
            barrierId_   = head->first;
            waitingForIdr_ = false;
            ++stats_.framesCompleted;
            pending_.erase(head);
            return out;
        }

        // Đầu hàng thiếu mảnh: bỏ nếu quá hạn hoặc bị ≥2 frame hoàn chỉnh mới hơn vượt mặt.
        size_t newerComplete = 0;
        for (auto n = std::next(head); n != pending_.end(); ++n)
            if (n->second.Complete()) ++newerComplete;
        if (nowUs - f.firstSeenUs > 2 * frameIntervalUs_ || newerComplete >= 2) {
            Drop(head, true);
            continue;
        }
        return std::nullopt; // còn hy vọng mảnh thiếu đang trên đường tới
    }
    return std::nullopt;
}

bool Reassembler::TakeLossEvent() {
    const bool e = lossEvent_;
    lossEvent_ = false;
    return e;
}

void Reassembler::Drop(PendingMap::iterator it, bool loss) {
    if (loss) {
        ++stats_.framesDropped;
        stats_.packetsLost += uint64_t(it->second.pktCount - it->second.received);
        lossEvent_ = true;
        ++stats_.lossEvents;
        waitingForIdr_ = true; // frame sau tham chiếu frame vừa mất → phải chờ IDR
    } else {
        ++stats_.framesSkipped;
    }
    if (!haveBarrier_ || it->first > barrierId_) {
        haveBarrier_ = true;
        barrierId_   = it->first;
    }
    pending_.erase(it);
}

} // namespace rgc
