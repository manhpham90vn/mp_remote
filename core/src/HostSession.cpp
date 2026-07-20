#include "rgc/HostSession.h"

namespace rgc {

bool HostSession::HandlePacket(std::span<const uint8_t> pkt, uint64_t nowUs) {
    const auto h = ParseCommonHeader(pkt);
    if (!h) return false;
    const auto payload = PayloadOf(pkt);

    switch (h->type) {
    case MsgType::Hello: {
        const auto m = ParseHello(payload);
        if (!m) return false;
        const State st = state();
        if (st != State::Idle && m->clientId != clientId_) {
            SendReject(); // v1: một client; đang bận → HELLO_ACK codec=0xFF
            return false;
        }
        if (st == State::Idle) {
            if (!(m->codecMask & kCodecMaskH264)) { SendReject(); return false; }
            clientId_ = m->clientId;
            // sessionId từ đồng hồ caller trộn clientId — đủ để phân biệt phiên, khác 0.
            uint32_t sid = uint32_t(nowUs ^ (nowUs >> 32)) ^ m->clientId;
            if (!sid) sid = 1;
            sessionId_.store(sid, std::memory_order_relaxed);
            state_.store(State::Ready, std::memory_order_release);
        }
        // READY/STREAMING cùng client = HELLO phát lại (mất ACK) → gửi lại ACK.
        lastRecvUs_ = nowUs;
        SendHelloAck(nowUs);
        return true;
    }
    case MsgType::Start:
        if (state() == State::Idle || h->sessionId != sessionId()) return false;
        lastRecvUs_ = nowUs;
        if (state() != State::Streaming) {
            state_.store(State::Streaming, std::memory_order_release);
            if (cb_.onStart) cb_.onStart();
        }
        return true;
    case MsgType::Ping: {
        if (state() == State::Idle || h->sessionId != sessionId()) return false;
        const auto m = ParsePingPong(payload);
        if (!m) return false;
        lastRecvUs_ = nowUs;
        const size_t n = BuildPong(buf_, sessionId(), *m);
        if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
        return true;
    }
    case MsgType::RequestKeyframe:
        if (state() != State::Streaming || h->sessionId != sessionId()) return false;
        lastRecvUs_ = nowUs;
        if (cb_.onKeyframeRequest) cb_.onKeyframeRequest();
        return true;
    case MsgType::Feedback: // GĐ5 mới xử lý — v1 chỉ nuôi timeout
        if (state() == State::Idle || h->sessionId != sessionId()) return false;
        lastRecvUs_ = nowUs;
        return true;
    case MsgType::Bye:
        if (state() == State::Idle || h->sessionId != sessionId()) return false;
        Disconnect();
        return false; // phiên đã đóng — đừng cập nhật peer theo gói này
    default:
        return false;
    }
}

void HostSession::Tick(uint64_t nowUs) {
    if (state() == State::Idle) return;
    if (nowUs - lastRecvUs_ > kSessionTimeoutUs) Disconnect();
}

void HostSession::SendHelloAck(uint64_t nowUs) {
    HelloAck a;
    a.sessionId  = sessionId();
    a.codec      = Codec::H264;
    a.width      = offer_.width;
    a.height     = offer_.height;
    a.fps        = offer_.fps;
    a.bitrateBps = offer_.bitrateBps;
    a.timebaseUs = nowUs; // mốc đồng hồ host để client ước lượng trễ e2e (§7)
    const size_t n = BuildHelloAck(buf_, a);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

void HostSession::SendReject() {
    HelloAck a{};
    a.codec = Codec::Rejected;
    const size_t n = BuildHelloAck(buf_, a);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

void HostSession::Disconnect() {
    state_.store(State::Idle, std::memory_order_release);
    sessionId_.store(0, std::memory_order_relaxed);
    clientId_ = 0;
    if (cb_.onDisconnect) cb_.onDisconnect();
}

} // namespace rgc
