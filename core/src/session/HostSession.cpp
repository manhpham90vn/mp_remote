// =============================================================================
// HostSession.cpp — cài đặt máy trạng thái và bộ định tuyến thông điệp phía host.
//
// HandlePacket là một switch lớn trên loại thông điệp, và mọi nhánh (trừ HELLO)
// đều mở đầu bằng CÙNG MỘT ĐÔI KIỂM TRA:
//
//     if (state() == ... || h->sessionId != sessionId()) return false;
//     lastRecvUs_ = nowUs;
//
//   - Kiểm tra trạng thái: thông điệp đến sai giai đoạn thì bỏ. Ví dụ INPUT_EVENT
//     lúc chưa STREAMING là vô nghĩa vì host còn chưa biết client là ai.
//   - Kiểm tra sessionId: chặn gói lạc từ phiên cũ hoặc từ máy khác trên mạng.
//     UDP không có kết nối, ai cũng gửi tới cổng này được, nên đây là hàng rào duy
//     nhất phân biệt "client của tôi" với phần còn lại của Internet.
//   - lastRecvUs_ = nowUs: mọi gói hợp lệ đều NUÔI TIMEOUT. Chỉ cần client còn nói
//     chuyện, bất kể nói gì, phiên vẫn sống.
//
// GIÁ TRỊ TRẢ VỀ CÓ Ý NGHĨA RIÊNG
//   true không chỉ là "gói hợp lệ" — AgentLoop dùng nó làm tín hiệu cập nhật địa
//   chỉ peer theo địa chỉ nguồn của gói (client đổi IP khi chuyển Wi-Fi/4G vẫn giữ
//   được phiên). Vì thế BYE trả về false dù nó là gói hoàn toàn hợp lệ: phiên vừa
//   đóng, cập nhật peer theo nó là vô nghĩa.
//
// LIÊN QUAN: deskhub/session/HostSession.h (máy trạng thái + lý do thiết kế),
//            ClientSession.cpp (đầu kia)
// =============================================================================
#include "deskhub/session/HostSession.h"

namespace deskhub {

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
                // v1 chỉ phát H.264. Client không giải mã được thì từ chối ngay ở bước
                // bắt tay, thay vì để nó ngồi nhìn màn hình đen không hiểu vì sao.
                if (!(m->codecMask & kCodecMaskH264)) {
                    SendReject();
                    return false;
                }
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
            // Client phát lại START tới khi thấy video nên gói này hay đến nhiều lần;
            // chỉ lần đầu mới đổi trạng thái và gọi onStart (nó ép encoder ra IDR —
            // gọi lặp sẽ làm luồng hình giật vì IDR nặng gấp nhiều lần P-frame).
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
        case MsgType::InputEvent:
            // Chỉ nhận input khi đang STREAMING: trước đó host chưa biết client là ai.
            if (state() != State::Streaming || h->sessionId != sessionId()) return false;
            lastRecvUs_ = nowUs;
            input_.HandlePacket(payload, cb_.onInput);
            return true;
        case MsgType::SetFocus: {
            if (state() != State::Streaming || h->sessionId != sessionId()) return false;
            lastRecvUs_ = nowUs;
            const auto focused = ParseSetFocus(payload);
            if (focused && cb_.onFocus) cb_.onFocus(*focused);
            return true;
        }
        case MsgType::Feedback: {
            if (state() == State::Idle || h->sessionId != sessionId()) return false;
            lastRecvUs_ = nowUs;
            const auto m = ParseFeedback(payload);
            if (m && cb_.onFeedback) cb_.onFeedback(*m);
            return true; // gói vẫn nuôi timeout kể cả khi payload hỏng
        }
        case MsgType::Nack: {
            if (state() != State::Streaming || h->sessionId != sessionId()) return false;
            lastRecvUs_ = nowUs;
            uint16_t idx[kMaxNackIndices];
            uint32_t frameId = 0;
            const size_t n = ParseNack(payload, frameId, idx);
            if (n && cb_.onNack) cb_.onNack(frameId, std::span<const uint16_t>(idx, n));
            return true;
        }
        case MsgType::InvalidateRef: {
            if (state() != State::Streaming || h->sessionId != sessionId()) return false;
            lastRecvUs_ = nowUs;
            const auto fid = ParseInvalidateRef(payload);
            if (fid && cb_.onInvalidateRef) cb_.onInvalidateRef(*fid);
            return true;
        }
        case MsgType::Clipboard: {
            if (state() != State::Streaming || h->sessionId != sessionId()) return false;
            const auto c = ParseClipboardChunk(payload);
            if (!c) return false;
            lastRecvUs_ = nowUs;
            // Push trả văn bản đúng một lần khi đủ mảnh — mảnh trùng/lẻ là nullopt.
            if (auto text = clip_.Push(*c); text && cb_.onClipboard)
                cb_.onClipboard(std::move(*text));
            return true;
        }
        case MsgType::Bye:
            if (state() == State::Idle || h->sessionId != sessionId()) return false;
            Disconnect();
            return false; // phiên đã đóng — đừng cập nhật peer theo gói này
        default:
            return false;
    }
}

