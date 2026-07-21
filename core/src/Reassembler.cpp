#include "rgc/Reassembler.h"

#include <iterator>

namespace rgc {

Reassembler::Pending* Reassembler::Slot(uint32_t id, uint16_t pktCount,
                                        uint64_t timestampUs, uint64_t nowUs) {
    if (haveBarrier_ && id <= barrierId_) return nullptr; // gói muộn của frame đã phát/bỏ

    auto it = pending_.find(id);
    if (it == pending_.end()) {
        // Frame mới trong khi đã đầy chỗ: frame già nhất đang chặn hàng — bỏ nó.
        while (pending_.size() >= kMaxPendingFrames)
            Drop(pending_.begin(), true);
        it = pending_.emplace(id, Pending{}).first;
        Pending& f = it->second;
        f.pktCount = pktCount;
        f.pieces.resize(pktCount);
        f.timestampUs = timestampUs;
        f.firstSeenUs = nowUs;
    }
    if (it->second.pktCount != pktCount) return nullptr; // gói hỏng / không khớp frame
    return &it->second;
}

void Reassembler::Push(const VideoPacketView& pkt, uint64_t nowUs) {
    ++stats_.packetsReceived;
    if (pkt.payload.empty()) return; // Packetizer không bao giờ phát mảnh rỗng

    Pending* fp = Slot(pkt.hdr.frameId, pkt.hdr.pktCount, pkt.hdr.timestampUs, nowUs);
    if (!fp) return;
    Pending& f = *fp;
    if (pkt.hdr.pktIndex >= f.pktCount) return; // gói hỏng
    auto& slot = f.pieces[pkt.hdr.pktIndex];
    if (!slot.empty()) return; // trùng
    slot.assign(pkt.payload.begin(), pkt.payload.end());
    f.bytes += slot.size();
    f.idr = f.idr || pkt.idr;
    ++f.received;

    // Gói này có thể là mảnh cuối nhóm cần để parity (đã tới trước) dùng được.
    TryRecover(f, uint8_t(pkt.hdr.pktIndex / kFecGroupSize));
}

void Reassembler::PushFec(const FecPacketView& pkt, uint64_t nowUs) {
    ++stats_.fecReceived;
    if (pkt.parity.size() < kFecLenPrefix) return;

    Pending* fp = Slot(pkt.hdr.frameId, pkt.hdr.pktCount, pkt.hdr.timestampUs, nowUs);
    if (!fp) return;
    Pending& f = *fp;
    f.idr = f.idr || pkt.idr;

    auto& slot = f.parity[pkt.hdr.groupIndex];
    if (!slot.empty()) return; // trùng
    slot.assign(pkt.parity.begin(), pkt.parity.end());
    TryRecover(f, pkt.hdr.groupIndex);
}

bool Reassembler::TryRecover(Pending& f, uint8_t group) {
    auto pit = f.parity.find(group);
    if (pit == f.parity.end()) return false;
    const std::vector<uint8_t>& par = pit->second;

    const size_t first = size_t(group) * kFecGroupSize;
    if (first >= f.pktCount) return false;
    size_t last = first + kFecGroupSize;
    if (last > f.pktCount) last = f.pktCount;

    // Parity XOR chỉ gỡ được MỘT ẩn số. Không thiếu gói nào thì thôi, thiếu ≥2 thì chịu.
    size_t missing = 0, missingIdx = 0;
    for (size_t i = first; i < last; ++i)
        if (f.pieces[i].empty()) { ++missing; missingIdx = i; }
    if (missing != 1) return false;

    // XOR ngược: parity ^ (mọi mảnh đã có) = mảnh thiếu, kèm 2 byte độ dài đứng đầu.
    std::vector<uint8_t> rec(par);
    for (size_t i = first; i < last; ++i) {
        if (i == missingIdx) continue;
        const auto& p = f.pieces[i];
        rec[0] ^= uint8_t(p.size() >> 8);
        rec[1] ^= uint8_t(p.size() & 0xFF);
        for (size_t b = 0; b < p.size(); ++b) rec[kFecLenPrefix + b] ^= p[b];
    }

    const size_t len = (size_t(rec[0]) << 8) | rec[1];
    // Độ dài dựng ra phải hợp lệ: parity hỏng/không cùng frame sẽ cho số vô nghĩa,
    // nhét bừa vào NAL còn tệ hơn bỏ frame.
    if (len == 0 || len > kMaxVideoPayload || kFecLenPrefix + len > rec.size()) return false;

    f.pieces[missingIdx].assign(rec.begin() + kFecLenPrefix,
                                rec.begin() + kFecLenPrefix + len);
    f.bytes += len;
    ++f.received;
    ++stats_.packetsRecovered;
    return true;
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

        // Đếm chùm mất liên tiếp (xem Stats::lossRuns). Chỉ chạy trên frame BỎ ĐI nên
        // không nằm trên đường nóng.
        const Pending& f = it->second;
        size_t run = 0;
        for (size_t i = 0; i <= f.pktCount; ++i) {
            const bool gone = i < f.pktCount && f.pieces[i].empty();
            if (gone) {
                ++run;
            } else if (run) {
                size_t b = 0;
                if (run <= 3)       b = run - 1;   // 1, 2, 3 tách riêng: chùm ngắn là
                else if (run < 8)   b = 3;         // thứ FEC hiện tại còn có cửa cứu
                else if (run < 16)  b = 4;
                else if (run < 32)  b = 5;
                else                b = 6;
                ++stats_.lossRuns[b];
                if (run > stats_.lossRunMax) stats_.lossRunMax = run;
                run = 0;
            }
        }

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
