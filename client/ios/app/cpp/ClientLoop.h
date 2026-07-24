#pragma once
// =============================================================================
// ClientLoop.h — vòng đời một phiên xem trên iOS. Lớp trung tâm của phần C++.
//                Port sát của client/android/.../ClientLoop.h.
//
// NHIỆM VỤ
//   Nối bốn thứ thành một phiên chạy được: socket UDP, máy trạng thái ClientSession
//   của core, bộ ghép mảnh Reassembler, và bộ giải mã VtDecoder. Kênh input phục vụ
//   điều khiển từ màn hình cảm ứng: chuột tuyệt đối (touch trên khung video), nút
//   chuột, phím rời (nút F9) và ký tự từ bàn phím ảo (QueueCharTap + KeyMap). Không
//   có chế độ chuột tương đối (F9-lock của Windows) — màn hình cảm ứng không có
//   delta chuột thô. Nguồn muốn xem do caller chọn sẵn qua QuerySources().
//
// BA THREAD, VÀ LÝ DO CÓ TỪNG CÁI
//   Main (main thread của app) — giao/thu hồi layer, hỏi trạng thái để vẽ overlay.
//   Net    — recvfrom → ClientSession + Reassembler → đẩy frame vào hàng đợi.
//   Decode — rút frame khỏi hàng đợi → VtDecoder → AVSampleBufferDisplayLayer.
//
//   Vì sao Net và Decode phải tách: nếu giải mã chạy ngay trên thread Net thì trong
//   lúc nó bận, recvfrom ngừng nghe, buffer UDP của hệ điều hành tràn và sinh mất
//   gói THẬT — loại mất mát mà cả FEC lẫn xin IDR đều không cứu được.
//
// HAI CƠ CHẾ ĐỒNG BỘ, ĐỪNG NHẦM LẪN
//   1. HÀNG ĐỢI FRAME (decMutex_/decCv_/decQueue_) — Net sản xuất, Decode tiêu thụ.
//      Giới hạn kMaxQueuedFrames = 3: đầy thì VỨT frame cũ nhất chứ không chặn Net.
//   2. BẮT TAY LAYER (winMutex_/winCv_/winAckCv_/winGen_/winAckGen_) — Main giao hoặc
//      thu hồi layer, và phải CHỜ Decode xác nhận đã buông. Bắt buộc: một
//      AVSampleBufferDisplayLayer bị buông trong khi decoder còn enqueue vào đó là
//      lỗi vòng đời. Đếm thế hệ để nhiều lần đổi liên tiếp không nuốt mất lần nào;
//      decodeExited_ là lối thoát chống treo khi thread Decode đã chết.
//
// LIÊN QUAN: ClientLoop.cpp, VtDecoder.h, deskhub/session/ClientSession.h,
//            deskhub/transport/Reassembler.h,
//            client/android/.../ClientLoop.h (bản song song)
// =============================================================================
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "VtDecoder.h"
#include "net/UdpSocket.h"

#include "deskhub/transport/Reassembler.h"
#include "deskhub/wire/Wire.h"

class ClientLoop {
public:
    // Trạng thái cho tầng UI hiển thị. UI chỉ cần biết "đang quay bánh xe" hay "đã
    // có hình", không cần Hello/Starting của ClientSession.
    enum class Phase : int32_t { Idle = 0,
        Connecting = 1,
        Streaming = 2,
        Ended = 3 };

    ClientLoop() = default;
    ~ClientLoop();
    ClientLoop(const ClientLoop&) = delete;
    ClientLoop& operator=(const ClientLoop&) = delete;

    // `sourceId` lấy từ SOURCE_LIST (xem SourceQuery.h); 0 = nguồn đầu tiên.
    bool Start(const NetAddr& server, uint8_t sourceId);
    void Stop();

    // Giao layer mới (AVSampleBufferDisplayLayer* dưới dạng __bridge void*), hoặc
    // nullptr khi app xuống nền / view biến mất. CHẶN tới khi thread Decode xác nhận
    // đã buông layer cũ — bắt buộc, vì decoder còn enqueue vào một layer đã buông là
    // lỗi vòng đời.
    void SetLayer(void* layer);

    // true khi phiên đã kết thúc (host BYE / timeout / lỗi) — main thoát về ConnectView.
    bool Finished() const {
        return finished_.load(std::memory_order_acquire);
    }

    Phase phase() const {
        return phase_.load(std::memory_order_acquire);
    }

    // Dòng số liệu cho overlay (fps/kbps/RTT/e2e), cập nhật 1s/lần. Chuỗi rỗng khi
    // chưa có số liệu. Có khóa vì UI thread đọc còn thread Net ghi.
    std::string StatusLine();

    // Lý do phiên kết thúc, để UI báo cho người dùng thay vì im lặng.
    std::string EndReason();

    // --- Kênh input (touch + bàn phím ảo). Tất cả gọi từ UI thread; thread Net vét
    // hàng đợi mỗi vòng rồi giao ClientSession đánh seq và gửi lặp chống kẹt phím
    // (InputSender). Chỉ có tác dụng khi phiên đang STREAMING. ---

    // Gõ một phím rời (nhấn rồi nhả sau kTapHoldUs) sang host — thanh phím tắt.
    // `vk` là mã phím ảo Windows, `scan` là scancode (bit8 = cờ E0, xem Wire.h).
    void QueueKeyTap(int32_t vk, int32_t scan);

