// Self-test M1 (xem NetTest.h). Chỉ dùng core (rgc) + C++ chuẩn - để sau này
// copy nguyên sang test riêng của core/ khi core build standalone.
#include "NetTest.h"

#include "rgc/Packetizer.h"
#include "rgc/Reassembler.h"
#include "rgc/HostSession.h"
#include "rgc/ClientSession.h"
#include "rgc/InputSender.h"
#include "rgc/InputReceiver.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace rgc;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

// PRNG xác định (xorshift32) để mỗi lần chạy cho cùng kết quả.
uint32_t g_rng = 0x1234ABCD;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

struct TestFrame {
    uint32_t id;
    bool idr;
    std::vector<uint8_t> nal;
};

// Tạo chuỗi frame giả: IDR mỗi `gop` frame, kích thước trộn các ca biên
// (nhỏ / đúng 1 payload / 1 payload + 1 byte / nhiều mảnh).
std::vector<TestFrame> MakeFrames(size_t count, size_t gop) {
    std::vector<TestFrame> v;
    v.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        TestFrame f;
        f.id = uint32_t(i);
        f.idr = (i % gop) == 0;
        size_t size = 0;
        switch (Rnd() % 4) {
        case 0: size = 80 + Rnd() % 300; break;
        case 1: size = kMaxVideoPayload; break;
        case 2: size = kMaxVideoPayload + 1; break;
        default: size = 8'000 + Rnd() % 40'000; break;
        }
        f.nal.resize(size);
        for (auto& b : f.nal) b = uint8_t(Rnd());
        v.push_back(std::move(f));
    }
    return v;
}

using Datagram = std::vector<uint8_t>;

std::vector<Datagram> Packetize(Packetizer& pk, const TestFrame& f, uint64_t tsUs) {
    std::vector<Datagram> out;
    pk.SendFrame(f.nal, f.id, tsUs, f.idr,
                 [&](std::span<const uint8_t> d) { out.emplace_back(d.begin(), d.end()); });
    return out;
}

void Feed(Reassembler& ra, const Datagram& d, uint64_t nowUs) {
    const auto h = ParseCommonHeader(d);
    if (!h) { Check(false, "ParseCommonHeader on packet from Packetizer"); return; }
    if (h->type == MsgType::FecPacket) {
        const auto v = ParseFecPacket(*h, PayloadOf(d));
        if (!v) { Check(false, "ParseFecPacket on packet from Packetizer"); return; }
        ra.PushFec(*v, nowUs);
        return;
    }
    const auto v = ParseVideoPacket(*h, PayloadOf(d));
    if (!v) { Check(false, "ParseVideoPacket on packet from Packetizer"); return; }
    ra.Push(*v, nowUs);
}

bool IsFec(const Datagram& d) {
    const auto h = ParseCommonHeader(d);
    return h && h->type == MsgType::FecPacket;
}

// Chỉ số (trong danh sách datagram) của gói DỮ LIỆU thứ n, bỏ qua các gói parity.
size_t NthDataPacket(const std::vector<Datagram>& pkts, size_t n) {
    size_t seen = 0;
    for (size_t i = 0; i < pkts.size(); ++i) {
        if (IsFec(pkts[i])) continue;
        if (seen == n) return i;
        ++seen;
    }
    return pkts.size();
}

bool SameFrame(const Reassembler::Frame& got, const TestFrame& want) {
    return got.frameId == want.id && got.idr == want.idr && got.nal == want.nal;
}

// ---- Cac bai test ----

