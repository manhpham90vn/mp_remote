#include "rgc/ClientSession.h"

#include "rgc/HostSession.h" // kSessionTimeoutUs dùng chung hai phía

namespace rgc {

void ClientSession::Start(const Hello& hello, uint64_t nowUs) {
    hello_      = hello;
    state_      = State::Hello;
    startedUs_  = nowUs;
    lastRecvUs_ = nowUs;
    lastSentUs_ = nowUs;
    SendHello();
}

bool ClientSession::HandlePacket(std::span<const uint8_t> pkt, uint64_t nowUs) {
    const auto h = ParseCommonHeader(pkt);
    if (!h) return false;
    const auto payload = PayloadOf(pkt);

    switch (h->type) {
    case MsgType::HelloAck: {
        const auto m = ParseHelloAck(payload);
        if (!m) return false;
        if (state_ != State::Hello) return true; // ACK phát lại — đã xử lý rồi
        if (m->codec == Codec::Rejected) {
            Die("host rejected (busy or codec mismatch)");
            return false;
        }
        sessionId_ = m->sessionId;
        params_.codec      = m->codec;
        params_.width      = m->width;
        params_.height     = m->height;
        params_.fps        = m->fps;
        params_.bitrateBps = m->bitrateBps;
        params_.timebaseUs = m->timebaseUs;
        state_      = State::Starting;
        lastRecvUs_ = nowUs;
        lastSentUs_ = nowUs;
        if (cb_.onReady) cb_.onReady(params_);
        SendStart();
        return true;
    }
    case MsgType::Pong: {
        if (state_ == State::Idle || state_ == State::Dead) return false;
        if (h->sessionId != sessionId_) return false;
        const auto m = ParsePingPong(payload);
        if (!m) return false;
        lastRecvUs_ = nowUs;
        lastRttUs_  = uint32_t(nowUs - m->sendTimeUs); // sendTimeUs là đồng hồ client
        if (cb_.onRtt) cb_.onRtt(lastRttUs_);
        return true;
    }
    case MsgType::Bye:
        if (h->sessionId != sessionId_ || sessionId_ == 0) return false;
        Die("host ended the session (BYE)");
        return false;
    default:
        return false;
    }
}

void ClientSession::NotifyVideoPacket(uint64_t nowUs) {
    if (state_ == State::Starting) state_ = State::Streaming;
    if (state_ == State::Streaming) lastRecvUs_ = nowUs;
}

void ClientSession::QueueInput(const InputEvent& e) {
    if (state_ != State::Streaming) return; // chưa có phiên → không có chỗ gửi
    input_.SetSessionId(sessionId_);
    input_.Queue(e);
}

void ClientSession::Tick(uint64_t nowUs) {
    switch (state_) {
    case State::Idle:
    case State::Dead:
        return;
    case State::Hello:
        if (nowUs - startedUs_ > kHelloGiveUpUs) { Die("could not connect (timed out)"); return; }
        if (nowUs - lastSentUs_ >= kHelloRetryUs) { lastSentUs_ = nowUs; SendHello(); }
        return;
    case State::Starting:
        if (nowUs - lastSentUs_ >= kHelloRetryUs) { lastSentUs_ = nowUs; SendStart(); }
        break; // vẫn ping/timeout như Streaming
    case State::Streaming:
        break;
    }

    if (nowUs - lastRecvUs_ > kSessionTimeoutUs) { Die("lost contact with host (timeout)"); return; }

    if (nowUs - lastPingUs_ >= kPingIntervalUs) {
        lastPingUs_ = nowUs;
        PingPong p{nextPingId_++, nowUs};
        const size_t n = BuildPing(buf_, sessionId_, p);
        if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
    }

    // Input đi trước keyframe: thao tác của người dùng nhạy cảm với trễ nhất.
    if (state_ == State::Streaming && cb_.send) input_.Flush(nowUs, cb_.send);

    if (keyframeWanted_ && state_ == State::Streaming &&
        nowUs - lastKeyframeReqUs_ >= kKeyframeRetryUs) {
        lastKeyframeReqUs_ = nowUs;
        const size_t n = BuildRequestKeyframe(buf_, sessionId_);
        if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
    }
}

void ClientSession::SendBye() {
    if (state_ == State::Starting || state_ == State::Streaming) {
        const size_t n = BuildBye(buf_, sessionId_);
        if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
    }
    state_ = State::Dead;
}

void ClientSession::SendHello() {
    const size_t n = BuildHello(buf_, hello_);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

void ClientSession::SendStart() {
    const size_t n = BuildStart(buf_, sessionId_);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

void ClientSession::Die(const char* reason) {
    state_ = State::Dead;
    if (cb_.onDisconnect) cb_.onDisconnect(reason);
}

} // namespace rgc
