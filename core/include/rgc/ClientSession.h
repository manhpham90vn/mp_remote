#pragma once
// ClientSession — máy trạng thái phía client: HELLO (retry) → nhận HELLO_ACK →
// onReady (client dựng decoder/renderer) → START (retry tới khi có video) →
// STREAMING (PING mỗi 1s, timeout 5s, REQUEST_KEYFRAME khi Reassembler báo loss).
//
// Thuần C++20 như HostSession: byte vào qua HandlePacket (chỉ gói Control —
// gói Video caller tự đưa vào Reassembler rồi gọi NotifyVideoPacket để nuôi
// timeout), byte ra qua callback `send`, thời gian bơm từ ngoài.
// Toàn bộ chạy trên MỘT thread (thread Recv của client) — không khóa.
#include "rgc/InputSender.h"
#include "rgc/Wire.h"

#include <cstdint>
#include <functional>
#include <span>

namespace rgc {

inline constexpr uint64_t kHelloRetryUs    = 500'000;    // phát lại HELLO/START
inline constexpr uint64_t kHelloGiveUpUs   = 10'000'000; // bỏ cuộc nếu host im lặng
inline constexpr uint64_t kPingIntervalUs  = 1'000'000;
inline constexpr uint64_t kKeyframeRetryUs = 250'000;
// SET_FOCUS gửi theo BIẾN CỐ (đổi cửa sổ), không gửi định kỳ: phát lại đều đặn thì
// người ngồi ở máy host không bao giờ bấm sang được ứng dụng khác của chính mình.
// Đổi lại phải chịu mất gói → phát kFocusRepeats lần cách kFocusRetryUs cho chắc.
inline constexpr uint64_t kFocusRetryUs = 50'000;
inline constexpr int      kFocusRepeats = 3;

struct NegotiatedParams {
    Codec    codec = Codec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t  fps = 60;
    uint32_t bitrateBps = 0;
    uint64_t timebaseUs = 0; // đồng hồ host tại thời điểm HELLO_ACK
};

struct ClientCallbacks {
    std::function<void(std::span<const uint8_t>)> send;
    std::function<void(const NegotiatedParams&)> onReady; // dựng decoder/renderer
    // RECONFIG: host đổi kích thước nguồn hoặc bitrate giữa phiên. params() đã cập
    // nhật khi callback chạy. Host gửi kèm IDR nên decoder tự đàm phán lại kích
    // thước qua MF_E_TRANSFORM_STREAM_CHANGE — caller chỉ cần cập nhật hiển thị.
    std::function<void(const NegotiatedParams&)> onReconfig;
    std::function<void(uint32_t rttUs)> onRtt;            // mỗi PONG
    std::function<void(const char* reason)> onDisconnect; // từ chối/BYE/timeout
};

class ClientSession {
public:
    enum class State : uint8_t { Idle, Hello, Starting, Streaming, Dead };

    explicit ClientSession(ClientCallbacks cb) : cb_(std::move(cb)) {}

    // Phát HELLO ngay và bắt đầu chu kỳ retry trong Tick.
    void Start(const Hello& hello, uint64_t nowUs);

    // Gói kênh Control từ host. Trả true nếu gói hợp lệ thuộc phiên.
    bool HandlePacket(std::span<const uint8_t> pkt, uint64_t nowUs);

    // Caller gọi khi nhận gói Video mang đúng sessionId: nuôi timeout,
    // Starting → Streaming (bằng chứng host đã nhận START).
    void NotifyVideoPacket(uint64_t nowUs);

    void Tick(uint64_t nowUs);

    // Xếp một event input để gửi (GĐ4). Chỉ có tác dụng khi đã STREAMING;
    // Tick lo việc đóng gói, đánh seq và gửi lặp chống kẹt phím.
    void QueueInput(const InputEvent& e);

    // Người dùng vừa chuyển sang (true) / rời khỏi (false) cửa sổ preview của nguồn
    // này → host đưa cửa sổ nguồn lên foreground, vì SendInput chỉ tới cửa sổ đang
    // foreground. Tick lo việc phát và phát lại; gọi lặp cùng giá trị là vô hại.
    void SetFocused(bool on);

    // Giữ cờ xin IDR: Tick phát REQUEST_KEYFRAME mỗi 250ms tới khi Cancel.
    void RequestKeyframe() { keyframeWanted_ = true; }
    void CancelKeyframeRequest() { keyframeWanted_ = false; }

    // Gửi FEEDBACK cho host (GĐ5). Caller gọi ~1s/lần từ khối thống kê của mình —
    // ClientSession không tự đo được mất gói (Reassembler nằm ngoài). Bỏ qua nếu
    // chưa STREAMING.
    void SendFeedback(const Feedback& fb);

    // Báo host mình rời đi (gửi 1 lần, best-effort) và kết thúc phiên.
    void SendBye();

    State    state() const { return state_; }
    uint32_t sessionId() const { return sessionId_; }
    uint32_t lastRttUs() const { return lastRttUs_; }
    const NegotiatedParams& params() const { return params_; }

private:
    void SendHello();
    void SendStart();
    void Die(const char* reason);

    ClientCallbacks cb_;
    InputSender input_;
    State    state_ = State::Idle;
    uint32_t sessionId_ = 0;
    Hello    hello_{};
    NegotiatedParams params_{};
    uint64_t startedUs_ = 0;      // lúc phát HELLO đầu — mốc bỏ cuộc
    uint64_t lastSentUs_ = 0;     // lần phát HELLO/START gần nhất
    uint64_t lastRecvUs_ = 0;
    uint64_t lastPingUs_ = 0;
    uint64_t lastKeyframeReqUs_ = 0;
    uint64_t lastFocusUs_ = 0;
    int      focusRepeatsLeft_ = 0;
    bool     focusWanted_ = false;  // giá trị đang phát lại
    bool     focusSent_ = false;    // giá trị host đã biết — khỏi phát lại thừa
    uint32_t nextPingId_ = 1;
    uint32_t lastRttUs_ = 0;
    bool     keyframeWanted_ = false;
    uint8_t  buf_[kMaxDatagram] = {};
};

} // namespace rgc
