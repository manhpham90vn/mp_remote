// Self-test M1 (xem NetTest.h). Chi dung core (rgc) + C++ chuan - de sau nay
// copy nguyen sang test rieng cua core/ khi core build standalone.
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

// PRNG xac dinh (xorshift32) de moi lan chay cho cung ket qua.
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

// Tao chuoi frame gia: IDR moi `gop` frame, kich thuoc tron cac ca bien
// (nho / dung 1 payload / 1 payload + 1 byte / nhieu manh).
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
    if (!h) { Check(false, "ParseCommonHeader tren goi tu Packetizer"); return; }
    const auto v = ParseVideoPacket(*h, PayloadOf(d));
    if (!v) { Check(false, "ParseVideoPacket tren goi tu Packetizer"); return; }
    ra.Push(*v, nowUs);
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
    Check(n == kCommonHeaderSize + 13, "kich thuoc HELLO");
    auto ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::Hello && ch->sessionId == 0, "header HELLO");
    auto hp = ParseHello(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(hp && hp->clientId == h.clientId && hp->codecMask == h.codecMask &&
          hp->maxWidth == h.maxWidth && hp->maxHeight == h.maxHeight &&
          hp->desiredFps == h.desiredFps && hp->features == h.features, "payload HELLO");

    HelloAck a{0xCAFE0001, Codec::H264, 1920, 1080, 60, 20'000'000, 123'456'789'012ull};
    n = BuildHelloAck(buf, a);
    auto ap = ParseHelloAck(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(ap && ap->sessionId == a.sessionId && ap->codec == a.codec &&
          ap->width == a.width && ap->height == a.height && ap->fps == a.fps &&
          ap->bitrateBps == a.bitrateBps && ap->timebaseUs == a.timebaseUs, "payload HELLO_ACK");

    PingPong p{7, 999'999'999'999ull};
    n = BuildPing(buf, 0xCAFE0001, p);
    ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::Ping && ch->sessionId == 0xCAFE0001, "header PING");
    auto pp = ParsePingPong(PayloadOf(std::span<const uint8_t>(buf, n)));
    Check(pp && pp->pingId == p.pingId && pp->sendTimeUs == p.sendTimeUs, "payload PING");

    n = BuildRequestKeyframe(buf, 0xCAFE0001);
    ch = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(ch && ch->type == MsgType::RequestKeyframe, "REQUEST_KEYFRAME");
}

void TestInOrder() {
    std::printf("[nettest] giao dung thu tu...\n");
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
                  "frame ra == frame vao (in-order)");
            ++popped;
        }
        now += 16'667;
    }
    Check(popped == frames.size(), "du 60 frame (in-order)");
    Check(ra.stats().framesDropped == 0 && !ra.TakeLossEvent(), "khong loss (in-order)");
}

void TestReorder() {
    std::printf("[nettest] tron thu tu trong cua so 2 frame...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(40, 20);
    uint64_t now = 1'000'000;
    size_t popped = 0;
    for (size_t i = 0; i < frames.size(); i += 2) {
        // Gop goi cua 2 frame lien nhau roi xao tron ngau nhien.
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
            Check(SameFrame(*out, frames[popped]), "frame ra dung thu tu (reorder)");
            ++popped;
        }
        now += 2 * 16'667;
    }
    Check(popped == frames.size(), "du 40 frame (reorder)");
    Check(ra.stats().framesDropped == 0 && !ra.TakeLossEvent(), "khong loss (reorder)");
}

