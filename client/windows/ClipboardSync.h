#pragma once
// =============================================================================
// ClipboardSync.h — theo dõi & đặt clipboard VĂN BẢN của Windows, chống vòng echo.
//
// NHIỆM VỤ
//   Nửa nền-tảng của tính năng đồng bộ clipboard (GĐ8). Core lo wire + ghép mảnh
//   (deskhub/session/ClipboardAssembler.h); lớp này lo hai việc dính Windows:
//     1. NGHE clipboard máy mình đổi (AddClipboardFormatListener) → báo caller
//        qua onLocal để gửi cho máy kia.
//     2. ĐẶT clipboard theo văn bản máy kia gửi sang (SetRemoteText).
//
// CHỐNG VÒNG ECHO
//   Đặt clipboard cũng làm listener của CHÍNH máy này bắn WM_CLIPBOARDUPDATE —
//   không chặn thì hai máy ném cùng một văn bản qua lại vô hạn. Chặn bằng cách
//   nhớ lastText_: văn bản vừa đặt (SetRemoteText) hoặc vừa đọc; update nào có
//   nội dung trùng lastText_ đều bị nuốt. Cách này đồng thời khử luôn các update
//   trùng lặp (nhiều phiên cùng gửi một bản copy).
//
// MÔ HÌNH LUỒNG
//   Start() phóng MỘT thread riêng sở hữu message-only window + vòng bơm message
//   (clipboard listener bắt buộc phải có HWND). onLocal chạy trên thread đó —
//   caller tự lo an toàn luồng (mailbox có khoá). SetRemoteText gọi được từ thread
//   bất kỳ: nó chỉ Post văn bản sang thread clipboard rồi về ngay.
//
// CHỈ VĂN BẢN (CF_UNICODETEXT)
//   Ảnh/file để ngoài phạm vi v1: chúng cần định dạng riêng trên wire và trần kích
//   thước hoàn toàn khác. Văn bản là 95% giá trị của tính năng.
//
// LIÊN QUAN: deskhub/session/ClipboardAssembler.h (nửa core), docs/04-protocol.md
//            §CLIPBOARD, ClientLoop.cpp / AgentLoop.cpp (hai người dùng)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class ClipboardSync {
public:
    // Văn bản UTF-8 người dùng máy này vừa copy (đã lọc echo/trùng).
    using OnLocalText = std::function<void(const std::string& utf8)>;

    ClipboardSync() = default;
    ~ClipboardSync() {
        Stop();
    }
    ClipboardSync(const ClipboardSync&) = delete;
    ClipboardSync& operator=(const ClipboardSync&) = delete;

    // Phóng thread clipboard, trả về khi listener đã sẵn sàng.
    bool Start(OnLocalText onLocal);
    void Stop();

    // Đặt clipboard máy này theo văn bản (UTF-8) từ máy kia. Thread bất kỳ.
    void SetRemoteText(const std::string& utf8);

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void ThreadMain();
    void OnClipboardChanged();
    void ApplyRemote(const std::wstring& text);

    OnLocalText onLocal_;
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr}; // message-only window, thuộc thread_
    HANDLE ready_ = nullptr;          // event: hwnd_ đã tạo xong

    std::mutex mu_;
    std::wstring lastText_; // văn bản vừa đặt/vừa đọc — chốt chống echo
};
