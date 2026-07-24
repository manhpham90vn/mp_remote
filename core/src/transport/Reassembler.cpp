// =============================================================================
// Reassembler.cpp — cài đặt việc ghép mảnh, khôi phục FEC và chính sách bỏ frame.
//
// BỐ CỤC (theo dòng chảy của một gói qua lớp này)
//   Slot()       — tìm/tạo chỗ ghép cho một frameId, và chặn gói quá muộn.
//   Push()       — nhận mảnh dữ liệu.
//   PushFec()    — nhận gói parity.
//   TryRecover() — thử dựng lại mảnh thiếu bằng XOR ngược.
//   PopReady()   — quyết định: trả frame ra, hay bỏ nó, hay chờ thêm.
//   Drop()       — bỏ một frame, cập nhật thống kê và mốc chặn.
//
// KHÁI NIỆM "BARRIER" (barrierId_)
//   Mốc frameId mà mọi frame ≤ nó đã được phát ra hoặc đã bị bỏ. Gói đến sau mốc
//   này là gói lạc quá muộn — nhận vào chỉ tổ dựng lại một frame đã lỡ, và nếu
//   phát ra sẽ đưa decoder đi lùi thời gian. Nên chúng bị bỏ ngay tại Slot().
//
// CẤU TRÚC DỮ LIỆU
//   pending_ là std::map (cây đỏ-đen) chứ không phải unordered_map, vì lớp này cần
//   duyệt theo THỨ TỰ frameId liên tục: pending_.begin() luôn là frame cũ nhất,
//   tức là frame kế tiếp phải phát. Với tối đa 4 phần tử thì chi phí cây không đáng kể.
//
// VỀ THỜI GIAN
//   Không gọi đồng hồ hệ thống ở bất cứ đâu; `nowUs` do người gọi bơm vào. Đó là
//   lý do CoreTests kiểm chứng được cả đường timeout mà không phải sleep thật.
//
// LIÊN QUAN: deskhub/transport/Reassembler.h (thiết kế + chính sách), Packetizer.cpp
// =============================================================================
#include "deskhub/transport/Reassembler.h"

#include <iterator>

