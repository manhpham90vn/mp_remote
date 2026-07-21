#pragma once
//
// ClientLoop — vòng đời client trên Android, bản port của client/windows/ClientLoop.cpp
// cho GĐ3 (view-only: chưa gửi input). Nguồn muốn xem do caller chọn sẵn qua
// QuerySources() (SourceQuery.h) rồi truyền vào Start().
//
// Ba thread, đúng lý do như bên Windows:
//   Main (UI thread của Activity): giao/thu hồi Surface, hỏi trạng thái.
//   Net: recvfrom -> ClientSession + Reassembler -> đẩy frame vào hàng đợi.
//   Decode: rút frame -> MediaCodecDecoder -> Surface.
// Vì sao tách Net và Decode: nếu decode chặn thread Net thì recvfrom ngừng nghe,
// buffer UDP của OS tràn và sinh mất gói THẬT.
//
#include <android/native_window.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "MediaCodecDecoder.h"
#include "UdpSocket.h"

#include "rgc/Reassembler.h"

class ClientLoop {
public:
    // Trạng thái cho tầng UI hiển thị. Không trùng ClientSession::State: UI chỉ cần
    // biết "đang quay bánh xe" hay "đã có hình", không cần Hello/Starting.
    enum class Phase : int32_t { Idle = 0, Connecting = 1, Streaming = 2, Ended = 3 };

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
    bool Finished() const { return finished_.load(std::memory_order_acquire); }

    Phase phase() const { return phase_.load(std::memory_order_acquire); }

    // Dòng số liệu cho overlay (fps/kbps/RTT/e2e), cập nhật 1s/lần. Chuỗi rỗng khi
    // chưa có số liệu. Có khóa vì UI thread đọc còn thread Net ghi.
    std::string StatusLine();

    // Lý do phiên kết thúc, để UI báo cho người dùng thay vì im lặng.
    std::string EndReason();

    // Kích thước video đàm phán được — UI dùng để đặt đúng tỉ lệ khung SurfaceView.
    uint32_t videoWidth() const { return negW_.load(); }
    uint32_t videoHeight() const { return negH_.load(); }

private:
    void NetThread();
    void DecodeThread();

    NetAddr   server_{};
    uint8_t   sourceId_ = 0;
    UdpSocket sock_;

    std::thread netThread_;
    std::thread decodeThread_;

    std::atomic<bool>  quit_{false};
    std::atomic<bool>  finished_{false};
    std::atomic<Phase> phase_{Phase::Idle};

    // Chuỗi hiển thị: thread Net ghi, UI thread đọc.
    std::mutex  textMutex_;
    std::string statusLine_;
    std::string endReason_;

    // Tham số đàm phán được (thread Net ghi, thread Decode đọc).
    std::atomic<uint32_t> negW_{0}, negH_{0};
    std::atomic<bool>     rebuildDecoder_{false}; // RECONFIG -> dựng lại codec

    // Surface: bắt tay theo thế hệ. Main tăng winGen_, Decode ack bằng winAckGen_.
    std::mutex              winMutex_;
    std::condition_variable winCv_;      // báo Decode có thay đổi
    std::condition_variable winAckCv_;   // báo Main thay đổi đã được áp dụng
    ANativeWindow*          window_ = nullptr;
    uint64_t                winGen_ = 0;
    uint64_t                winAckGen_ = 0;
    bool                    decodeExited_ = false;

    // Hàng đợi frame Net -> Decode.
    static constexpr size_t kMaxQueuedFrames = 3;
    std::mutex                          decMutex_;
    std::condition_variable             decCv_;
    std::deque<rgc::Reassembler::Frame> decQueue_;

    std::atomic<bool>     decodeFailed_{false};
    std::atomic<bool>     queueOverflow_{false};
    std::atomic<uint32_t> stRendered_{0};

    // Ước lượng trễ e2e (docs/06 §7): Net ghi, Decode đọc.
    std::atomic<int64_t>  ackDeltaUs_{0};
    std::atomic<uint32_t> minRttUs_{0};
    std::atomic<int64_t>  lastE2eUs_{-1};
};
