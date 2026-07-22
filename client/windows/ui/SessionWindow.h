#pragma once
// =============================================================================
// SessionWindow.h — cửa sổ quản lý phiên chia sẻ đang chạy, phía HOST.
//
// VẤN ĐỀ
//   Trước đây bấm Share xong là màn hình chính ẩn đi (SW_HIDE) và RunAgent chặn
//   tới hết phiên — người dùng KHÔNG còn giao diện nào: không biết đang chia sẻ
//   gì, không thêm/bớt được nguồn, muốn dừng chỉ có cách đóng cửa sổ đang share
//   hoặc giết tiến trình trong Task Manager.
//
// GIẢI PHÁP
//   Một cửa sổ nhỏ sống suốt phiên share, hiện danh sách nguồn đang chia sẻ và
//   ba nút: Add (mở lại picker để thêm màn hình/cửa sổ), Stop selected (tắt bớt
//   một nguồn), Stop sharing (kết thúc phiên, quay về màn hình chính).
//
// MÔ HÌNH LUỒNG — điểm quan trọng nhất của lớp này
//   Cửa sổ chạy trên MỘT THREAD UI RIÊNG (tự bơm message), vì thread gọi RunAgent
//   chính là vòng Recv — nó chặn ở recvfrom 100ms nên không bơm message được.
//   Hai thread nói chuyện qua hộp thư có mutex, KHÔNG gọi thẳng vào nhau:
//     UI  → Recv : adds_/removes_/stopReq_ — vòng Recv rút ra mỗi vòng lặp
//                  (TakeAdds/TakeRemoves/stopRequested).
//     Recv → UI  : SetRows(danh sách nguồn hiện tại) + cờ dirty; thread UI có
//                  timer ~300ms, thấy dirty thì đổ lại listbox.
//   Nhờ vậy không API nào ở đây chặn vòng Recv quá một lần khoá mutex ngắn.
//
// LIÊN QUAN: AgentLoop.cpp (nơi rút lệnh và đẩy danh sách), ui/WindowPickerDialog.h
//            (picker mở lại khi bấm Add), ui/MainMenuWindow.cpp (nơi ẩn màn chính)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AgentLoop.h"     // AgentSource
#include "ui/SessionRow.h" // SessionSourceRow — dùng chung với ViewerWindow phía client

class SessionWindow {
public:
    SessionWindow() = default;
    ~SessionWindow() {
        Stop();
    }
    SessionWindow(const SessionWindow&) = delete;
    SessionWindow& operator=(const SessionWindow&) = delete;

    // Mở cửa sổ trên thread UI riêng, trả về ngay. `port` chỉ để hiển thị;
    // `maxSources` = trần số nguồn (kMaxSources) để nút Add tự chặn.
    void Start(uint16_t port, size_t maxSources);

    // Đóng cửa sổ và join thread UI. Gọi được nhiều lần / khi chưa Start.
    void Stop();

    // True khi cửa sổ đang sống. False = tạo cửa sổ hỏng hoặc đã Stop —
    // vòng Recv dựa vào đây để rơi về hành vi cũ (hết nguồn là hết phiên).
    bool active() const {
        return active_.load(std::memory_order_acquire);
    }

    // Người dùng bấm Stop sharing / đóng cửa sổ — vòng Recv thấy là kết thúc phiên.
    bool stopRequested() const {
        return stopReq_.load(std::memory_order_acquire);
    }

    // Vòng Recv đẩy danh sách nguồn hiện tại. So sánh với lần trước, chỉ đánh
    // dấu dirty khi khác — gọi mỗi giây cũng không làm listbox nhấp nháy.
    void SetRows(std::vector<SessionSourceRow> rows);

    // Vòng Recv rút các nguồn người dùng vừa chọn thêm (nút Add).
    std::vector<AgentSource> TakeAdds();

    // Vòng Recv rút các sourceId người dùng vừa yêu cầu tắt (nút Stop selected).
    std::vector<uint8_t> TakeRemoves();

private:
    void ThreadMain();
    LRESULT HandleMsg(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK WndProcThunk(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void RefreshList();

    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopReq_{false};
    // Stop() đặt cờ này TRƯỚC khi post message đóng: nếu Stop chạy sớm hơn lúc
    // cửa sổ tạo xong (hwnd_ còn null, message không gửi được) thì ThreadMain
    // thấy cờ ngay sau khi tạo và tự đóng — không thì join() treo vĩnh viễn.
    std::atomic<bool> quitReq_{false};
    std::atomic<HWND> hwnd_{nullptr}; // để Stop() (thread khác) post message đóng
    uint16_t port_ = 0;
    size_t maxSources_ = 8;

    // --- Hộp thư giữa thread Recv và thread UI, mutex bảo vệ cả bốn ---
    std::mutex m_;
    std::vector<SessionSourceRow> rows_; // Recv ghi, UI chép ra khi dirty
    bool dirty_ = false;
    std::vector<AgentSource> adds_; // UI ghi, Recv rút
    std::vector<uint8_t> removes_;  // UI ghi, Recv rút

    // --- Chỉ thread UI chạm ---
    std::vector<SessionSourceRow> uiRows_; // bản đang hiện trong listbox
    HWND list_ = nullptr;
};