    // Tổ hợp kiểu Ctrl+C: giữ phím bổ trợ (`modVk`), gõ phím chính, nhả phím chính
    // rồi mới nhả phím bổ trợ (cả hai nhả sau kTapHoldUs).
    void QueueKeyChord(int32_t modVk, int32_t modScan, int32_t vk, int32_t scan);

    // Chuột tuyệt đối từ màn hình cảm ứng: `nx`/`ny` chuẩn hoá 0..65535 trong khung
    // video — cùng hệ toạ độ với InputCapture bên Windows, host map lên khung hình
    // đã capture. Caller tự chuẩn hoá theo rect của view video; ở đây chỉ kẹp biên.
    void QueueMouseMoveAbs(int32_t nx, int32_t ny);

    // Chuột TƯƠNG ĐỐI — chế độ khoá chuột cho game FPS (đối ứng F9 bên client
    // Windows): dx/dy là delta thô, absolute = 0, host bơm qua SendMoveRelative và
    // game tự áp sensitivity. UI hiện CHƯA có nút bật (nút Lock từng có, bỏ vì rối
    // giao diện) — giữ API cho khi cần lại. Không kẹp biên — delta không có biên.
    void QueueMouseMoveRel(int32_t dx, int32_t dy);

    // Nhấn/nhả một nút chuột tại vị trí con trỏ hiện hành. `button` theo
    // deskhub::MouseButton (1 = trái, 2 = phải).
    void QueueMouseButton(int32_t button, bool down);

    // Gõ một KÝ TỰ từ bàn phím ảo: KeyMap (layout US) đổi thành chuỗi
    // [Shift↓] key↓ key↑ [Shift↑]. Ký tự không quy đổi được thì lặng lẽ bỏ qua.
    void QueueCharTap(uint32_t codepoint);

    // Kích thước video đàm phán được — UI dùng để đặt đúng tỉ lệ khung.
    uint32_t videoWidth() const {
        return negW_.load();
    }
    uint32_t videoHeight() const {
        return negH_.load();
    }

private:
    void NetThread();
    void DecodeThread();

    NetAddr server_{};
    uint8_t sourceId_ = 0;
    UdpSocket sock_;

    std::thread netThread_;
    std::thread decodeThread_;

    std::atomic<bool> quit_{false};
    std::atomic<bool> finished_{false};
    std::atomic<Phase> phase_{Phase::Idle};

    // Chuỗi hiển thị: thread Net ghi, UI thread đọc.
    std::mutex textMutex_;
    std::string statusLine_;
    std::string endReason_;

    // Tham số đàm phán được (thread Net ghi, thread Decode đọc).
    std::atomic<uint32_t> negW_{0}, negH_{0};
    std::atomic<bool> rebuildDecoder_{false}; // RECONFIG -> dựng lại decoder

    // Layer: bắt tay theo thế hệ. Main tăng winGen_, Decode ack bằng winAckGen_.
    std::mutex winMutex_;
    std::condition_variable winCv_;    // báo Decode có thay đổi
    std::condition_variable winAckCv_; // báo Main thay đổi đã được áp dụng
    void* layer_ = nullptr;            // AVSampleBufferDisplayLayer* (__bridge)
    uint64_t winGen_ = 0;
    uint64_t winAckGen_ = 0;
    bool decodeExited_ = false;

    // Hàng đợi frame Net -> Decode.
    static constexpr size_t kMaxQueuedFrames = 3;
    std::mutex decMutex_;
    std::condition_variable decCv_;
    std::deque<deskhub::Reassembler::Frame> decQueue_;

    std::atomic<bool> decodeFailed_{false};
    std::atomic<bool> queueOverflow_{false};
    std::atomic<uint32_t> stRendered_{0};

    // Input UI thread gom -> thread Net vét (cùng mô hình client Windows). Khóa chỉ
    // giữ vài chục nano giây quanh push/swap, không nằm trên đường nóng của video.
    // wantFocus_: đã từng gửi input thì phải báo host SET_FOCUS — SendInput bên host
    // chỉ tới được cửa sổ đang foreground.
    // delayedInput_: cú NHẢ phím của tap được hẹn giờ +kTapHoldUs thay vì đi liền
    // cú nhấn — down/up dính nhau 0ms thì game poll bàn phím theo frame
    // (GetAsyncKeyState mỗi ~16-33ms) không kịp thấy phím từng được nhấn.
    static constexpr uint64_t kTapHoldUs = 50'000;
    std::mutex inputMutex_;
    std::vector<deskhub::InputEvent> inputQueue_;
    std::vector<std::pair<uint64_t, deskhub::InputEvent>> delayedInput_; // (hạn nhả, event)
    std::atomic<bool> wantFocus_{false};

    // --- Chẩn đoán (docs/09): t_dec của cửa sổ 1s. Thread Decode ghi, thread Net
    // đọc-và-reset. ---
    std::atomic<uint32_t> dgDecMsSum_{0}, dgDecMsMax_{0}, dgDecCount_{0};

    // Ước lượng trễ e2e (docs/06 §7): Net ghi, Decode đọc.
    std::atomic<int64_t> ackDeltaUs_{0};
    std::atomic<uint32_t> minRttUs_{0};
    std::atomic<int64_t> lastE2eUs_{-1};
};
