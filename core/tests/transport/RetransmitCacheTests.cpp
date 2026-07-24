// =============================================================================
// RetransmitCacheTests.cpp — kho gửi lại phía host (GĐ7) và đường NACK trọn vẹn:
// client mất mảnh → xin lại → host tra kho → gửi lại → frame cứu được không cần IDR.
// =============================================================================
#include "Tests.h"
#include "support/TestSupport.h"

#include "deskhub/transport/Packetizer.h"
#include "deskhub/transport/Reassembler.h"
#include "deskhub/transport/RetransmitCache.h"

#include <algorithm>
#include <cstdio>

using namespace deskhub;

namespace {

void TestRetransmitCache() {
    std::printf("[retx] store/find video datagrams, ignore FEC/control, ring eviction...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    RetransmitCache cache;

    // Kho một frame 3 mảnh (FEC tắt -> đúng 3 gói video).
    auto f = MakeIdrFrame(100, 3);
    auto pkts = Packetize(pk, f, 1'000'000);
    Check(pkts.size() == 3, "3 data packets to store");
    for (auto& d : pkts) cache.Store(d);
    for (uint16_t i = 0; i < 3; ++i) {
        auto d = cache.Find(100, i);
        Check(!d.empty() && d.size() == pkts[i].size() &&
                  std::equal(d.begin(), d.end(), pkts[i].begin()),
            "cached packet found and byte-identical");
    }
    Check(cache.Find(100, 3).empty(), "out-of-range pktIndex not found");
    Check(cache.Find(999, 0).empty(), "unknown frame not found");

    // Gói FEC bị bỏ qua (không có pktIndex để tra); gói control cũng vậy.
    pk.SetFecEnabled(true);
    auto f2 = MakeIdrFrame(101, 3);
    for (auto& d : Packetize(pk, f2, 1'000'000)) cache.Store(d);
    for (uint16_t i = 0; i < 3; ++i)
        Check(!cache.Find(101, i).empty(), "data packet of a FEC frame still stored");
    uint8_t cbuf[kMaxDatagram];
    size_t cn = BuildPing(cbuf, 42, PingPong{1, 1});
    cache.Store(std::span<const uint8_t>(cbuf, cn)); // control: bỏ qua, không sập

    // Ring: nạp thêm kCacheFrames frame mới -> 100 và 101 bị đẩy ra.
    pk.SetFecEnabled(false);
    for (uint32_t id = 200; id < 200 + RetransmitCache::kCacheFrames; ++id)
        for (auto& d : Packetize(pk, MakeIdrFrame(id, 1), 1'000'000)) cache.Store(d);
    Check(cache.Find(100, 0).empty(), "oldest frame evicted from the ring");
    Check(!cache.Find(200 + RetransmitCache::kCacheFrames - 1, 0).empty(),
        "newest frame retained");

    cache.Reset();
    Check(cache.Find(200, 0).empty(), "reset clears the cache");
}

// Đường trọn vẹn: gói video được kho lại; client mất một mảnh; PlanNack chỉ ra nó;
// host tra kho và "gửi lại"; frame hoàn chỉnh mà không phải xin IDR.
void TestNackEndToEnd() {
    std::printf("[retx] end-to-end NACK: lost packet retransmitted, frame recovered...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    RetransmitCache cache;

    auto f = MakeIdrFrame(0, 4);
    auto pkts = Packetize(pk, f, 1'000'000); // 4 gói dữ liệu
    for (auto& d : pkts) cache.Store(d);     // host lưu mọi gói vừa phát

    const uint64_t t0 = 1'000'000;
    for (size_t i = 0; i < pkts.size(); ++i)
        if (i != 2) Feed(ra, pkts[i], t0); // client mất mảnh 2
    Check(!ra.PopReady(t0).has_value(), "frame incomplete without packet 2");

    uint16_t out[8];
    uint32_t fid = 0;
    size_t n = ra.PlanNack(t0 + 3000, 5000, fid, out);
    Check(n == 1 && out[0] == 2 && fid == 0, "client NACKs the lost packet");

    auto rd = cache.Find(fid, out[0]);
    Check(!rd.empty(), "host finds the packet to retransmit");
    Feed(ra, Datagram(rd.begin(), rd.end()), t0 + 6000); // gói gửi lại tới nơi

    auto got = ra.PopReady(t0 + 6000);
    Check(got && got->frameId == 0 && SameFrame(*got, f), "frame recovered via NACK retransmit");
    Check(ra.stats().framesDropped == 0, "no drop when NACK recovers in time");
}

// pktCount lệch giữa hai lần Store của cùng frameId (pktCount lẽ ra cố định,
// phòng gói hỏng): kho nới slot thay vì vứt mảnh, mảnh cũ vẫn tra được.
void TestRetransmitPktCountGrows() {
    std::printf("[retx] mismatched pktCount grows the slot instead of dropping...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    RetransmitCache cache;

    for (auto& d : Packetize(pk, MakeIdrFrame(500, 2), 1'000'000)) cache.Store(d);
    // Cùng frameId nhưng frame "to hơn" (4 mảnh) — mảnh 3 nằm ngoài slot 2 mảnh cũ.
    auto pkts = Packetize(pk, MakeIdrFrame(500, 4), 1'000'000);
    cache.Store(pkts[3]);
    Check(!cache.Find(500, 3).empty(), "slot grew to fit the larger pktCount");
    Check(!cache.Find(500, 0).empty(), "earlier packets survive the resize");
}

} // namespace

void RunRetransmitCacheTests() {
    TestRetransmitCache();
    TestNackEndToEnd();
    TestRetransmitPktCountGrows();
}