void TestDropPacket() {
    std::printf("[nettest] mat 1 goi -> bo frame, loss event, nuot toi IDR...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    // Frame co dinh 5 manh, IDR moi 10 frame.
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
        if (f.id == 5) pkts.erase(pkts.begin() + 2); // mat manh giua cua frame 5
        for (const auto& d : pkts) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) got.push_back(out->frameId);
        sawLoss = sawLoss || ra.TakeLossEvent();
        now += 16'667;
    }
    // Mong doi: 0..4 phat binh thuong, 5 bo, 6..9 bi nuot, 10..19 phat lai tu IDR.
    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 5; ++i) want.push_back(i);
    for (uint32_t i = 10; i < 20; ++i) want.push_back(i);
    Check(got == want, "chuoi frame sau mat goi dung chinh sach");
    Check(sawLoss, "co loss event sau khi bo frame");
    Check(ra.stats().framesDropped == 1 && ra.stats().packetsLost == 1, "thong ke drop/lost");
    Check(ra.stats().framesSkipped == 4, "4 frame non-IDR bi nuot");
}

void TestDuplicates() {
    std::printf("[nettest] goi trung lap...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(20, 10);
    uint64_t now = 1'000'000;
    size_t popped = 0;
    for (const auto& f : frames) {
        const auto pkts = Packetize(pk, f, now);
        for (const auto& d : pkts) Feed(ra, d, now);
        for (const auto& d : pkts) Feed(ra, d, now); // phat lai toan bo
        while (auto out = ra.PopReady(now)) {
            Check(SameFrame(*out, frames[popped]), "frame ra dung du goi trung");
            ++popped;
        }
        now += 16'667;
    }
    Check(popped == frames.size(), "du frame (duplicate)");
    Check(ra.stats().framesDropped == 0, "khong drop (duplicate)");
}

void TestJoinMidStream() {
    std::printf("[nettest] join giua chung -> cho IDR dau tien...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    const auto frames = MakeFrames(16, 10); // IDR tai 0 va 10; ta bo qua frame 0
    uint64_t now = 1'000'000;
    std::vector<uint32_t> got;
    for (size_t i = 1; i < frames.size(); ++i) {
        Check(ra.WaitingForIdr() == (got.empty()), "WaitingForIdr truoc IDR dau");
        for (const auto& d : Packetize(pk, frames[i], now)) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) got.push_back(out->frameId);
        now += 16'667;
    }
    std::vector<uint32_t> want{10, 11, 12, 13, 14, 15};
    Check(got == want, "chi phat tu IDR (join giua chung)");
    Check(ra.stats().framesSkipped == 9, "9 frame truoc IDR bi nuot");
    Check(!ra.TakeLossEvent(), "nuot khi cho IDR khong phai loss");
}

void TestHeadTimeout() {
    std::printf("[nettest] frame thieu manh qua 2 khoang frame -> bo theo timeout...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    Reassembler ra(16'667);
    std::vector<TestFrame> frames;
    for (uint32_t i = 0; i < 4; ++i) {
        TestFrame f{i, i == 0 || i == 3, {}}; // IDR o 0 va 3
        f.nal.resize(3 * kMaxVideoPayload);
        for (auto& b : f.nal) b = uint8_t(Rnd());
        frames.push_back(std::move(f));
    }
    uint64_t now = 1'000'000;
    for (const auto& d : Packetize(pk, frames[0], now)) Feed(ra, d, now);
    auto out = ra.PopReady(now);
    Check(out && out->frameId == 0, "frame 0 phat binh thuong");

    auto pkts1 = Packetize(pk, frames[1], now);
    pkts1.pop_back(); // frame 1 thieu manh cuoi
    for (const auto& d : pkts1) Feed(ra, d, now);
    for (const auto& d : Packetize(pk, frames[2], now)) Feed(ra, d, now);
    Check(!ra.PopReady(now).has_value(), "chua bo khi con trong han");

    now += 40'000; // > 2 * 16667
    Check(!ra.PopReady(now).has_value(), "frame 2 (non-IDR sau loss) khong duoc phat");
    Check(ra.TakeLossEvent(), "loss event sau timeout");

    for (const auto& d : Packetize(pk, frames[3], now)) Feed(ra, d, now);
    out = ra.PopReady(now);
    Check(out && out->frameId == 3, "hoi phuc bang IDR sau loss");
}

// ---- Mo phong 2 session noi bang "day" trong bo nho ----

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

    auto pump = [&] { // giao het goi dang cho o ca hai chieu (khong tre, khong mat)
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

    // HELLO dau tien bi "mat" -> retry sau 500ms phai toi noi.
    cli.Start(Hello{0x11223344, kCodecMaskH264, 2560, 1440, 60, 0}, now);
    w.toHost.clear(); // gia lap mat goi HELLO
    now += 600'000;
    cli.Tick(now); // phat lai HELLO
    pump();
    Check(cliReady, "onReady sau HELLO_ACK (qua retry)");
    Check(np.width == 1920 && np.height == 1080 && np.fps == 60, "tham so dam phan");
    Check(host.state() == HostSession::State::Streaming, "host STREAMING sau START");
    Check(hostStarted, "onStart da goi (force IDR mo man)");
    Check(cli.sessionId() == host.sessionId() && cli.sessionId() != 0, "sessionId khop");

    // Goi video dau tien toi -> client sang STREAMING.
    cli.NotifyVideoPacket(now);
    Check(cli.state() == ClientSession::State::Streaming, "client STREAMING khi co video");

    // PING/PONG do RTT.
    now += 1'100'000;
    cli.Tick(now);
    host.Tick(now);
    const uint64_t pingSent = now;
    now += 30'000; // gia lap 30ms tren duong day
    pump();
    Check(cliRtt == uint32_t(now - pingSent), "RTT = tre khu hoi gia lap");

    // Xin keyframe (co retry 250ms) -> host bao len encoder.
    cli.RequestKeyframe();
    now += 260'000;
    cli.Tick(now);
    pump();
    Check(hostKeyframeReq, "REQUEST_KEYFRAME toi host");

    // HELLO tu client khac trong khi dang ban -> tu choi.
    {
        WirePair w2;
        std::string otherDead;
        ClientCallbacks c2 = ccb;
        c2.send = [&](std::span<const uint8_t> d) { w2.toHost.emplace_back(d.begin(), d.end()); };
        c2.onReady = [&](const NegotiatedParams&) {};
        c2.onDisconnect = [&](const char* r) { otherDead = r; };
        ClientSession other(c2);
        other.Start(Hello{0x55667788, kCodecMaskH264, 1280, 720, 30, 0}, now);
        // Dua HELLO cua client 2 vao host; host dang send vao w.toClient —
        // ACK tu choi vua sinh ra duoc chuyen ngay cho client 2.
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
        Check(!otherDead.empty(), "client thu hai bi tu choi khi host ban");
        Check(host.state() == HostSession::State::Streaming, "phien cu khong bi anh huong");
    }

    // BYE -> host quay ve IDLE.
    cli.SendBye();
    pump();
    Check(hostDisconnected && host.state() == HostSession::State::Idle, "BYE -> host IDLE");

    // Timeout: client 2 gui duoc HELLO (host sang READY) roi ca hai im lang.
    // (pump() giao ACK cho `cli` cu nen cli2 khong bao gio nhan ACK — dung y:
    // ta chi can hai phia tu thoat. Host timeout 5s; client bo cuoc HELLO 10s.)
    hostDisconnected = false;
    ClientSession cli2(ccb);
    cliDead.clear();
    cli2.Start(Hello{0x99AA0001, kCodecMaskH264, 1920, 1080, 60, 0}, now);
    while (!w.toHost.empty()) { // chi giao chieu client->host
        host.HandlePacket(w.toHost.front(), now);
        w.toHost.pop_front();
    }
    w.toClient.clear(); // ACK "mat" tren duong ve
    Check(host.state() != HostSession::State::Idle, "phien thu hai thiet lap duoc");
    now += 11'000'000;
    host.Tick(now);
    Check(hostDisconnected && host.state() == HostSession::State::Idle, "timeout -> host IDLE");
    cli2.Tick(now);
    Check(!cliDead.empty(), "client bo cuoc khi host im lang");
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

// Wire: mot batch event di qua build/parse phai ve nguyen ven tung truong.
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
    Check(n > 0, "BuildInputEvents thanh cong");
    const auto h = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(h && h->type == MsgType::InputEvent && h->chan == Chan::Input, "header input dung");
    Check(h && h->sessionId == 0xCAFEBABE, "sessionId giu nguyen");

    InputEvent out[kMaxInputEvents];
    uint32_t firstSeq = 0;
    const size_t got = ParseInputEvents(PayloadOf(std::span<const uint8_t>(buf, n)),
                                        firstSeq, out);
    Check(got == in.size() && firstSeq == 77, "so event + firstSeq dung");
    for (size_t i = 0; i < got && i < in.size(); ++i) {
        Check(out[i].type == in[i].type && out[i].a == in[i].a && out[i].b == in[i].b &&
              out[i].state == in[i].state && out[i].absolute == in[i].absolute &&
              out[i].timestampUs == in[i].timestampUs, "event roundtrip nguyen ven");
    }
    // So am phai song sot (a = -1234) - loi kinh dien khi ep i32 qua u32.
    Check(got >= 2 && out[1].a == -1234, "toa do am giu dung dau");
}

// Duong ong day du: sender -> (mo phong mat goi) -> receiver.
// Yeu cau then chot: KHONG event nao bi ap dung hai lan, va gui lap phai bu
// duoc goi mat (neu khong -> ket phim).
void TestInputSenderReceiver() {
    std::printf("[M1] Input sender/receiver: khu trung + bu goi mat...\n");
    InputSender sender;
    sender.SetSessionId(1234);
    InputReceiver receiver;

    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    // 20 lan nhan/nha phim, moi lan Flush ngay (giong go phim thuc te).
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
    // Vai vong khong co event moi -> sender phat lai duoi.
    for (int i = 0; i < 4; ++i) { now += kInputRepeatIntervalUs; sender.Flush(now, send); }

    // Bo 1/3 so datagram (mat goi nang hon thuc te nhieu).
    std::vector<InputEvent> applied;
    auto apply = [&](const InputEvent& e) { applied.push_back(e); };
    size_t dropped = 0;
    for (size_t i = 0; i < wire.size(); ++i) {
        if (i % 3 == 1) { ++dropped; continue; }
        receiver.HandlePacket(PayloadOf(wire[i]), apply);
    }
    Check(dropped > 0, "co gia lap mat goi");
    Check(applied.size() == sentEvents.size(),
          "moi event den dung mot lan du mat 1/3 goi (khong ket phim, khong lap)");
    for (size_t i = 0; i < applied.size() && i < sentEvents.size(); ++i) {
        Check(applied[i].a == sentEvents[i].a && applied[i].state == sentEvents[i].state,
              "event dung thu tu va noi dung");
    }
    Check(receiver.stats().duplicates > 0, "co ban lap bi khu (dung la co gui lap)");
    Check(receiver.stats().lost == 0, "khong mat event nao");
}

// Goi den dao thu tu khong duoc "tua nguoc" thao tac: gia su goi cu ve sau,
// moi event trong do deu la seq cu -> phai bi bo het.
void TestInputReorder() {
    std::printf("[M1] Input dao thu tu...\n");
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
    // Nhan goi cuoi TRUOC, roi moi den cac goi truoc do.
    receiver.HandlePacket(PayloadOf(wire.back()), apply);
    const size_t afterNewest = applied.size();
    for (size_t i = 0; i + 1 < wire.size(); ++i)
        receiver.HandlePacket(PayloadOf(wire[i]), apply);
    Check(applied.size() == afterNewest, "goi den tre khong ap dung lai event cu");
    Check(applied.back().a == 'A' + 5, "trang thai cuoi la event moi nhat");
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
    std::printf("=== GD4 M1: self-test input (offline) ===\n");
    TestInputWireRoundtrip();
    TestInputSenderReceiver();
    TestInputReorder();
    if (g_failures == 0) {
        std::printf("=== NETTEST PASS: moi kiem tra dat ===\n");
        return 0;
    }
    std::printf("=== NETTEST FAIL: %d kiem tra truot ===\n", g_failures);
    return 1;
}
