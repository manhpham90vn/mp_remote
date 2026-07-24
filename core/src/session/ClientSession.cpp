// =============================================================================
// ClientSession.cpp — cài đặt bắt tay, giữ nhịp và lịch phát của kênh Control.
//
// HAI ĐƯỜNG VÀO
//   HandlePacket() — phản ứng với thông điệp từ host (HELLO_ACK, PONG, RECONFIG, BYE).
//   Tick()         — mọi thứ do THỜI GIAN thúc đẩy: phát lại HELLO/START, PING định
//                    kỳ, kiểm tra timeout, và đẩy các hàng đợi đang chờ đi.
//
// THỨ TỰ PHÁT TRONG Tick() LÀ CÓ CHỦ Ý
//   Mỗi vòng Tick có thể phát nhiều gói, và thứ tự chúng rời máy quyết định cảm
//   giác của người dùng khi mạng chật:
//
//     1. PING            — phải đều đặn, nếu không host tưởng ta đã chết.
//     2. SET_FOCUS       — trước input, vì host chỉ bơm phím vào cửa sổ đang
//                          foreground; gửi sau thì những phím đầu tiên sau khi đổi
//                          cửa sổ rơi vào khoảng trống.
//     3. INPUT           — thao tác của người dùng nhạy cảm với trễ nhất.
//     4. REQUEST_KEYFRAME— quan trọng nhưng chịu được trễ vài chục mili-giây, và
//                          nó sẽ kéo về một IDR nặng nên không nên vội.
//
// CƠ CHẾ PHÁT LẶP DÙNG CHUNG MỘT KHUÔN
//   Mọi thứ cần phát lại đều theo mẫu "mốc thời gian lần cuối + khoảng cách tối
//   thiểu": lastSentUs_/kHelloRetryUs, lastPingUs_/kPingIntervalUs,
//   lastKeyframeReqUs_/kKeyframeRetryUs, lastFocusUs_/kFocusRetryUs. Riêng
//   SET_FOCUS có thêm quota focusRepeatsLeft_ vì nó KHÔNG được phát mãi — xem lý
//   do ở ClientSession.h chỗ khai báo kFocusRepeats.
//
// LIÊN QUAN: deskhub/session/ClientSession.h (máy trạng thái), HostSession.cpp (đầu kia)
// =============================================================================
#include "deskhub/session/ClientSession.h"

#include "deskhub/session/HostSession.h" // kSessionTimeoutUs dùng chung hai phía