void TestWireRoundtrip() {
    std::printf("[nettest] wire round-trip...\n");
    uint8_t buf[kMaxDatagram];

    Hello h{0xDEADBEEF, kCodecMaskH264 | kCodecMaskHevc, 2560, 1440, 120, 0x0001};
    size_t n = BuildHello(buf, h);
    Check(n == kCommonHeaderSize + 14, "HELLO size"); // +1 byte sourceId ở GD6
    auto ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::Hello && ch->sessionId == 0, "HELLO header");
    auto hp = ParseHello(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(hp && hp->clientId == h.clientId && hp->codecMask == h.codecMask &&
          hp->maxWidth == h.maxWidth && hp->maxHeight == h.maxHeight &&
          hp->desiredFps == h.desiredFps && hp->features == h.features, "HELLO payload");

    HelloAck a{0xCAFE0001, Codec::H264, 1920, 1080, 60, 20'000'000, 123'456'789'012ull};
    n = BuildHelloAck(buf, a);
    auto ap = ParseHelloAck(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(ap && ap->sessionId == a.sessionId && ap->codec == a.codec &&
          ap->width == a.width && ap->height == a.height && ap->fps == a.fps &&
          ap->bitrateBps == a.bitrateBps && ap->timebaseUs == a.timebaseUs, "HELLO_ACK payload");

    PingPong p{7, 999'999'999'999ull};
    n = BuildPing(buf, 0xCAFE0001, p);
    ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::Ping && ch->sessionId == 0xCAFE0001, "PING header");
    auto pp = ParsePingPong(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(pp && pp->pingId == p.pingId && pp->sendTimeUs == p.sendTimeUs, "PING payload");

    n = BuildRequestKeyframe(buf, 0xCAFE0001);
    ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::RequestKeyframe, "REQUEST_KEYFRAME");
}

void TestInOrder() {
    std::printf("[nettest] in-order delivery...\n");
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
    std::printf("[nettest] shuffled order within a 2-frame window...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(40, 20);
    uint64_t now = 1'000'000;
    size_t popped = 0;
    for (size_t i = 0; i < frames.size(); i += 2) {
        // Gộp gói của 2 frame liền nhau rồi xáo trộn ngẫu nhiên.
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
    std::printf("[nettest] drop 1 packet -> drop frame, loss event, swallow until IDR...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    // Frame cố định 5 mảnh, IDR mỗi 10 frame.
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
    // Mong đợi: 0..4 phát bình thường, 5 bỏ, 6..9 bị nuốt, 10..19 phát lại từ IDR.
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 5; ++i) want.push_back(i);
    for (uint32_t i = 10; i < 20; ++i) want.push_back(i);
    Check(got == want, "frame sequence after packet loss matches policy");
    Check(sawLoss, "loss event occurred after dropping frame");
    Check(ra.stats().framesDropped == 1 && ra.stats().packetsLost == 1, "drop/lost stats");
    Check(ra.stats().framesSkipped == 4, "4 non-IDR frames swallowed");
}

// ---- GD6: nhiều nguồn ----

void TestSourceListWire() {
    std::printf("[nettest] SOURCE_LIST + HELLO.sourceId round-trip...\n");
    uint8_t buf[kMaxDatagram];

    // Tên UTF-8 viết bằng escape thay vì ký tự thật: khỏi phụ thuộc mã hóa file
    // nguồn và trình biên dịch có bật /utf-8 hay không. "\xE1\xBA\xA1" = "ạ" (3 byte).
    const std::string kViet = "Cua so \xE1\xBA\xA1\xE1\xBA\xA1";

    std::vector<SourceInfo> in;
    in.push_back(SourceInfo{0, 1920, 1080, "Screen 1 (primary)"});
    in.push_back(SourceInfo{1, 1689, 1392, "Notepad"});
    in.push_back(SourceInfo{7, 800, 600, kViet});

    size_t n = BuildSourceList(buf, in);
    Check(n > 0 && n <= kMaxDatagram, "SOURCE_LIST fits one datagram");
    auto ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::SourceList, "SOURCE_LIST header");

    SourceInfo out[kMaxSources];
    size_t cnt = ParseSourceList(PayloadOf(std::span<const uint8_t>(buf, n)), out);
    Check(cnt == in.size(), "SOURCE_LIST count");
    bool same = cnt == in.size();
    for (size_t i = 0; same && i < cnt; ++i)
        same = out[i].sourceId == in[i].sourceId && out[i].width == in[i].width &&
               out[i].height == in[i].height && out[i].name == in[i].name;
    Check(same, "SOURCE_LIST entries survive round-trip (including UTF-8 names)");

    // Tên dài bị cắt, nhưng phải cắt ở ranh giới ký tự UTF-8 chứ không giữa chừng.
    // Ký tự 3 byte, và kMaxSourceNameBytes=64 KHÔNG chia hết cho 3 — cắt ngây thơ ở
    // đúng 64 sẽ xé đôi một ký tự, nên ca này bắt được lỗi đó (đáp án đúng: 63).
    std::vector<SourceInfo> longName;
    std::string vn;
    while (vn.size() < kMaxSourceNameBytes + 20) vn += "\xE1\xBA\xA1";
    longName.push_back(SourceInfo{3, 640, 480, vn});
    n = BuildSourceList(buf, longName);
    cnt = ParseSourceList(PayloadOf(std::span<const uint8_t>(buf, n)), out);
    Check(cnt == 1, "long-name SOURCE_LIST parses");
    if (cnt == 1) {
        Check(out[0].name.size() <= kMaxSourceNameBytes, "long name truncated to limit");
        Check(out[0].name.size() % 3 == 0, "truncation landed on a UTF-8 boundary");
        Check(vn.compare(0, out[0].name.size(), out[0].name) == 0, "truncated name is a prefix");
    }

    // HELLO mang sourceId; gói 13 byte kiểu cũ vẫn đọc được, hiểu là nguồn 0.
    Hello h{0xDEADBEEF, kCodecMaskH264, 2560, 1440, 120, 0, 5};
    n = BuildHello(buf, h);
    auto hp = ParseHello(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(hp && hp->sourceId == 5, "HELLO carries sourceId");
    auto old = ParseHello(PayloadOf(std::span<const uint8_t>(buf, n - 1)));
    Check(old && old->sourceId == 0, "13-byte HELLO still parses as source 0");
}

// ---- GD5: FEC ----

// Khung sườn chung: 20 frame cố định `pktsPerFrame` mảnh, IDR mỗi 10 frame, bật FEC,
// bỏ các gói dữ liệu theo `dropIdx` ở MỌI frame không phải IDR.
struct FecCase {
    size_t pktsPerFrame;
    std::vector<size_t> dropIdx; // chỉ số gói dữ liệu bị bỏ (trong frame)
};

void RunFecCase(const FecCase& c, std::vector<uint32_t>& got, Reassembler::Stats& stats,
                std::vector<TestFrame>& frames) {
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);
    frames.clear();
    for (uint32_t i = 0; i < 20; ++i) {
        TestFrame f{i, (i % 10) == 0, {}};
        // -100 để gói cuối ngắn hơn: ép nhánh lenXor phải đúng.
        f.nal.resize(c.pktsPerFrame * kMaxVideoPayload - 100);
        for (auto& b : f.nal) b = uint8_t(Rnd());
        frames.push_back(std::move(f));
    }
    uint64_t now = 1'000'000;
    for (const auto& f : frames) {
        auto pkts = Packetize(pk, f, now);
        if (!f.idr) {
            // Xóa từ cuối lên để chỉ số trước không bị dịch.
            std::vector<size_t> pos;
            for (size_t d : c.dropIdx) pos.push_back(NthDataPacket(pkts, d));
            std::sort(pos.begin(), pos.end(), std::greater<size_t>());
            for (size_t p : pos) if (p < pkts.size()) pkts.erase(pkts.begin() + p);
        }
        for (const auto& d : pkts) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) got.push_back(out->frameId);
        now += 16'667;
    }
    stats = ra.stats();
}

void TestFecRecoverOne() {
    std::printf("[nettest] FEC: 1 packet lost per frame -> recovered, no drop...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    RunFecCase({5, {2}}, got, st, frames);

    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 20; ++i) want.push_back(i);
    Check(got == want, "all frames delivered despite 1 loss each");
    Check(st.framesDropped == 0, "no frame dropped (FEC)");
    Check(st.packetsRecovered == 18, "18 non-IDR frames each recovered 1 packet");
}

void TestFecRecoverLastPacket() {
    std::printf("[nettest] FEC: lost packet is the SHORT last one -> length restored...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    RunFecCase({5, {4}}, got, st, frames); // mảnh 4 = mảnh cuối, ngắn hơn kMaxVideoPayload

    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 20; ++i) want.push_back(i);
    Check(got == want, "all frames delivered when the short tail packet is lost");
    Check(st.framesDropped == 0, "no frame dropped (FEC, tail packet)");
}

void TestFecContentIntact() {
    std::printf("[nettest] FEC: recovered frame is byte-identical...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);

    TestFrame f{0, true, {}};
    f.nal.resize(5 * kMaxVideoPayload - 100);
    for (auto& b : f.nal) b = uint8_t(Rnd());

    auto pkts = Packetize(pk, f, 1'000'000);
    pkts.erase(pkts.begin() + NthDataPacket(pkts, 3));
    for (const auto& d : pkts) Feed(ra, d, 1'000'000);

    auto out = ra.PopReady(1'000'000);
    Check(out.has_value(), "frame completed via FEC");
    if (out) Check(SameFrame(*out, f), "recovered frame byte-identical to original");
}

void TestFecTwoLossesSameGroup() {
    std::printf("[nettest] FEC: 2 losses in one group -> cannot recover, old policy...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    // 5 mảnh = 1 nhóm (kFecGroupSize=8), bỏ 2 -> parity vô dụng.
    RunFecCase({5, {1, 3}}, got, st, frames);

    // Giống hệt ca không có FEC: frame 1..9 hỏng/nuốt, phát lại từ IDR 10.
    Check(st.packetsRecovered == 0, "nothing recovered with 2 losses in a group");
    Check(st.framesDropped > 0, "frames still dropped with 2 losses in a group");
    Check(std::find(got.begin(), got.end(), 10u) != got.end(), "recovers at next IDR");
}

void TestFecSinglePacketFrame() {
    std::printf("[nettest] FEC: 1-packet frame, data lost, parity alone rebuilds it...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);

    TestFrame f{0, true, {}};
    f.nal.resize(300);
    for (auto& b : f.nal) b = uint8_t(Rnd());

    auto pkts = Packetize(pk, f, 1'000'000);
    Check(pkts.size() == 2, "1 data packet + 1 parity packet");
    pkts.erase(pkts.begin() + NthDataPacket(pkts, 0)); // bỏ gói dữ liệu duy nhất
    for (const auto& d : pkts) Feed(ra, d, 1'000'000);

    auto out = ra.PopReady(1'000'000);
    Check(out.has_value(), "single-packet frame rebuilt from parity alone");
    if (out) Check(SameFrame(*out, f), "rebuilt single-packet frame identical");
}

void TestFecDisabledByDefault() {
    std::printf("[nettest] FEC off by default -> no parity packets on the wire...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    TestFrame f{0, true, {}};
    f.nal.resize(3 * kMaxVideoPayload);
    for (auto& b : f.nal) b = uint8_t(Rnd());
    const auto pkts = Packetize(pk, f, 1'000'000);
    Check(pkts.size() == 3, "3 data packets, no parity when FEC disabled");
    for (const auto& d : pkts) Check(!IsFec(d), "no FEC packet when disabled");
}

void TestDuplicates() {
    std::printf("[nettest] duplicate packets...\n");
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
    std::printf("[nettest] join mid-stream -> wait for first IDR...\n");
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
    std::printf("[nettest] frame missing a piece past 2 frame intervals -> drop on timeout...\n");
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

// ---- Mô phỏng 2 session nối bằng "dây" trong bộ nhớ ----

struct WirePair {
    std::deque<Datagram> toHost, toClient;
};

void TestSessions() {
    std::printf("[nettest] handshake HostSession <-> ClientSession...\n");
    WirePair w;
    uint64_t now = 10'000'000;

    bool hostStarted = false, hostKeyframeReq = false, hostDisconnected = false;
    HostCallbacks hcb;
    hcb.send = [&](std::span<const uint8_t> d) { w.toClient.emplace_back(d.begin(), d.end()); };
    hcb.onStart = [&] { hostStarted = true; };
    hcb.onKeyframeRequest = [&] { hostKeyframeReq = true; };
    hcb.onDisconnect = [&] { hostDisconnected = true; };
    HostSession host(hcb, StreamParams{1920, 1080, 60, 20'000'000});

    bool cliReady = false;
    uint32_t cliRtt = 0;
    std::string cliDead;
    NegotiatedParams np{};
    ClientCallbacks ccb;
    ccb.send = [&](std::span<const uint8_t> d) { w.toHost.emplace_back(d.begin(), d.end()); };
    ccb.onReady = [&](const NegotiatedParams& p) { cliReady = true; np = p; };
    ccb.onRtt = [&](uint32_t r) { cliRtt = r; };
    ccb.onDisconnect = [&](const char* r) { cliDead = r; };
    ClientSession cli(ccb);

    auto pump = [&] { // giao hết gói đang chờ ở cả hai chiều (không trễ, không mất)
        for (int guard = 0; guard < 8; ++guard) {
            if (w.toHost.empty() && w.toClient.empty()) break;
            while (!w.toHost.empty()) {
                auto d = std::move(w.toHost.front());
                w.toHost.pop_front();
                host.HandlePacket(d, now);
            }
            while (!w.toClient.empty()) {
                auto d = std::move(w.toClient.front());
                w.toClient.pop_front();
                cli.HandlePacket(d, now);
            }
        }
    };

    // HELLO đầu tiên bị "mất" -> retry sau 500ms phải tới nơi.
    cli.Start(Hello{0x11223344, kCodecMaskH264, 2560, 1440, 60, 0}, now);
    w.toHost.clear(); // giả lập mất gói HELLO
    now += 600'000;
    cli.Tick(now); // phát lại HELLO
    pump();
    Check(cliReady, "onReady after HELLO_ACK (via retry)");
    Check(np.width == 1920 && np.height == 1080 && np.fps == 60, "negotiated parameters");
    Check(host.state() == HostSession::State::Streaming, "host STREAMING after START");
    Check(hostStarted, "onStart was called (force IDR to open)");
    Check(cli.sessionId() == host.sessionId() && cli.sessionId() != 0, "sessionId matches");

    // Gói video đầu tiên tới -> client sang STREAMING.
    cli.NotifyVideoPacket(now);
    Check(cli.state() == ClientSession::State::Streaming, "client STREAMING when video present");

    // PING/PONG đo RTT.
    now += 1'100'000;
    cli.Tick(now);
    host.Tick(now);
    const uint64_t pingSent = now;
    now += 30'000; // giả lập 30ms trên đường dây
    pump();
    Check(cliRtt == uint32_t(now - pingSent), "RTT = simulated round-trip delay");

    // Xin keyframe (có retry 250ms) -> host báo lên encoder.
    cli.RequestKeyframe();
    now += 260'000;
    cli.Tick(now);
    pump();
    Check(hostKeyframeReq, "REQUEST_KEYFRAME reaches host");

    // HELLO từ client khác trong khi đang bận -> từ chối.
    {
        WirePair w2;
        std::string otherDead;
        ClientCallbacks c2 = ccb;
        c2.send = [&](std::span<const uint8_t> d) { w2.toHost.emplace_back(d.begin(), d.end()); };
        c2.onReady = [&](const NegotiatedParams&) {};
        c2.onDisconnect = [&](const char* r) { otherDead = r; };
        ClientSession other(c2);
        other.Start(Hello{0x55667788, kCodecMaskH264, 1280, 720, 30, 0}, now);
        // Đưa HELLO của client 2 vào host; host đang send vào w.toClient —
        // ACK từ chối vừa sinh ra được chuyển ngay cho client 2.
        while (!w2.toHost.empty()) {
            auto d = std::move(w2.toHost.front());
            w2.toHost.pop_front();
            const size_t before = w.toClient.size();
            host.HandlePacket(d, now);
            while (w.toClient.size() > before) {
                other.HandlePacket(w.toClient.back(), now);
                w.toClient.pop_back();
            }
        }
        Check(!otherDead.empty(), "second client refused while host is busy");
        Check(host.state() == HostSession::State::Streaming, "existing session unaffected");
    }

    // BYE -> host quay về IDLE.
    cli.SendBye();
    pump();
    Check(hostDisconnected && host.state() == HostSession::State::Idle, "BYE -> host IDLE");

    // Timeout: client 2 gửi được HELLO (host sang READY) rồi cả hai im lặng.
    // (pump() giao ACK cho `cli` cũ nên cli2 không bao giờ nhận ACK — dụng ý:
    // ta chỉ cần hai phía tự thoát. Host timeout 5s; client bỏ cuộc HELLO 10s.)
    hostDisconnected = false;
    ClientSession cli2(ccb);
    cliDead.clear();
    cli2.Start(Hello{0x99AA0001, kCodecMaskH264, 1920, 1080, 60, 0}, now);
    while (!w.toHost.empty()) { // chỉ giao chiều client->host
        host.HandlePacket(w.toHost.front(), now);
        w.toHost.pop_front();
    }
    w.toClient.clear(); // ACK "mất" trên đường về
    Check(host.state() != HostSession::State::Idle, "second session established");
    now += 11'000'000;
    host.Tick(now);
    Check(hostDisconnected && host.state() == HostSession::State::Idle, "timeout -> host IDLE");
    cli2.Tick(now);
    Check(!cliDead.empty(), "client gives up when host goes silent");
}

// ---- GD4: input ----

InputEvent MakeKey(int32_t vk, int32_t scan, bool down) {
    InputEvent e;
    e.type  = InputType::Key;
    e.a     = vk;
    e.b     = scan;
    e.state = down ? 1 : 0;
    return e;
}

// Wire: một batch event đi qua build/parse phải về nguyên vẹn từng trường.
void TestInputWireRoundtrip() {
    std::printf("[M1] Wire input roundtrip...\n");
    std::vector<InputEvent> in;
    in.push_back(MakeKey('W', 0x11, true));
    InputEvent mv;
    mv.type = InputType::MouseMove; mv.a = -1234; mv.b = 5678; mv.absolute = 0;
    in.push_back(mv);
    InputEvent ab;
    ab.type = InputType::MouseMove; ab.a = 65535; ab.b = 0; ab.absolute = 1;
    in.push_back(ab);
    InputEvent wh;
    wh.type = InputType::MouseWheel; wh.b = -120;
    in.push_back(wh);
    for (auto& e : in) e.timestampUs = 0x0123456789ABCDEFull;

    uint8_t buf[kMaxDatagram];
    const size_t n = BuildInputEvents(buf, 0xCAFEBABE, 77, in);
    Check(n > 0, "BuildInputEvents succeeded");
    const auto h = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(h && h->type == MsgType::InputEvent && h->chan == Chan::Input, "input header correct");
    Check(h && h->sessionId == 0xCAFEBABE, "sessionId preserved");

    InputEvent out[kMaxInputEvents];
    uint32_t firstSeq = 0;
    const size_t got = ParseInputEvents(PayloadOf(std::span<const uint8_t>(buf, n)),
                                        firstSeq, out);
    Check(got == in.size() && firstSeq == 77, "event count + firstSeq correct");
    for (size_t i = 0; i < got && i < in.size(); ++i) {
        Check(out[i].type == in[i].type && out[i].a == in[i].a && out[i].b == in[i].b &&
              out[i].state == in[i].state && out[i].absolute == in[i].absolute &&
              out[i].timestampUs == in[i].timestampUs, "event roundtrip intact");
    }
    // Số âm phải sống sót (a = -1234) - lỗi kinh điển khi ép i32 qua u32.
    Check(got >= 2 && out[1].a == -1234, "negative coordinate keeps correct sign");
}

// Đường ống đầy đủ: sender -> (mô phỏng mất gói) -> receiver.
// Yêu cầu then chốt: KHÔNG event nào bị áp dụng hai lần, và gửi lặp phải bù
// được gói mất (nếu không -> kẹt phím).
void TestInputSenderReceiver() {
    std::printf("[M1] Input sender/receiver: dedupe + compensate for lost packets...\n");
    InputSender sender;
    sender.SetSessionId(1234);
    InputReceiver receiver;

    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    // 20 lần nhấn/nhả phím, mỗi lần Flush ngay (giống gõ phím thực tế).
    std::vector<InputEvent> sentEvents;
    uint64_t now = 0;
    for (int i = 0; i < 20; ++i) {
        const bool down = (i % 2) == 0;
        const auto e = MakeKey('A' + (i / 2), 0x1E + (i / 2), down);
        sender.Queue(e);
        sentEvents.push_back(e);
        now += 10'000;
        sender.Flush(now, send);
    }
    // Vài vòng không có event mới -> sender phát lại đuôi.
    for (int i = 0; i < 4; ++i) { now += kInputRepeatIntervalUs; sender.Flush(now, send); }

    // Bỏ 1/3 số datagram (mất gói nặng hơn thực tế nhiều).
    std::vector<InputEvent> applied;
    auto apply = [&](const InputEvent& e) { applied.push_back(e); };
    size_t dropped = 0;
    for (size_t i = 0; i < wire.size(); ++i) {
        if (i % 3 == 1) { ++dropped; continue; }
        receiver.HandlePacket(PayloadOf(wire[i]), apply);
    }
    Check(dropped > 0, "packet loss was simulated");
    Check(applied.size() == sentEvents.size(),
          "every event arrives exactly once despite losing 1/3 of packets (no stuck keys, no duplicates)");
    for (size_t i = 0; i < applied.size() && i < sentEvents.size(); ++i) {
        Check(applied[i].a == sentEvents[i].a && applied[i].state == sentEvents[i].state,
              "event correct order and content");
    }
    Check(receiver.stats().duplicates > 0, "duplicates were deduped (confirms repeats were sent)");
    Check(receiver.stats().lost == 0, "no events lost");
}

// Gói đến đảo thứ tự không được "tua ngược" thao tác: giả sử gói cũ về sau,
// mọi event trong đó đều là seq cũ -> phải bị bỏ hết.
void TestInputReorder() {
    std::printf("[M1] Input out-of-order...\n");
    InputSender sender;
    InputReceiver receiver;
    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    uint64_t now = 0;
    for (int i = 0; i < 6; ++i) {
        sender.Queue(MakeKey('A' + i, 0x1E + i, true));
        now += 10'000;
        sender.Flush(now, send);
    }
    std::vector<InputEvent> applied;
    auto apply = [&](const InputEvent& e) { applied.push_back(e); };
    // Nhận gói cuối TRƯỚC, rồi mới đến các gói trước đó.
    receiver.HandlePacket(PayloadOf(wire.back()), apply);
    const size_t afterNewest = applied.size();
    for (size_t i = 0; i + 1 < wire.size(); ++i)
        receiver.HandlePacket(PayloadOf(wire[i]), apply);
    Check(applied.size() == afterNewest, "late-arriving packet doesn't reapply old events");
    Check(applied.back().a == 'A' + 5, "final state is the newest event");
}

} // namespace

int RunNetTest() {
    std::printf("=== GD3 M1: self-test packetize/reassemble + session (offline) ===\n");
    TestWireRoundtrip();
    TestInOrder();
    TestReorder();
    TestDropPacket();
    TestDuplicates();
    TestJoinMidStream();
    TestHeadTimeout();
    TestSessions();
    std::printf("=== GD6 M1: self-test multi-source wire (offline) ===\n");
    TestSourceListWire();
    std::printf("=== GD5 M1: self-test FEC (offline) ===\n");
    TestFecDisabledByDefault();
    TestFecRecoverOne();
    TestFecRecoverLastPacket();
    TestFecContentIntact();
    TestFecTwoLossesSameGroup();
    TestFecSinglePacketFrame();
    std::printf("=== GD4 M1: self-test input (offline) ===\n");
    TestInputWireRoundtrip();
    TestInputSenderReceiver();
    TestInputReorder();
    if (g_failures == 0) {
        std::printf("=== NETTEST PASS: all checks passed ===\n");
        return 0;
    }
    std::printf("=== NETTEST FAIL: %d checks failed ===\n", g_failures);
    return 1;
}
