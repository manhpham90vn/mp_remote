#pragma once
// =============================================================================
// ClientLoop.h — vòng đời một phiên xem trên Android. Lớp trung tâm của phần C++.
//
// NHIỆM VỤ
//   Nối bốn thứ lại với nhau thành một phiên chạy được: socket UDP, máy trạng thái
//   ClientSession của core, bộ ghép mảnh Reassembler, và bộ giải mã MediaCodec.
//   Bản port của client/windows/ClientLoop.cpp. Kênh input phục vụ điều khiển từ
//   màn hình cảm ứng: chuột tuyệt đối (touch trên khung video), nút chuột, phím rời
//   (nút F9) và ký tự từ bàn phím ảo (QueueCharTap + KeyMap). Không có chế độ chuột
//   tương đối (F9-lock của Windows) — màn hình cảm ứng không có delta chuột thô.
//   Nguồn muốn xem do caller chọn sẵn qua QuerySources() rồi truyền vào Start().
//
// BA THREAD, VÀ LÝ DO CÓ TỪNG CÁI
//   Main (UI thread của Activity) — giao/thu hồi Surface, hỏi trạng thái để vẽ overlay.
//   Net    — recvfrom → ClientSession + Reassembler → đẩy frame vào hàng đợi.
//   Decode — rút frame khỏi hàng đợi → MediaCodecDecoder → Surface.
//
//   Vì sao Net và Decode phải tách: nếu giải mã chạy ngay trên thread Net thì trong
//   lúc nó bận, recvfrom ngừng nghe, buffer UDP của hệ điều hành tràn và sinh mất
//   gói THẬT — loại mất mát mà cả FEC lẫn xin IDR đều không cứu được, vì gói đã bị
//   vứt trước khi tới tay chương trình.
//
// HAI CƠ CHẾ ĐỒNG BỘ, ĐỪNG NHẦM LẪN
//   1. HÀNG ĐỢI FRAME (decMutex_/decCv_/decQueue_) — Net sản xuất, Decode tiêu thụ.
//      Có giới hạn kMaxQueuedFrames = 3: đầy thì VỨT frame cũ nhất chứ không chặn
//      thread Net. Thà bỏ hình còn hơn nghẽn đường nhận (xem lý do ở trên).
//
//   2. BẮT TAY SURFACE (winMutex_/winCv_/winAckCv_/winGen_/winAckGen_) — Main giao
//      hoặc thu hồi Surface, và phải CHỜ Decode xác nhận đã buông. Đây là cơ chế
//      duy nhất trong app mà một thread chặn để đợi thread khác, và nó bắt buộc
//      phải có: ANativeWindow bị hủy trong khi codec còn đang render vào đó là lỗi
//      dùng-sau-giải-phóng, thường biểu hiện thành app chết ngay lập tức.
//      Cách làm là đếm thế hệ: Main tăng winGen_, Decode ack bằng winAckGen_. Dùng
//      số đếm thay cho cờ bool để nhiều lần đổi liên tiếp không nuốt mất lần nào.
//      Cờ decodeExited_ là lối thoát chống treo vĩnh viễn khi thread Decode đã chết.
//
// VÌ SAO NHIỀU std::atomic ĐẾN THẾ
//   Các trường chỉ mang một giá trị đơn (kích thước video, cờ xin dựng lại codec,
//   số frame đã render) được đọc/ghi chéo giữa các thread nhưng không cần đồng bộ
//   với gì khác, nên atomic là đủ và rẻ hơn khoá. Chỉ những thứ đi thành CỤM —
//   chuỗi trạng thái, hàng đợi frame, trạng thái Surface — mới cần mutex.
//
// LIÊN QUAN: JniBridge.cpp (người gọi duy nhất), decode/MediaCodecDecoder.h,
//            deskhub/session/ClientSession.h, deskhub/transport/Reassembler.h,
//            client/windows/ClientLoop.cpp (bản song song)
// =============================================================================
#include <android/native_window.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decode/MediaCodecDecoder.h"
#include "net/UdpSocket.h"