namespace deskhub {

// Cổng vào chung của Push và PushFec: mọi gói đều phải qua đây để lấy chỗ ghép.
// Trả nullptr nghĩa là "bỏ gói này" — quá muộn, hoặc không khớp frame đang ghép.
Reassembler::Pending* Reassembler::Slot(uint32_t id, uint16_t pktCount,
    uint64_t timestampUs, uint64_t nowUs) {
    if (haveBarrier_ && id <= barrierId_) return nullptr; // gói muộn của frame đã phát/bỏ

    auto it = pending_.find(id);
    if (it == pending_.end()) {
        // Frame mới trong khi đã đầy chỗ: frame già nhất đang chặn hàng — bỏ nó.
        while (pending_.size() >= kMaxPendingFrames)
            Drop(pending_.begin(), DropReason::Evicted, nowUs);
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

// Nhận một mảnh dữ liệu. Đếm packetsReceived TRƯỚC mọi kiểm tra, kể cả với gói
// trùng hay quá muộn: đây là mẫu số của tỉ lệ mất gói, bỏ sót gói nào ở đây sẽ làm
// tỉ lệ báo cáo cho host lệch đi.
void Reassembler::Push(const VideoPacketView& pkt, uint64_t nowUs) {
    ++stats_.packetsReceived;

    // Khoảng lặng giữa hai gói video liên tiếp — cảm biến "gói về đều hay về cục".
    // Đo trên MỌI gói dữ liệu, kể cả trùng/muộn: cái ta quan tâm là nhịp đường
    // truyền, không phải tính hợp lệ của gói.
    if (lastPushUs_) {
        const uint32_t gapMs = uint32_t((nowUs - lastPushUs_) / 1000);
        if (gapMs > maxGapMs_) maxGapMs_ = gapMs;
    }
    lastPushUs_ = nowUs;

    if (pkt.payload.empty()) return; // Packetizer không bao giờ phát mảnh rỗng

    // Gói của frame đã khai tử vì "mất gói" mà giờ mới về = KHÔNG mất, chỉ MUỘN.
    // Ghi nhận trước khi Slot() chặn nó — số liệu này phân xử §7b của docs/06.
    if (haveBarrier_ && pkt.hdr.frameId <= barrierId_)
        NoteLatePacket(pkt.hdr.frameId, nowUs);

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

    // Gói này có thể là mảnh cuối một nhóm cần để parity (đã tới trước) dùng được.
    // Nhóm xen kẽ: gói pktIndex thuộc nhóm (pktIndex % numGroups).
    const size_t numGroups = (size_t(f.pktCount) + kFecGroupSize - 1) / kFecGroupSize;
    if (numGroups) TryRecover(f, uint8_t(pkt.hdr.pktIndex % numGroups));
}

// Nhận một gói parity. Đếm riêng vào fecReceived chứ KHÔNG cộng vào
// packetsReceived — trộn parity vào mẫu số sẽ làm tỉ lệ mất gói tụt xuống đúng vào
// lúc FEC đang bật, tức là đúng lúc đường truyền đang có vấn đề cần báo cáo.
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

// Thử dựng lại mảnh thiếu của một nhóm FEC.
//
// Nguyên lý: parity = m₁ ⊕ m₂ ⊕ … ⊕ mₙ. XOR có tính chất tự nghịch đảo, nên nếu
// thiếu mảnh mₖ thì mₖ = parity ⊕ (mọi mảnh còn lại). Đúng một ẩn số thì giải được;
// hai ẩn trở lên thì một phương trình là không đủ.
//
// Gọi từ CẢ HAI đường (Push và PushFec) vì không biết trước cái nào tới sau: mảnh
// dữ liệu cuối cùng có thể tới sau parity, hoặc ngược lại. Gọi thừa là vô hại —
// hàm tự thoát ngay khi điều kiện chưa đủ.
bool Reassembler::TryRecover(Pending& f, uint8_t group) {
    auto pit = f.parity.find(group);
    if (pit == f.parity.end()) return false;
    const std::vector<uint8_t>& par = pit->second;

    // Nhóm xen kẽ `group` phủ các gói group, group+numGroups, group+2*numGroups, …
    const size_t numGroups = (size_t(f.pktCount) + kFecGroupSize - 1) / kFecGroupSize;
    if (numGroups == 0 || group >= numGroups) return false;

    // Parity XOR chỉ gỡ được MỘT ẩn số. Không thiếu gói nào thì thôi, thiếu ≥2 thì chịu.
    size_t missing = 0, missingIdx = 0;
    for (size_t i = group; i < f.pktCount; i += numGroups)
        if (f.pieces[i].empty()) {
            ++missing;
            missingIdx = i;
        }
    if (missing != 1) return false;

    // XOR ngược: parity ^ (mọi mảnh đã có) = mảnh thiếu, kèm 2 byte độ dài đứng đầu.
    std::vector<uint8_t> rec(par);
    for (size_t i = group; i < f.pktCount; i += numGroups) {
        if (i == missingIdx) continue;
        const auto& p = f.pieces[i];
        // Phòng thủ tầng hai (Wire đã chặn kích thước từng gói): mảnh dài hơn phần
        // dữ liệu của parity nghĩa là hai gói không cùng một nhóm hợp lệ — XOR tiếp
        // sẽ ghi ra ngoài `rec`. Bỏ khôi phục, coi như nhóm này không cứu được.
        if (kFecLenPrefix + p.size() > rec.size()) return false;
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

// Trái tim của chính sách. Mỗi vòng lặp chỉ xét frame CŨ NHẤT (đầu hàng) và quyết
// một trong ba điều:
//   - đủ mảnh → ghép lại, trả ra, dời barrier;
//   - hết hy vọng (quá hạn, hoặc đã bị hai frame mới hơn vượt mặt) → bỏ, xét tiếp;
//   - còn hy vọng → dừng, trả nullopt.
// Không bao giờ nhảy qua đầu hàng để trả frame sau, kể cả khi frame sau đã đủ:
// decoder H.264 cần đúng thứ tự.
std::optional<Reassembler::Frame> Reassembler::PopReady(uint64_t nowUs) {
    while (!pending_.empty()) {
        auto head = pending_.begin();
        Pending& f = head->second;

        if (f.Complete()) {
            if (waitingForIdr_ && !f.idr) { // nuốt non-IDR khi chờ IDR
                Drop(head, DropReason::PreIdr, nowUs);
                continue;
            }
            // Nối các mảnh theo đúng thứ tự pktIndex thành NAL Annex-B liền mạch —
            // đúng chuỗi byte mà encoder đã đẻ ra ở đầu kia. reserve trước theo
            // f.bytes để chỉ cấp phát một lần.
            Frame out;
            out.frameId = head->first;
            out.timestampUs = f.timestampUs;
            out.idr = f.idr;
            out.firstSeenUs = f.firstSeenUs;
            out.nal.reserve(f.bytes);
            for (const auto& p : f.pieces)
                out.nal.insert(out.nal.end(), p.begin(), p.end());
            haveBarrier_ = true;
            barrierId_ = head->first;
            waitingForIdr_ = false;
            ++stats_.framesCompleted;
            pending_.erase(head);
            return out;
        }

        // Đầu hàng thiếu mảnh: bỏ nếu quá hạn hoặc bị ≥2 frame hoàn chỉnh mới hơn vượt mặt.
        size_t newerComplete = 0;
        for (auto n = std::next(head); n != pending_.end(); ++n)
            if (n->second.Complete()) ++newerComplete;
        if (nowUs - f.firstSeenUs > 2 * frameIntervalUs_) {
            Drop(head, DropReason::Timeout, nowUs);
            continue;
        }
        if (newerComplete >= 2) {
            Drop(head, DropReason::Overtaken, nowUs);
            continue;
        }
        return std::nullopt; // còn hy vọng mảnh thiếu đang trên đường tới
    }
    return std::nullopt;
}

// Lập yêu cầu NACK cho frame CŨ NHẤT còn dở. Xem lý do các mốc thời gian ở đầu file
// và ở Reassembler.h. Không đổi chính sách bỏ frame: NACK là best-effort chồng lên
// trên, gói gửi lại về kịp hạn ghép thì frame cứu được, không kịp thì Drop như cũ.
size_t Reassembler::PlanNack(uint64_t nowUs, uint64_t rttUs, uint32_t& frameId,
    std::span<uint16_t> out) {
    if (out.empty()) return 0;
    for (auto& [id, f] : pending_) { // map: begin() là frame cũ nhất
        if (f.Complete()) continue;
        // Frame sắp bị bỏ vì quá hạn ghép: gửi lại cũng không về kịp → thôi.
        if (nowUs - f.firstSeenUs >= 2 * frameIntervalUs_) continue;
        // Cho gói đảo thứ tự một nhịp trước khi kết luận là mất.
        if (nowUs - f.firstSeenUs < kNackHoldUs) return 0;
        // Đừng xin lại quá dày: gói gửi lại cần ~1 RTT mới về.
        const uint64_t interval = rttUs > kNackMinIntervalUs ? rttUs : kNackMinIntervalUs;
        if (f.lastNackUs && nowUs - f.lastNackUs < interval) return 0;

        size_t n = 0;
        for (uint16_t i = 0; i < f.pktCount && n < out.size(); ++i)
            if (f.pieces[i].empty()) out[n++] = i;
        if (n == 0) return 0; // chưa Complete mà không thiếu gì: không xảy ra, thủ sẵn
        f.lastNackUs = nowUs;
        frameId = id;
        return n;
    }
    return 0;
}

bool Reassembler::TakeLossEvent() {
    const bool e = lossEvent_;
    lossEvent_ = false;
    return e;
}

// Bỏ một frame khỏi hàng chờ. `reason` phân biệt hai tình huống rất khác nhau:
//   Timeout/Overtaken/Evicted — frame thiếu mảnh vì MẤT GÓI (hoặc gói chưa kịp
//           tới). Tính vào thống kê mất mát, bật cờ xin keyframe, chuyển sang
//           trạng thái chờ IDR, và chôn vào nghĩa địa để nhận diện gói về muộn.
//   PreIdr — frame LÀNH LẶN nhưng bị nuốt vì đang chờ IDR. Không phải lỗi đường
//           truyền, chỉ tính vào framesSkipped.
// Dù theo đường nào, barrier vẫn được dời tới để gói lạc của frame này bị chặn,
// và onFrameDrop (nếu client gắn) nhận được bản khám nghiệm.
void Reassembler::Drop(PendingMap::iterator it, DropReason reason, uint64_t nowUs) {
    const Pending& f = it->second;
    const bool loss = reason != DropReason::PreIdr;

    FrameDropInfo info;
    info.frameId = it->first;
    info.reason = reason;
    info.total = f.pktCount;
    info.idr = f.idr;
    info.waitedMs = uint32_t((nowUs - f.firstSeenUs) / 1000);
    info.bytesGot = uint32_t(f.bytes);

    if (loss) {
        ++stats_.framesDropped;
        stats_.packetsLost += uint64_t(it->second.pktCount - it->second.received);

        // Đếm chùm mất liên tiếp (xem Stats::lossRuns) và vị trí chùm thiếu cho bản
        // khám nghiệm. Chỉ chạy trên frame BỎ ĐI nên không nằm trên đường nóng.
        size_t run = 0;
        bool anyMissing = false;
        for (size_t i = 0; i <= f.pktCount; ++i) {
            const bool gone = i < f.pktCount && f.pieces[i].empty();
            if (gone) {
                ++info.missing;
                if (!anyMissing) {
                    anyMissing = true;
                    info.firstMissing = uint16_t(i);
                }
                info.lastMissing = uint16_t(i);
                ++run;
            } else if (run) {
                size_t b = 0;
                if (run <= 3)
                    b = run - 1; // 1, 2, 3 tách riêng: chùm ngắn là
                else if (run < 8)
                    b = 3; // thứ FEC hiện tại còn có cửa cứu
                else if (run < 16)
                    b = 4;
                else if (run < 32)
                    b = 5;
                else
                    b = 6;
                ++stats_.lossRuns[b];
                if (run > stats_.lossRunMax) stats_.lossRunMax = run;
                run = 0;
            }
        }

        // Chôn vào nghĩa địa (ghi đè entry cũ nhất): gói của frame này còn lết về
        // sau sẽ được NoteLatePacket nhận diện là "muộn" thay vì "mất".
        graveyard_[graveNext_] = Grave{it->first, nowUs};
        graveNext_ = (graveNext_ + 1) % kGraveyardSize;

        lossEvent_ = true;
        ++stats_.lossEvents;
        waitingForIdr_ = true; // frame sau tham chiếu frame vừa mất → phải chờ IDR
    } else {
        ++stats_.framesSkipped;
    }
    if (!haveBarrier_ || it->first > barrierId_) {
        haveBarrier_ = true;
        barrierId_ = it->first;
    }
    pending_.erase(it);

    if (onFrameDrop) onFrameDrop(info);
}

// Gói dữ liệu mang frameId ≤ barrier — nếu frame đó nằm trong nghĩa địa (bị khai
// tử vì "mất gói") thì đây là bằng chứng nó KHÔNG mất mà chỉ MUỘN. Quét tuyến tính
// 16 entry, chỉ chạy trên gói muộn (hiếm) nên không đáng kể.
void Reassembler::NoteLatePacket(uint32_t id, uint64_t nowUs) {
    for (const Grave& g : graveyard_) {
        if (g.dropUs == 0 || g.frameId != id) continue;
        ++stats_.latePackets;
        const uint64_t lateMs = (nowUs - g.dropUs) / 1000;
        stats_.lateMsSum += lateMs;
        if (lateMs > stats_.lateMsMax) stats_.lateMsMax = lateMs;
        return;
    }
}

} // namespace deskhub