// Gọi đều đặn từ vòng lặp Recv. Việc duy nhất: phát hiện client biến mất không kịp
// nói lời chào. UDP không báo đứt kết nối, nên im lặng quá kSessionTimeoutUs là dấu
// hiệu duy nhất ta có — có thể client rút mạng, tắt máy, hoặc sập nguồn.
void HostSession::Tick(uint64_t nowUs) {
    if (state() == State::Idle) return;
    if (nowUs - lastRecvUs_ > kSessionTimeoutUs) Disconnect();
}

// Chia văn bản thành mảnh ≤ kMaxClipboardChunk rồi phát một loạt — cùng chính sách
// best-effort với ClientSession::SendClipboard (xem ghi chú ở đó).
void HostSession::SendClipboard(std::string_view utf8) {
    if (state() != State::Streaming || !cb_.send) return;
    if (utf8.empty() || utf8.size() > kMaxClipboardBytes) return;
    const uint32_t id = ++clipUpdateId_;
    const size_t count = (utf8.size() + kMaxClipboardChunk - 1) / kMaxClipboardChunk;
    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * kMaxClipboardChunk;
        const size_t len = utf8.size() - off < kMaxClipboardChunk ? utf8.size() - off
                                                                  : kMaxClipboardChunk;
        const auto* d = reinterpret_cast<const uint8_t*>(utf8.data()) + off;
        const size_t n = BuildClipboardChunk(buf_, sessionId(), id, uint16_t(i),
            uint16_t(count), std::span<const uint8_t>(d, len));
        if (n) cb_.send(std::span<const uint8_t>(buf_, n));
    }
}

void HostSession::SendHelloAck(uint64_t nowUs) {
    HelloAck a;
    a.sessionId = sessionId();
    a.codec = Codec::H264;
    a.width = offer_.width;
    a.height = offer_.height;
    a.fps = offer_.fps;
    a.bitrateBps = offer_.bitrateBps;
    a.timebaseUs = nowUs; // mốc đồng hồ host để client ước lượng trễ e2e (§7)
    const size_t n = BuildHelloAck(buf_, a);
    if (n && cb_.send) cb_.send(std::span<const uint8_t>(buf_, n));
}

// Từ chối = HELLO_ACK với codec = Rejected, mọi trường khác để 0. Dùng lại chính
// thông điệp ACK thay vì thêm một loại "REJECT" riêng: client vốn đã phải chờ ACK,
// nên nó nhận được câu trả lời dứt khoát ngay thay vì đợi hết 10 giây rồi bỏ cuộc.
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
    input_.Reset(); // client sau bắt đầu lại từ seq 0
    // Quên clipboard đang ghép dở: client sau đánh updateId lại từ đầu, giữ trạng
    // thái cũ thì mảnh đầu của nó có thể bị nhận nhầm là "bản đã áp dụng".
    clip_ = ClipboardAssembler{};
    if (cb_.onDisconnect) cb_.onDisconnect(); // caller nhả hết phím đang giữ
}

} // namespace deskhub
