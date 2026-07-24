#pragma once
// =============================================================================
// ClientSession.h — máy trạng thái của một phiên, phía CLIENT.
//
// NHIỆM VỤ
//   Đối tác của HostSession. Lo toàn bộ việc bắt tay, giữ nhịp, và mọi thứ phát đi
//   trên kênh Control: PING đo RTT, REQUEST_KEYFRAME khi mất hình, FEEDBACK báo
//   chất lượng, SET_FOCUS khi người dùng đổi cửa sổ, và cả hàng đợi input.
//
// MÁY TRẠNG THÁI
//   Idle ──Start()──→ Hello ──HELLO_ACK──→ Starting ──gói video đầu tiên──→ Streaming
//                       │                     │                                │
//                       │ 10 giây im lặng     └──── BYE / timeout 5 giây ──────┤
//                       └──────────────────────────────────────────────────→ Dead
//
//   Hello    — phát HELLO lại mỗi 0.5 giây tới khi có ACK, bỏ cuộc sau 10 giây.
//   Starting — đã có tham số phiên, đã gọi onReady để client dựng decoder; phát
//              START lại mỗi 0.5 giây. Chuyển sang Streaming khi thấy gói video
//              đầu tiên — đó là bằng chứng duy nhất rằng host đã nhận được START.
//   Streaming— chạy bình thường: PING mỗi giây, timeout 5 giây.
//   Dead     — đã gọi onDisconnect; không tự hồi phục, client phải tạo phiên mới.
//
// VÌ SAO PHẢI PHÁT LẠI HELLO VÀ START
//   Cả hai đi trên UDP và đều không có ACK riêng. Mất HELLO thì host không biết có
//   ai gọi; mất START thì host nằm chờ ở READY còn client ngồi nhìn màn hình đen.
//   Cứ phát lại đều đặn là xong — HostSession chịu được gói lặp (HELLO lặp chỉ làm
//   nó gửi lại ACK, START lặp không đổi gì).
//
// PHÂN CÔNG VỚI NGƯỜI GỌI
//   Lớp này CHỈ xử lý kênh Control. Gói Video do caller tự đưa thẳng vào
//   Reassembler (đường nóng, không nên đi vòng), rồi gọi NotifyVideoPacket để nuôi
//   timeout và đẩy Starting → Streaming. Tương tự, lớp này không tự đo được mất gói
//   nên FEEDBACK phải do caller dựng từ LinkStats rồi đưa vào SendFeedback.
//
// MÔ HÌNH LUỒNG
//   Thuần C++20 như HostSession: byte ra qua callback `send`, thời gian bơm từ
//   ngoài. Toàn bộ chạy trên MỘT thread (thread Recv của client) — không khoá,
//   và khác HostSession, ở đây không có trường atomic nào.
//
// LIÊN QUAN: deskhub/session/HostSession.h (đầu kia), deskhub/input/InputSender.h,
//            deskhub/transport/Reassembler.h, deskhub/control/LinkStats.h
// =============================================================================
#include "deskhub/input/InputSender.h"
#include "deskhub/session/ClipboardAssembler.h"
#include "deskhub/wire/Wire.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace deskhub {

inline constexpr uint64_t kHelloRetryUs = 500'000;     // phát lại HELLO/START
inline constexpr uint64_t kHelloGiveUpUs = 10'000'000; // bỏ cuộc nếu host im lặng
inline constexpr uint64_t kPingIntervalUs = 1'000'000;
inline constexpr uint64_t kKeyframeRetryUs = 250'000;
// SET_FOCUS gửi theo BIẾN CỐ (đổi cửa sổ), không gửi định kỳ: phát lại đều đặn thì
// người ngồi ở máy host không bao giờ bấm sang được ứng dụng khác của chính mình.
// Đổi lại phải chịu mất gói → phát kFocusRepeats lần cách kFocusRetryUs cho chắc.
inline constexpr uint64_t kFocusRetryUs = 50'000;
inline constexpr int kFocusRepeats = 3;

struct NegotiatedParams {
    Codec codec = Codec::H264;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t fps = 60;
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
    // GĐ8: host vừa copy văn bản — caller đặt vào clipboard máy mình.
    std::function<void(std::string text)> onClipboard;
};

class ClientSession {
public:
    enum class State : uint8_t { Idle,
        Hello,
        Starting,
        Streaming,
        Dead };

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
    void RequestKeyframe() {
        keyframeWanted_ = true;
    }
    void CancelKeyframeRequest() {
        keyframeWanted_ = false;
    }

    // Gửi FEEDBACK cho host (GĐ5). Caller gọi ~1s/lần từ khối thống kê của mình —
    // ClientSession không tự đo được mất gói (Reassembler nằm ngoài). Bỏ qua nếu
    // chưa STREAMING.
    void SendFeedback(const Feedback& fb);

    // Gửi NACK xin host gửi lại các mảnh `indices` của `frameId` (GĐ7). Caller lấy
    // danh sách từ Reassembler::PlanNack. Bỏ qua nếu chưa STREAMING hoặc indices rỗng.
    void SendNack(uint32_t frameId, std::span<const uint16_t> indices);

    // Báo host đã bỏ hẳn `frameId` để nó thôi tham chiếu (GĐ7). Bỏ qua nếu chưa STREAMING.
    void SendInvalidateRef(uint32_t frameId);

    // Gửi văn bản clipboard của máy mình cho host (GĐ8), tự chia mảnh. Bỏ qua nếu
    // chưa STREAMING, text rỗng hoặc quá kMaxClipboardBytes.
    void SendClipboard(std::string_view utf8);

    // Báo host mình rời đi (gửi 1 lần, best-effort) và kết thúc phiên.
    void SendBye();

    State state() const {
        return state_;
    }
    uint32_t sessionId() const {
        return sessionId_;
    }
    uint32_t lastRttUs() const {
        return lastRttUs_;
    }
    const NegotiatedParams& params() const {
        return params_;
    }

private:
    void SendHello();
    void SendStart();
    void Die(const char* reason);

    ClientCallbacks cb_;
    InputSender input_;
    State state_ = State::Idle;
    uint32_t sessionId_ = 0;
    Hello hello_{};
    NegotiatedParams params_{};
    uint64_t startedUs_ = 0;  // lúc phát HELLO đầu — mốc bỏ cuộc
    uint64_t lastSentUs_ = 0; // lần phát HELLO/START gần nhất
    uint64_t lastRecvUs_ = 0;
    uint64_t lastPingUs_ = 0;
    uint64_t lastKeyframeReqUs_ = 0;
    uint64_t lastFocusUs_ = 0;
    int focusRepeatsLeft_ = 0;
    bool focusWanted_ = false; // giá trị đang phát lại
    bool focusSent_ = false;   // giá trị host đã biết — khỏi phát lại thừa
    uint32_t nextPingId_ = 1;
    uint32_t lastRttUs_ = 0;
    bool keyframeWanted_ = false;
    ClipboardAssembler clip_;   // ghép mảnh clipboard từ host (GĐ8)
    uint32_t clipUpdateId_ = 0; // updateId của lần SendClipboard kế tiếp
    uint8_t buf_[kMaxDatagram] = {};
};

} // namespace deskhub