#include "deskhub/transport/Reassembler.h"
#include "deskhub/wire/Wire.h"

class ClientLoop {
public:
    // Trạng thái cho tầng UI hiển thị. Không trùng ClientSession::State: UI chỉ cần
    // biết "đang quay bánh xe" hay "đã có hình", không cần Hello/Starting.
    enum class Phase : int32_t { Idle = 0,
        Connecting = 1,
        Streaming = 2,
        Ended = 3 };

    ClientLoop() = default;
    ~ClientLoop();
    ClientLoop(const ClientLoop&) = delete;
    ClientLoop& operator=(const ClientLoop&) = delete;

    // `sourceId` lấy từ SOURCE_LIST (xem SourceQuery.h); 0 = nguồn đầu tiên, cũng là
    // thứ host bản cũ (một nguồn) hiểu được.
    bool Start(const NetAddr& server, uint8_t sourceId);
    void Stop();

    // Giao Surface mới, hoặc nullptr khi hệ điều hành thu hồi (APP_CMD_TERM_WINDOW).
    // CHẶN tới khi thread Decode xác nhận đã buông surface cũ — bắt buộc, vì
    // ANativeWindow bị hủy trong khi codec còn đang render vào nó là dùng-sau-giải-phóng.
    void SetWindow(ANativeWindow* window);

    // true khi phiên đã kết thúc (host BYE / timeout / lỗi) — main thoát app.
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

    // Gõ một phím rời (nhấn rồi nhả ngay) sang host — phục vụ nút F9 trên header.
    // `vk` là mã phím ảo Windows, `scan` là scancode (bit8 = cờ E0, xem Wire.h).
    void QueueKeyTap(int32_t vk, int32_t scan);

    // Chuột tuyệt đối từ màn hình cảm ứng: `nx`/`ny` chuẩn hoá 0..65535 trong khung
    // video — cùng hệ toạ độ với InputCapture bên Windows, host map lên khung hình
    // đã capture. Caller tự chuẩn hoá theo rect của view video; ở đây chỉ kẹp biên.
    void QueueMouseMoveAbs(int32_t nx, int32_t ny);

    // Nhấn/nhả một nút chuột tại vị trí con trỏ hiện hành. `button` theo
    // deskhub::MouseButton (1 = trái, 2 = phải).
    void QueueMouseButton(int32_t button, bool down);

    // Gõ một KÝ TỰ từ bàn phím ảo: KeyMap (layout US) đổi thành chuỗi
    // [Shift↓] key↓ key↑ [Shift↑]. Ký tự không quy đổi được thì lặng lẽ bỏ qua.
    void QueueCharTap(uint32_t codepoint);

    // Kích thước video đàm phán được — UI dùng để đặt đúng tỉ lệ khung SurfaceView.
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
    std::atomic<bool> rebuildDecoder_{false}; // RECONFIG -> dựng lại codec

    // Surface: bắt tay theo thế hệ. Main tăng winGen_, Decode ack bằng winAckGen_.
    std::mutex winMutex_;
    std::condition_variable winCv_;    // báo Decode có thay đổi
    std::condition_variable winAckCv_; // báo Main thay đổi đã được áp dụng
    ANativeWindow* window_ = nullptr;
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
    std::mutex inputMutex_;
    std::vector<deskhub::InputEvent> inputQueue_;
    std::atomic<bool> wantFocus_{false};

    // --- Chẩn đoán (docs/09): t_dec của cửa sổ 1s. Thread Decode ghi, thread Net
    // đọc-và-reset. Max ghi kiểu load/store (một writer duy nhất là Decode) — đua
    // với lần reset cùng lắm rơi một mẫu, chấp nhận được cho số liệu chẩn đoán. ---
    std::atomic<uint32_t> dgDecMsSum_{0}, dgDecMsMax_{0}, dgDecCount_{0};

    // Ước lượng trễ e2e (docs/06 §7): Net ghi, Decode đọc.
    std::atomic<int64_t> ackDeltaUs_{0};
    std::atomic<uint32_t> minRttUs_{0};
    std::atomic<int64_t> lastE2eUs_{-1};
};
