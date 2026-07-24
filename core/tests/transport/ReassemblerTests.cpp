// =============================================================================
// ReassemblerTests.cpp — ghép mảnh dưới điều kiện UDP thật: đảo thứ tự, trùng, mất
// gói, join giữa chừng, timeout — và lập kế hoạch NACK (GĐ7).
// =============================================================================
#include "Tests.h"
#include "support/TestSupport.h"

#include "deskhub/transport/Packetizer.h"
#include "deskhub/transport/Reassembler.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace deskhub;

namespace {

void TestInOrder() {
    std::printf("[reasm] in-order delivery...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(60, 30);
    uint64_t now = 1'000'000;
    size_t popped = 0;
    for (const auto& f : frames) {
        for (const auto& d : Packetize(pk, f, now)) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) {
            Check(popped < frames.size() && SameFrame(*out, frames[popped]),
                "output frame == input frame (in-order)");
            ++popped;
        }
        now += 16'667;
    }
    Check(popped == frames.size(), "got all 60 frames (in-order)");
    Check(ra.stats().framesDropped == 0 && !ra.TakeLossEvent(), "no loss (in-order)");
}

void TestReorder() {
    std::printf("[reasm] shuffled order within a 2-frame window...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(40, 20);
    uint64_t now = 1'000'000;
    size_t popped = 0;
    for (size_t i = 0; i < frames.size(); i += 2) {
        std::vector<Datagram> batch = Packetize(pk, frames[i], now);
        if (i + 1 < frames.size()) {
            auto more = Packetize(pk, frames[i + 1], now + 16'667);
            batch.insert(batch.end(), std::make_move_iterator(more.begin()),
                std::make_move_iterator(more.end()));
        }
        for (size_t k = batch.size(); k > 1; --k)
            std::swap(batch[k - 1], batch[Rnd() % k]);
        for (const auto& d : batch) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) {
            Check(SameFrame(*out, frames[popped]), "output frame in correct order (reorder)");
            ++popped;
        }
        now += 2 * 16'667;
    }
    Check(popped == frames.size(), "got all 40 frames (reorder)");
    Check(ra.stats().framesDropped == 0 && !ra.TakeLossEvent(), "no loss (reorder)");
}

void TestDropPacket() {
    std::printf("[reasm] drop 1 packet -> drop frame, loss event, swallow until IDR...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    std::vector<TestFrame> frames;
    for (uint32_t i = 0; i < 20; ++i) {
        TestFrame f{i, (i % 10) == 0, {}};
        f.nal.resize(5 * kMaxVideoPayload - 100);
        for (auto& b : f.nal) b = uint8_t(Rnd());
        frames.push_back(std::move(f));
    }
    uint64_t now = 1'000'000;
    std::vector<uint32_t> got;
    bool sawLoss = false;
    for (const auto& f : frames) {
        auto pkts = Packetize(pk, f, now);
        if (f.id == 5) pkts.erase(pkts.begin() + 2); // mất mảnh giữa của frame 5
        for (const auto& d : pkts) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) got.push_back(out->frameId);
        sawLoss = sawLoss || ra.TakeLossEvent();
        now += 16'667;
    }
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 5; ++i) want.push_back(i);
    for (uint32_t i = 10; i < 20; ++i) want.push_back(i);
    Check(got == want, "frame sequence after packet loss matches policy");
    Check(sawLoss, "loss event occurred after dropping frame");
    Check(ra.stats().framesDropped == 1 && ra.stats().packetsLost == 1, "drop/lost stats");
    Check(ra.stats().framesSkipped == 4, "4 non-IDR frames swallowed");
}

void TestDuplicates() {
    std::printf("[reasm] duplicate packets...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(20, 10);
    uint64_t now = 1'000'000;
    size_t popped = 0;
    for (const auto& f : frames) {
        const auto pkts = Packetize(pk, f, now);
        for (const auto& d : pkts) Feed(ra, d, now);
        for (const auto& d : pkts) Feed(ra, d, now); // phát lại toàn bộ
        while (auto out = ra.PopReady(now)) {
            Check(SameFrame(*out, frames[popped]), "output frame correct despite duplicate packets");
            ++popped;
        }
        now += 16'667;
    }
    Check(popped == frames.size(), "got all frames (duplicate)");
    Check(ra.stats().framesDropped == 0, "no drop (duplicate)");
}

void TestJoinMidStream() {
    std::printf("[reasm] join mid-stream -> wait for first IDR...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(16, 10); // IDR tại 0 và 10; ta bỏ qua frame 0
    uint64_t now = 1'000'000;
    std::vector<uint32_t> got;
    for (size_t i = 1; i < frames.size(); ++i) {
        Check(ra.WaitingForIdr() == (got.empty()), "WaitingForIdr before first IDR");
        for (const auto& d : Packetize(pk, frames[i], now)) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) got.push_back(out->frameId);
        now += 16'667;
    }
    std::vector<uint32_t> want{10, 11, 12, 13, 14, 15};
    Check(got == want, "only emits from IDR (join mid-stream)");
    Check(ra.stats().framesSkipped == 9, "9 frames before IDR swallowed");
    Check(!ra.TakeLossEvent(), "swallowing while waiting for IDR is not loss");
}

void TestHeadTimeout() {
    std::printf("[reasm] frame missing a piece past 2 frame intervals -> drop on timeout...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    std::vector<TestFrame> frames;
    for (uint32_t i = 0; i < 4; ++i) {
        TestFrame f{i, i == 0 || i == 3, {}}; // IDR ở 0 và 3
        f.nal.resize(3 * kMaxVideoPayload);
        for (auto& b : f.nal) b = uint8_t(Rnd());
        frames.push_back(std::move(f));
    }
    uint64_t now = 1'000'000;
    for (const auto& d : Packetize(pk, frames[0], now)) Feed(ra, d, now);
    auto out = ra.PopReady(now);
    Check(out && out->frameId == 0, "frame 0 emitted normally");

    auto pkts1 = Packetize(pk, frames[1], now);
    pkts1.pop_back(); // frame 1 thiếu mảnh cuối
    for (const auto& d : pkts1) Feed(ra, d, now);
    for (const auto& d : Packetize(pk, frames[2], now)) Feed(ra, d, now);
    Check(!ra.PopReady(now).has_value(), "not dropped yet while still within deadline");

    now += 40'000; // > 2 * 16667
    Check(!ra.PopReady(now).has_value(), "frame 2 (non-IDR after loss) not emitted");
    Check(ra.TakeLossEvent(), "loss event after timeout");

    for (const auto& d : Packetize(pk, frames[3], now)) Feed(ra, d, now);
    out = ra.PopReady(now);
    Check(out && out->frameId == 3, "recovered via IDR after loss");
}

// GĐ7: lập kế hoạch NACK — chờ gói đảo thứ tự, liệt kê mảnh thiếu, điều tiết theo
// RTT, bỏ qua frame quá hạn và frame đã đủ, kẹp về sức chứa out.
void TestNackPlanning() {
    std::printf("[reasm] NACK planning: hold, rate-limit, deadline, clamp...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);

    auto f = MakeIdrFrame(0, 3);
    auto pkts = Packetize(pk, f, 1'000'000); // 3 gói dữ liệu (FEC tắt)
    const uint64_t t0 = 1'000'000;
    for (size_t i = 0; i < pkts.size(); ++i)
        if (i != 1) Feed(ra, pkts[i], t0); // thiếu mảnh 1

    uint16_t out[8];
    uint32_t fid = 0xFFFF;
    Check(ra.PlanNack(t0, 5000, fid, out) == 0, "no NACK before hold time");
    size_t n = ra.PlanNack(t0 + 2000, 5000, fid, out);
    Check(n == 1 && out[0] == 1 && fid == 0, "NACK lists the missing packet after hold");
    Check(ra.PlanNack(t0 + 2000, 5000, fid, out) == 0, "no re-NACK within the interval");
    Check(ra.PlanNack(t0 + 12000, 5000, fid, out) == 1, "re-NACK allowed after the interval");

    Feed(ra, pkts[1], t0 + 12000);
    auto got = ra.PopReady(t0 + 12000);
    Check(got && got->frameId == 0, "frame completes once the packet arrives");
    Check(ra.PlanNack(t0 + 20000, 5000, fid, out) == 0, "nothing to NACK once delivered");

    // Kẹp về sức chứa out.
    {
        Reassembler r2(16'667);
        auto g = MakeIdrFrame(0, 5);
        auto gp = Packetize(pk, g, 2'000'000);
        Feed(r2, gp[0], 2'000'000);
        Feed(r2, gp[4], 2'000'000); // thiếu 1,2,3
        uint16_t small[2];
        uint32_t id = 0;
        Check(r2.PlanNack(2'002'000, 0, id, std::span<uint16_t>(small, 2)) == 2,
            "NACK clamps to out span size");
    }
    // Frame quá hạn ghép: không NACK (gửi lại cũng không kịp).
    {
        Reassembler r3(16'667);
        auto g = MakeIdrFrame(0, 3);
        auto gp = Packetize(pk, g, 3'000'000);
        Feed(r3, gp[0], 3'000'000); // thiếu 1,2
        uint16_t o[8];
        uint32_t id = 0;
        Check(r3.PlanNack(3'040'000, 0, id, o) == 0, "no NACK for a frame past the reassembly deadline");
    }
}

// Frame non-IDR đúng `pkts` mảnh (để dựng các ca drop mà không vướng chờ-IDR sau IDR mở màn).
TestFrame MakePFrame(uint32_t id, size_t pkts) {
    TestFrame f{id, false, {}};
    f.nal.resize(pkts * kMaxVideoPayload - 100);
    for (auto& b : f.nal) b = uint8_t(Rnd());
    return f;
}

// Đầu hàng thiếu mảnh mà đã có ≥2 frame mới hơn hoàn chỉnh -> bỏ với lý do Overtaken.
void TestOvertakenDrop() {
    std::printf("[reasm] head incomplete + 2 newer complete -> Overtaken...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    std::vector<Reassembler::DropReason> reasons;
    ra.onFrameDrop = [&](const Reassembler::FrameDropInfo& d) { reasons.push_back(d.reason); };

    const uint64_t now = 1'000'000;
    for (const auto& d : Packetize(pk, MakeIdrFrame(0, 2), now)) Feed(ra, d, now);
    Check(ra.PopReady(now).has_value(), "IDR frame 0 delivered");

    auto p1 = Packetize(pk, MakePFrame(1, 2), now);
    p1.pop_back(); // frame 1 thiếu mảnh cuối
    for (const auto& d : p1) Feed(ra, d, now);
    for (uint32_t id = 2; id <= 3; ++id)
        for (const auto& d : Packetize(pk, MakePFrame(id, 2), now)) Feed(ra, d, now);

    while (ra.PopReady(now)) {}
    Check(std::count(reasons.begin(), reasons.end(), Reassembler::DropReason::Overtaken) >= 1,
        "incomplete head dropped as Overtaken");
}

// Hàng chờ đầy (kMaxPendingFrames) mà tới frame mới -> frame già nhất bị Evicted.
void TestEvictedDrop() {
    std::printf("[reasm] pending queue full -> oldest Evicted...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    std::vector<Reassembler::DropReason> reasons;
    ra.onFrameDrop = [&](const Reassembler::FrameDropInfo& d) { reasons.push_back(d.reason); };

    const uint64_t now = 1'000'000;
    for (const auto& d : Packetize(pk, MakeIdrFrame(0, 2), now)) Feed(ra, d, now);
    ra.PopReady(now);
    // 5 frame dở dang (mỗi frame chỉ 1/2 mảnh) -> vượt sức chứa 4 -> frame 1 bị đẩy ra.
    for (uint32_t id = 1; id <= 5; ++id) {
        auto p = Packetize(pk, MakePFrame(id, 2), now);
        p.pop_back();
        for (const auto& d : p) Feed(ra, d, now);
    }
    Check(std::count(reasons.begin(), reasons.end(), Reassembler::DropReason::Evicted) >= 1,
        "oldest pending frame evicted when the queue overflows");
}

// Gói của một frame đã khai tử vì "mất" mà giờ mới về -> tính là TỚI MUỘN, không phải mất.
void TestLatePacketAccounting() {
    std::printf("[reasm] packet arriving after its frame was dropped -> counted late...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);

    uint64_t now = 1'000'000;
    for (const auto& d : Packetize(pk, MakeIdrFrame(0, 2), now)) Feed(ra, d, now);
    ra.PopReady(now);

    auto p1 = Packetize(pk, MakePFrame(1, 2), now);
    Datagram late = p1.back(); // giữ lại mảnh cuối để "về muộn"
    p1.pop_back();
    for (const auto& d : p1) Feed(ra, d, now);

    now += 40'000; // quá 2 khoảng frame -> frame 1 bị bỏ vì timeout
    ra.PopReady(now);
    Check(ra.stats().framesDropped == 1, "frame 1 dropped on timeout");

    Feed(ra, late, now + 5'000); // mảnh cuối lết về sau khi frame đã chết
    Check(ra.stats().latePackets == 1, "the straggler is counted as late, not lost");
}

// TakeMaxGapMs báo khoảng lặng dài nhất giữa hai gói video liên tiếp rồi tự xoá.
void TestMaxGap() {
    std::printf("[reasm] TakeMaxGapMs reports the longest inter-packet gap...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    auto pkts = Packetize(pk, MakeIdrFrame(0, 2), 1'000'000);
    Feed(ra, pkts[0], 1'000'000);
    Feed(ra, pkts[1], 1'150'000); // cách 150ms
    Check(ra.TakeMaxGapMs() == 150, "gap measured in ms");
    Check(ra.TakeMaxGapMs() == 0, "read-and-clear: second read is 0");
}

// Gói mang cùng frameId nhưng pktCount lệch (gói hỏng/ác ý): Slot từ chối nó,
// frame đang ghép không bị ảnh hưởng và vẫn hoàn chỉnh bằng các mảnh thật.
void TestPktCountMismatch() {
    std::printf("[reasm] packet with a mismatched pktCount is ignored...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    auto f = MakeIdrFrame(0, 3);
    auto pkts = Packetize(pk, f, 1'000'000);
    Feed(ra, pkts[0], 1'000'000);

    // Giả mạo: lấy mảnh 1, sửa pktCount (2 byte cuối video header) thành 5.
    Datagram forged = pkts[1];
    forged[kCommonHeaderSize + 14] = 0;
    forged[kCommonHeaderSize + 15] = 5;
    Feed(ra, forged, 1'000'000);

    Feed(ra, pkts[1], 1'000'000);
    Feed(ra, pkts[2], 1'000'000);
    auto out = ra.PopReady(1'000'000);
    Check(out.has_value() && SameFrame(*out, f),
        "mismatched-pktCount packet ignored, frame still completes");
}

// Chùm mất dài: lossRuns rơi đúng bucket, lossRunMax ghi kỷ lục, và bản khám
// nghiệm FrameDropInfo chỉ đúng vị trí các chùm thiếu (thủng đuôi = dấu hiệu burst).
void TestLossRunBucketsAndDropInfo() {
    std::printf("[reasm] burst loss -> lossRuns buckets + FrameDropInfo autopsy...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    Reassembler::FrameDropInfo info{};
    int drops = 0;
    ra.onFrameDrop = [&](const Reassembler::FrameDropInfo& d) {
        info = d;
        ++drops;
    };

    uint64_t now = 1'000'000;
    for (const auto& d : Packetize(pk, MakeIdrFrame(0, 2), now)) Feed(ra, d, now);
    ra.PopReady(now);

    // Frame 40 mảnh, mất chùm 5 (mảnh 3..7), chùm 10 (mảnh 20..29) và mảnh cuối
    // (39) — mảnh cuối để kiểm lastMissing chỉ đúng lỗ thủng ở đuôi.
    const TestFrame f = MakePFrame(1, 40);
    const auto pkts = Packetize(pk, f, now);
    for (size_t i = 0; i < pkts.size(); ++i) {
        const bool dropIt = (i >= 3 && i <= 7) || (i >= 20 && i <= 29) || i == 39;
        if (!dropIt) Feed(ra, pkts[i], now);
    }
    now += 40'000; // quá 2 khoảng frame -> khai tử vì timeout
    Check(!ra.PopReady(now).has_value(), "incomplete frame dropped, nothing emitted");
    Check(drops == 1 && info.reason == Reassembler::DropReason::Timeout, "dropped once on timeout");
    Check(info.missing == 16 && info.firstMissing == 3 && info.lastMissing == 39,
        "autopsy pinpoints the missing spans (tail hole included)");
    Check(info.total == 40 && info.bytesGot > 0, "autopsy carries totals");

    const auto& st = ra.stats();
    Check(st.lossRuns[3] == 1, "run of 5 lands in the 4..7 bucket");
    Check(st.lossRuns[4] == 1, "run of 10 lands in the 8..15 bucket");
    Check(st.lossRuns[0] == 1, "run of 1 (the tail packet) lands in the first bucket");
    Check(st.lossRunMax == 10, "longest run recorded");
}

// Biên của Packetizer: frame rỗng hoặc thiếu send callback -> không phát gì.
void TestPacketizerEdges() {
    std::printf("[reasm] Packetizer edge inputs -> 0 packets...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    size_t sent = 0;
    auto count = [&](std::span<const uint8_t>) { ++sent; };
    Check(pk.SendFrame({}, 1, 1, false, count) == 0 && sent == 0, "empty frame -> 0 packets");
    const uint8_t one = 0xAB;
    Check(pk.SendFrame(std::span<const uint8_t>(&one, 1), 1, 1, false, nullptr) == 0,
        "missing send callback -> 0");
}

} // namespace

void RunReassemblerTests() {
    TestInOrder();
    TestReorder();
    TestDropPacket();
    TestDuplicates();
    TestJoinMidStream();
    TestHeadTimeout();
    TestNackPlanning();
    TestOvertakenDrop();
    TestEvictedDrop();
    TestLatePacketAccounting();
    TestMaxGap();
    TestPktCountMismatch();
    TestLossRunBucketsAndDropInfo();
    TestPacketizerEdges();
}