namespace deskhub {

void ClientSession::Start(const Hello& hello, uint64_t nowUs) {
    hello_ = hello;
    state_ = State::Hello;
    startedUs_ = nowUs;
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
            // Ta phát HELLO lại mỗi 0.5 giây nên host cũng ACK lại từng ấy lần. Chỉ cái
            // đầu tiên có tác dụng; những cái sau trả true (gói hợp lệ, nuôi timeout)
            // nhưng không đụng vào trạng thái — dựng lại decoder giữa phiên là hỏng hình.
            if (state_ != State::Hello) return true;
            if (m->codec == Codec::Rejected) {
                Die("host rejected (busy or codec mismatch)");
                return false;
            }
            sessionId_ = m->sessionId;
            params_.codec = m->codec;
            params_.width = m->width;
            params_.height = m->height;
            params_.fps = m->fps;
            params_.bitrateBps = m->bitrateBps;
            params_.timebaseUs = m->timebaseUs;
            state_ = State::Starting;
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
            // sendTimeUs là đồng hồ CLIENT do chính ta đặt vào PING và host dội lại
            // nguyên văn — nên hiệu này là RTT thật, và hai đồng hồ không cần đồng bộ.
            lastRttUs_ = uint32_t(nowUs - m->sendTimeUs);
            if (cb_.onRtt) cb_.onRtt(lastRttUs_);
            return true;
        }
        case MsgType::Reconfig: {
            if (h->sessionId != sessionId_ || sessionId_ == 0) return false;
            if (state_ != State::Starting && state_ != State::Streaming) return false;
            const auto m = ParseReconfig(payload);
            if (!m) return false;
            lastRecvUs_ = nowUs;
            // Kích thước 0 = host gửi hỏng; giữ nguyên còn hơn dựng decoder 0x0.
            if (m->width && m->height) {
                params_.width = m->width;
                params_.height = m->height;
            }
            if (m->bitrateBps) params_.bitrateBps = m->bitrateBps;
            if (cb_.onReconfig) cb_.onReconfig(params_);
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

// Caller gọi khi nhận gói Video mang đúng sessionId. Hai tác dụng:
// nuôi timeout, và — quan trọng hơn — gói video đầu tiên chính là BẰNG CHỨNG host
// đã nhận được START. START không có ACK riêng, nên đây là tín hiệu duy nhất cho
// biết khi nào ngừng phát lại nó.
void ClientSession::NotifyVideoPacket(uint64_t nowUs) {
    if (state_ == State::Starting) state_ = State::Streaming;
    if (state_ == State::Streaming) lastRecvUs_ = nowUs;
}

void ClientSession::QueueInput(const InputEvent& e) {
    if (state_ != State::Streaming) return; // chưa có phiên → không có chỗ gửi
    input_.SetSessionId(sessionId_);
    input_.Queue(e);
}

void ClientSession::SetFocused(bool on) {
    if (focusWanted_ == on && focusSent_ == on) return; // host đã biết rồi
    focusWanted_ = on;
    focusRepeatsLeft_ = kFocusRepeats;
    lastFocusUs_ = 0; // phát ngay ở Tick kế tiếp, không đợi hết chu kỳ
}

// Gọi mỗi vòng lặp của client. Xem ghi chú đầu file về thứ tự phát các gói.
void ClientSession::Tick(uint64_t nowUs) {
    // Phần đầu: việc riêng của từng trạng thái. Starting cố ý dùng `break` chứ
    // không `return` — nó vẫn cần ping và kiểm tra timeout y như Streaming.
    switch (state_) {
        case State::Idle:
        case State::Dead:
            return;
        case State::Hello:
            if (nowUs - startedUs_ > kHelloGiveUpUs) {
                Die("could not connect (timed out)");
                return;
            }
            if (nowUs - lastSentUs_ >= kHelloRetryUs) {
                lastSentUs_ = nowUs;
                SendHello();
            }
            return;
        case State::Starting:
            if (nowUs - lastSentUs_ >= kHelloRetryUs) {
                lastSentUs_ = nowUs;
                SendStart();
            }
            break; // vẫn ping/timeout như Streaming
        case State::Streaming:
            break;
    }

    if (nowUs - lastRecvUs_ > kSessionTimeoutUs) {
        Die("lost contact with host (timeout)");
        return;
    }

    if (nowUs - lastPingUs_ >= kPingIntervalUs) {
        lastPingUs_ = nowUs;
        PingPong p{nextPingId_++, nowUs};
        const size_t n = BuildPing(buf_, sessionId_, p);
        if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
    }

    // SET_FOCUS đi TRƯỚC input: host chỉ bơm khi cửa sổ nguồn đang foreground, gửi
    // sau thì những phím đầu tiên sau khi đổi cửa sổ rơi vào khoảng trống.
    if (state_ == State::Streaming && focusRepeatsLeft_ > 0 &&
        nowUs - lastFocusUs_ >= kFocusRetryUs) {
        lastFocusUs_ = nowUs;
        --focusRepeatsLeft_;
        focusSent_ = focusWanted_;
        const size_t n = BuildSetFocus(buf_, sessionId_, focusWanted_);
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

void ClientSession::SendFeedback(const Feedback& fb) {
    if (state_ != State::Streaming) return;
    const size_t n = BuildFeedback(buf_, sessionId_, fb);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

void ClientSession::SendNack(uint32_t frameId, std::span<const uint16_t> indices) {
    if (state_ != State::Streaming || indices.empty()) return;
    const size_t n = BuildNack(buf_, sessionId_, frameId, indices);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

void ClientSession::SendInvalidateRef(uint32_t frameId) {
    if (state_ != State::Streaming) return;
    const size_t n = BuildInvalidateRef(buf_, sessionId_, frameId);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
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

} // namespace deskhub
