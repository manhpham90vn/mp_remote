// =============================================================================
// SessionTests.cpp — bắt tay và vòng đời phiên: nối HostSession <-> ClientSession
// bằng "dây" trong bộ nhớ. Gồm handshake/timeout/reject và định tuyến NACK/
// INVALIDATE_REF (GĐ7).
// =============================================================================
#include "Tests.h"
#include "support/TestSupport.h"

#include "deskhub/session/ClientSession.h"
#include "deskhub/session/HostSession.h"

#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace deskhub;

namespace {

struct WirePair {
    std::deque<Datagram> toHost, toClient;
};

void TestSessions() {
    std::printf("[session] handshake HostSession <-> ClientSession...\n");
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

    auto pump = [&] {
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
    w.toHost.clear();
    now += 600'000;
    cli.Tick(now);
    pump();
    Check(cliReady, "onReady after HELLO_ACK (via retry)");
    Check(np.width == 1920 && np.height == 1080 && np.fps == 60, "negotiated parameters");
    Check(host.state() == HostSession::State::Streaming, "host STREAMING after START");
    Check(hostStarted, "onStart was called (force IDR to open)");
    Check(cli.sessionId() == host.sessionId() && cli.sessionId() != 0, "sessionId matches");

    cli.NotifyVideoPacket(now);
    Check(cli.state() == ClientSession::State::Streaming, "client STREAMING when video present");

    // PING/PONG đo RTT.
    now += 1'100'000;
    cli.Tick(now);
    host.Tick(now);
    const uint64_t pingSent = now;
    now += 30'000;
    pump();
    Check(cliRtt == uint32_t(now - pingSent), "RTT = simulated round-trip delay");

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

    // Timeout: client 2 gửi HELLO (host sang READY) rồi cả hai im lặng.
    hostDisconnected = false;
    ClientSession cli2(ccb);
    cliDead.clear();
    cli2.Start(Hello{0x99AA0001, kCodecMaskH264, 1920, 1080, 60, 0}, now);
    while (!w.toHost.empty()) {
        host.HandlePacket(w.toHost.front(), now);
        w.toHost.pop_front();
    }
    w.toClient.clear();
    Check(host.state() != HostSession::State::Idle, "second session established");
    now += 11'000'000;
    host.Tick(now);
    Check(hostDisconnected && host.state() == HostSession::State::Idle, "timeout -> host IDLE");
    cli2.Tick(now);
    Check(!cliDead.empty(), "client gives up when host goes silent");
}

// GĐ7: NACK và INVALIDATE_REF từ client được host định tuyến đúng callback; và cả
// hai bị bỏ qua khi client chưa STREAMING.
void TestSessionsNackInvalidate() {
    std::printf("[session] NACK / INVALIDATE_REF routing + pre-stream gating...\n");
    WirePair w;
    uint64_t now = 10'000'000;

    uint32_t nackFrame = 0, invFrame = 0;
    std::vector<uint16_t> nackIdx;
    HostCallbacks hcb;
    hcb.send = [&](std::span<const uint8_t> d) { w.toClient.emplace_back(d.begin(), d.end()); };
    hcb.onNack = [&](uint32_t fid, std::span<const uint16_t> idx) {
        nackFrame = fid;
        nackIdx.assign(idx.begin(), idx.end());
    };
    hcb.onInvalidateRef = [&](uint32_t fid) { invFrame = fid; };
    HostSession host(hcb, StreamParams{1920, 1080, 60, 20'000'000});

    ClientCallbacks ccb;
    ccb.send = [&](std::span<const uint8_t> d) { w.toHost.emplace_back(d.begin(), d.end()); };
    ClientSession cli(ccb);

    auto pump = [&] {
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

    cli.Start(Hello{0x1, kCodecMaskH264, 1920, 1080, 60, 0}, now);
    pump();
    cli.NotifyVideoPacket(now);
    Check(host.state() == HostSession::State::Streaming &&
              cli.state() == ClientSession::State::Streaming,
        "both sides streaming");

    const uint16_t idx[] = {1, 4, 7};
    cli.SendNack(0xABCD, idx);
    cli.SendInvalidateRef(0x1234);
    pump();
    Check(nackFrame == 0xABCD && nackIdx.size() == 3 && nackIdx[0] == 1 && nackIdx[2] == 7,
        "host routed NACK to onNack with the right indices");
    Check(invFrame == 0x1234, "host routed INVALIDATE_REF to onInvalidateRef");

    // Trước khi STREAMING, cả hai là no-op (không có phiên để gửi lên).
    ClientSession idle(ccb);
    const size_t before = w.toHost.size();
    idle.SendNack(1, idx);
    idle.SendInvalidateRef(1);
    Check(w.toHost.size() == before, "SendNack/SendInvalidateRef ignored before STREAMING");
}

// RECONFIG (host->client giữa phiên), SET_FOCUS và FEEDBACK (client->host) đi đúng
// đường: cập nhật tham số / gọi callback, và bị bỏ khi kích thước suy biến.
void TestReconfigFocusFeedback() {
    std::printf("[session] RECONFIG / SET_FOCUS / FEEDBACK routing...\n");
    WirePair w;
    uint64_t now = 10'000'000;

    bool focus = false, gotFocusFalse = false;
    Feedback lastFb{};
    bool gotFb = false;
    HostCallbacks hcb;
    hcb.send = [&](std::span<const uint8_t> d) { w.toClient.emplace_back(d.begin(), d.end()); };
    hcb.onFocus = [&](bool on) { focus = on; if (!on) gotFocusFalse = true; };
    hcb.onFeedback = [&](const Feedback& fb) { lastFb = fb; gotFb = true; };
    HostSession host(hcb, StreamParams{1920, 1080, 60, 20'000'000});

    bool reconfigured = false;
    NegotiatedParams rp{};
    ClientCallbacks ccb;
    ccb.send = [&](std::span<const uint8_t> d) { w.toHost.emplace_back(d.begin(), d.end()); };
    ccb.onReconfig = [&](const NegotiatedParams& p) { reconfigured = true; rp = p; };
    ClientSession cli(ccb);

    auto pump = [&] {
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

    cli.Start(Hello{0x1, kCodecMaskH264, 1920, 1080, 60, 0}, now);
    pump();
    cli.NotifyVideoPacket(now);

    // Host gửi RECONFIG -> client cập nhật params + gọi onReconfig.
    uint8_t buf[kMaxDatagram];
    size_t n = BuildReconfig(buf, cli.sessionId(), Reconfig{1280, 720, 8'000'000});
    cli.HandlePacket(std::span<const uint8_t>(buf, n), now);
    Check(reconfigured && rp.width == 1280 && rp.height == 720 && rp.bitrateBps == 8'000'000,
        "RECONFIG updates params and fires onReconfig");
    // Kích thước 0 = gói hỏng -> giữ nguyên, không dựng decoder 0x0.
    reconfigured = false;
    n = BuildReconfig(buf, cli.sessionId(), Reconfig{0, 0, 0});
    cli.HandlePacket(std::span<const uint8_t>(buf, n), now);
    Check(reconfigured && rp.width == 1280 && rp.height == 720,
        "RECONFIG with zero size keeps the previous dimensions");

    // Client -> host: SET_FOCUS (true rồi false) và FEEDBACK.
    cli.SetFocused(true);
    cli.Tick(now);
    pump();
    Check(focus, "SET_FOCUS(true) reaches host onFocus");
    cli.SetFocused(false);
    now += 100'000;
    cli.Tick(now);
    pump();
    Check(gotFocusFalse, "SET_FOCUS(false) reaches host onFocus");

    cli.SendFeedback(Feedback{2, 7, 25, 9000});
    pump();
    Check(gotFb && lastFb.lossPct == 7 && lastFb.rttMs == 25, "FEEDBACK reaches host onFeedback");
}

// Đếm số datagram mang loại thông điệp `t` trong một hàng đợi dây.
size_t CountType(const std::deque<Datagram>& q, MsgType t) {
    size_t n = 0;
    for (const auto& d : q) {
        const auto h = ParseCommonHeader(d);
        if (h && h->type == t) ++n;
    }
    return n;
}

InputEvent SessionKey(int32_t vk, bool down) {
    InputEvent e;
    e.type = InputType::Key;
    e.a = vk;
    e.b = 0x1E;
    e.state = down ? 1 : 0;
    return e;
}

// Bộ khung dùng chung cho các test bên dưới: một cặp host<->client đã nối dây
// kèm bộ đếm callback — các test đầu file (viết trước) tự dựng tay từng bước.
struct Rig {
    WirePair w;
    uint64_t now = 10'000'000;
    int startCalls = 0, readyCalls = 0, nackCalls = 0;
    bool hostDisconnected = false;
    uint32_t lastRtt = 0;
    std::vector<InputEvent> hostInput;
    std::string cliDead;
    HostSession host;
    ClientSession cli;

    Rig() : host(HostCb(), StreamParams{1920, 1080, 60, 20'000'000}), cli(CliCb()) {}

    std::string hostClip, cliClip;
    int hostClipCalls = 0, cliClipCalls = 0;

    HostCallbacks HostCb() {
        HostCallbacks cb;
        cb.send = [this](std::span<const uint8_t> d) { w.toClient.emplace_back(d.begin(), d.end()); };
        cb.onStart = [this] { ++startCalls; };
        cb.onDisconnect = [this] { hostDisconnected = true; };
        cb.onInput = [this](const InputEvent& e) { hostInput.push_back(e); };
        cb.onNack = [this](uint32_t, std::span<const uint16_t>) { ++nackCalls; };
        cb.onClipboard = [this](std::string t) { hostClip = std::move(t); ++hostClipCalls; };
        return cb;
    }
    ClientCallbacks CliCb() {
        ClientCallbacks cb;
        cb.send = [this](std::span<const uint8_t> d) { w.toHost.emplace_back(d.begin(), d.end()); };
        cb.onReady = [this](const NegotiatedParams&) { ++readyCalls; };
        cb.onRtt = [this](uint32_t r) { lastRtt = r; };
        cb.onDisconnect = [this](const char* r) { cliDead = r; };
        cb.onClipboard = [this](std::string t) { cliClip = std::move(t); ++cliClipCalls; };
        return cb;
    }
    void Pump() {
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
    }
    // Đưa cả hai bên tới STREAMING (kèm "gói video đầu tiên" qua NotifyVideoPacket).
    void Handshake(uint32_t clientId = 0x1) {
        cli.Start(Hello{clientId, kCodecMaskH264, 1920, 1080, 60, 0}, now);
        Pump();
        cli.NotifyVideoPacket(now);
    }
};

// Các gói TRÙNG của handshake phải vô hại: client retry HELLO/START mỗi 0.5s nên
// HELLO_ACK lặp, HELLO phát lại và START lặp là chuyện thường ngày trên UDP.
void TestHandshakeDuplicates() {
    std::printf("[session] duplicate HELLO_ACK / re-HELLO / repeated START are idempotent...\n");
    Rig r;
    r.Handshake();
    Check(r.startCalls == 1 && r.readyCalls == 1, "handshake reached STREAMING once");

    // HELLO_ACK lặp mang số khác: hợp lệ (nuôi timeout) nhưng không được đụng vào
    // trạng thái — dựng lại decoder giữa phiên là hỏng hình.
    uint8_t buf[kMaxDatagram];
    HelloAck dup{};
    dup.sessionId = 0xDEAD;
    dup.codec = Codec::H264;
    dup.width = 640;
    dup.height = 480;
    dup.fps = 30;
    size_t n = BuildHelloAck(buf, dup);
    Check(r.cli.HandlePacket(std::span<const uint8_t>(buf, n), r.now),
        "duplicate HELLO_ACK accepted as valid traffic");
    Check(r.cli.sessionId() != 0xDEAD && r.cli.params().width == 1920 && r.readyCalls == 1,
        "duplicate HELLO_ACK doesn't rebuild the session");

    // HELLO phát lại từ CÙNG client (ACK bị mất): host ACK lại, giữ nguyên phiên.
    const uint32_t sid = r.host.sessionId();
    r.w.toClient.clear();
    n = BuildHello(buf, Hello{0x1, kCodecMaskH264, 1920, 1080, 60, 0});
    Check(r.host.HandlePacket(std::span<const uint8_t>(buf, n), r.now),
        "re-HELLO from the same client accepted");
    Check(r.host.sessionId() == sid && r.host.state() == HostSession::State::Streaming,
        "re-HELLO keeps the existing session");
    Check(CountType(r.w.toClient, MsgType::HelloAck) == 1, "re-HELLO answered with another ACK");
    const auto ack = ParseHelloAck(PayloadOf(r.w.toClient.front()));
    Check(ack && ack->sessionId == sid, "re-sent ACK carries the same sessionId");

    // START lặp: chỉ lần đầu đổi trạng thái và force IDR.
    n = BuildStart(buf, sid);
    r.host.HandlePacket(std::span<const uint8_t>(buf, n), r.now);
    Check(r.startCalls == 1, "repeated START doesn't re-fire onStart");
}

// Hai đường chết còn thiếu của client: host chủ động BYE, và host im lặng giữa phiên.
void TestClientDeathPaths() {
    std::printf("[session] BYE from host + mid-session timeout kill the client...\n");
    {
        Rig r;
        r.Handshake();
        uint8_t buf[kMaxDatagram];
        const size_t n = BuildBye(buf, r.cli.sessionId());
        r.cli.HandlePacket(std::span<const uint8_t>(buf, n), r.now);
        Check(r.cli.state() == ClientSession::State::Dead && !r.cliDead.empty(),
            "BYE from host kills the client session");
    }
    {
        Rig r;
        r.Handshake();
        r.now += kSessionTimeoutUs + 1'000'000;
        r.cli.Tick(r.now);
        Check(r.cli.state() == ClientSession::State::Dead && !r.cliDead.empty(),
            "silent host -> client dies on session timeout");
    }
}

// Client không giải mã được H.264: host v1 chỉ phát H.264 nên phải từ chối ngay ở
// bắt tay, thay vì để client ngồi nhìn màn hình đen.
void TestRejectCodecMismatch() {
    std::printf("[session] HELLO without H.264 -> rejected at handshake...\n");
    Rig r;
    r.cli.Start(Hello{0x2, kCodecMaskHevc, 1920, 1080, 60, 0}, r.now);
    r.Pump();
    Check(!r.cliDead.empty(), "client without H.264 refused at handshake");
    Check(r.host.state() == HostSession::State::Idle, "host stays IDLE after the codec reject");
}

// Input đi TRỌN qua session: QueueInput -> Tick flush -> host onInput. Trước khi
// STREAMING là no-op, bản phát lại (redundancy) phải bị host khử trùng.
void TestInputThroughSession() {
    std::printf("[session] input flows client -> host, deduped, gated on STREAMING...\n");
    Rig r;
    r.cli.QueueInput(SessionKey('A', true)); // chưa có phiên -> rơi vào hư không
    r.Handshake();
    r.cli.Tick(r.now);
    r.Pump();
    Check(r.hostInput.empty(), "input queued before STREAMING is dropped");

    r.cli.QueueInput(SessionKey('B', true));
    r.cli.QueueInput(SessionKey('B', false));
    r.now += 20'000;
    r.cli.Tick(r.now);
    // Tick thêm vài nhịp cho InputSender phát lại đuôi — bản lặp phải bị khử.
    for (int i = 0; i < 3; ++i) {
        r.now += kInputRepeatIntervalUs;
        r.cli.Tick(r.now);
    }
    r.Pump();
    Check(r.hostInput.size() == 2 && r.hostInput[0].a == 'B' && r.hostInput[0].state == 1 &&
              r.hostInput[1].a == 'B' && r.hostInput[1].state == 0,
        "input events reach host exactly once, in order");
}

// sessionId là hàng rào DUY NHẤT chặn gói lạc trên UDP — quét qua các loại thông
// điệp chính, cả hai chiều: gói mang sessionId lạ bị bỏ và không đụng trạng thái.
void TestStraySessionIdIgnored() {
    std::printf("[session] packets with a stray sessionId are ignored on both sides...\n");
    Rig r;
    r.Handshake();
    uint8_t buf[kMaxDatagram];
    const uint32_t bad = r.cli.sessionId() ^ 0x55AA55AA;

    size_t n = BuildPong(buf, bad, PingPong{9, 1});
    Check(!r.cli.HandlePacket(std::span<const uint8_t>(buf, n), r.now) && r.lastRtt == 0,
        "stray PONG rejected, RTT untouched");
    n = BuildReconfig(buf, bad, Reconfig{320, 200, 1'000'000});
    Check(!r.cli.HandlePacket(std::span<const uint8_t>(buf, n), r.now) &&
              r.cli.params().width == 1920,
        "stray RECONFIG ignored, params untouched");
    n = BuildBye(buf, bad);
    Check(!r.cli.HandlePacket(std::span<const uint8_t>(buf, n), r.now) &&
              r.cli.state() == ClientSession::State::Streaming,
        "stray BYE doesn't kill the session");

    r.w.toClient.clear();
    n = BuildPing(buf, bad, PingPong{1, 1});
    Check(!r.host.HandlePacket(std::span<const uint8_t>(buf, n), r.now) && r.w.toClient.empty(),
        "stray PING rejected, no PONG sent");
    const uint16_t idx[] = {1};
    n = BuildNack(buf, bad, 7, idx);
    Check(!r.host.HandlePacket(std::span<const uint8_t>(buf, n), r.now) && r.nackCalls == 0,
        "stray NACK rejected, onNack not called");
    const InputEvent ev = SessionKey('Z', true);
    n = BuildInputEvents(buf, bad, 0, std::span<const InputEvent>(&ev, 1));
    Check(!r.host.HandlePacket(std::span<const uint8_t>(buf, n), r.now) && r.hostInput.empty(),
        "stray INPUT_EVENT rejected, nothing injected");
}

// SET_FOCUS chỉ được phát đúng kFocusRepeats lần rồi im (phát mãi thì người ngồi
// máy host không dùng nổi ứng dụng khác), và REQUEST_KEYFRAME dừng được bằng Cancel.
void TestFocusRepeatsAndKeyframeCancel() {
    std::printf("[session] SET_FOCUS repeat quota + CancelKeyframeRequest...\n");
    Rig r;
    r.Handshake();
    r.w.toHost.clear();

    r.cli.SetFocused(true);
    for (int i = 0; i < 10; ++i) {
        r.now += kFocusRetryUs;
        r.cli.Tick(r.now);
    }
    Check(CountType(r.w.toHost, MsgType::SetFocus) == size_t(kFocusRepeats),
        "SET_FOCUS sent exactly kFocusRepeats times");

    // Gọi lại cùng giá trị khi host đã biết: không phát thêm gói nào.
    r.w.toHost.clear();
    r.cli.SetFocused(true);
    r.now += kFocusRetryUs;
    r.cli.Tick(r.now);
    Check(CountType(r.w.toHost, MsgType::SetFocus) == 0, "SetFocused(same value) doesn't resend");

    r.w.toHost.clear();
    r.cli.RequestKeyframe();
    r.cli.Tick(r.now);
    r.now += kKeyframeRetryUs;
    r.cli.Tick(r.now);
    Check(CountType(r.w.toHost, MsgType::RequestKeyframe) == 2,
        "REQUEST_KEYFRAME repeats while wanted");
    r.cli.CancelKeyframeRequest();
    r.w.toHost.clear();
    r.now += kKeyframeRetryUs;
    r.cli.Tick(r.now);
    Check(CountType(r.w.toHost, MsgType::RequestKeyframe) == 0,
        "CancelKeyframeRequest stops the retries");
}

// ClipboardAssembler trần: ghép lạc thứ tự, khử trùng, thay bản dở dang, chặn khai điêu.
void TestClipboardAssembler() {
    std::printf("[session] ClipboardAssembler: reorder, dup, replace, oversize...\n");
    auto chunk = [](uint32_t id, uint16_t idx, uint16_t cnt, const std::string& s) {
        ClipboardChunkView c;
        c.updateId = id;
        c.chunkIndex = idx;
        c.chunkCount = cnt;
        c.data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return c;
    };

    ClipboardAssembler as;
    const std::string a = "AA", b = "BB", c3 = "CC";
    // 3 mảnh về lạc thứ tự, kèm một mảnh trùng giữa chừng.
    Check(!as.Push(chunk(1, 2, 3, c3)).has_value(), "chunk 2/3 alone -> not done");
    Check(!as.Push(chunk(1, 0, 3, a)).has_value(), "chunk 0/3 -> not done");
    Check(!as.Push(chunk(1, 0, 3, a)).has_value(), "duplicate chunk ignored");
    auto done = as.Push(chunk(1, 1, 3, b));
    Check(done && *done == "AABBCC", "chunks reassembled in index order");
    Check(!as.Push(chunk(1, 1, 3, b)).has_value(), "late chunk of a done update ignored");

    // Bản dở dang bị thay khi update mới tới; update mới vẫn ghép trọn.
    Check(!as.Push(chunk(2, 0, 2, a)).has_value(), "start of update 2");
    Check(!as.Push(chunk(3, 0, 2, b)).has_value(), "update 3 replaces unfinished 2");
    Check(!as.Push(chunk(2, 1, 2, a)).has_value(), "stale chunk of dropped update ignored");
    done = as.Push(chunk(3, 1, 2, c3));
    Check(done && *done == "BBCC", "replacement update completes");

    // chunkCount lệch giữa các mảnh cùng update = gói hỏng.
    Check(!as.Push(chunk(4, 0, 2, a)).has_value(), "start of update 4");
    Check(!as.Push(chunk(4, 1, 3, b)).has_value(), "mismatched chunkCount rejected");

    // Khai điêu vượt trần: huỷ cả update, không cấp phát vô hạn.
    {
        ClipboardAssembler big;
        const std::string huge(kMaxClipboardChunk, 'x');
        const uint16_t cnt = uint16_t(kMaxClipboardBytes / kMaxClipboardChunk + 2);
        bool anyDone = false;
        for (uint16_t i = 0; i < cnt; ++i)
            anyDone = anyDone || big.Push(chunk(9, i, cnt, huge)).has_value();
        Check(!anyDone, "update over kMaxClipboardBytes discarded");
    }
}

// CLIPBOARD đi trọn qua hai session, cả hai chiều, văn bản nhiều mảnh; chặn khi
// chưa STREAMING và khi sessionId lạ.
void TestClipboardThroughSession() {
    std::printf("[session] clipboard flows both ways, multi-chunk, gated...\n");
    Rig r;

    // Trước khi STREAMING: không gửi gì lên dây.
    r.cli.SendClipboard("early");
    Check(r.w.toHost.empty(), "SendClipboard before STREAMING is a no-op");

    r.Handshake();
    // Văn bản 3 mảnh (vượt 2 datagram) + ký tự nhiều byte để soi ranh giới cắt.
    std::string text = "vi\xE1\xBB\x87t ";
    while (text.size() < 2 * kMaxClipboardChunk + 100) text += "0123456789";
    r.cli.SendClipboard(text);
    Check(CountType(r.w.toHost, MsgType::Clipboard) == 3, "client sent 3 chunks");
    r.Pump();
    Check(r.hostClipCalls == 1 && r.hostClip == text, "host received the exact text");

    // Chiều ngược lại: host -> client.
    r.host.SendClipboard("from host");
    r.Pump();
    Check(r.cliClipCalls == 1 && r.cliClip == "from host", "client received host text");

    // Mảnh mang sessionId lạ bị bỏ, không đụng bộ ghép.
    uint8_t buf[kMaxDatagram];
    const uint8_t d[] = {'x'};
    const size_t n = BuildClipboardChunk(buf, r.cli.sessionId() ^ 0x5A5A, 99, 0, 1,
        std::span<const uint8_t>(d, 1));
    Check(!r.host.HandlePacket(std::span<const uint8_t>(buf, n), r.now) &&
              r.hostClipCalls == 1,
        "stray-session clipboard chunk rejected");
}

// Datagram rác thuần ngẫu nhiên (PRNG xác định, tái lập được): cả hai bên phải
// đứng vững và giữ nguyên phiên — UDP là cổng mở, ai cũng gửi tới được.
void TestSessionsSurviveGarbage() {
    std::printf("[session] 500 garbage datagrams -> both sides unaffected...\n");
    Rig r;
    r.Handshake();
    const uint32_t sid = r.host.sessionId();
    for (int i = 0; i < 500; ++i) {
        Datagram d(Rnd() % 1300, 0);
        for (auto& b : d) b = uint8_t(Rnd());
        r.host.HandlePacket(d, r.now);
        r.cli.HandlePacket(d, r.now);
    }
    Check(r.host.state() == HostSession::State::Streaming && r.host.sessionId() == sid,
        "host unaffected by garbage datagrams");
    Check(r.cli.state() == ClientSession::State::Streaming && r.cliDead.empty(),
        "client unaffected by garbage datagrams");
}

} // namespace

void RunSessionTests() {
    TestSessions();
    TestSessionsNackInvalidate();
    TestReconfigFocusFeedback();
    TestHandshakeDuplicates();
    TestClientDeathPaths();
    TestRejectCodecMismatch();
    TestInputThroughSession();
    TestStraySessionIdIgnored();
    TestFocusRepeatsAndKeyframeCancel();
    TestClipboardAssembler();
    TestClipboardThroughSession();
    TestSessionsSurviveGarbage();
}
