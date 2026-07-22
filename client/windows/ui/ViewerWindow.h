#pragma once
// =============================================================================
// ViewerWindow.h — cửa sổ quản lý phiên XEM, phía CLIENT.
//
// ĐỐI XỨNG VỚI ui/SessionWindow.h PHÍA HOST
//   Cùng vấn đề: sau khi bấm Connect, màn hình chính ẩn đi và RunClient chặn —
//   ngoài các cửa sổ preview ra không còn giao diện nào để biết mình đang xem
//   gì, mở thêm nguồn, hay ngắt kết nối gọn gàng.
//
// MÔ HÌNH DANH SÁCH: KHÁC HẲN PHÍA HOST — cửa sổ này hiện TOÀN BỘ nguồn host
//   đang chia sẻ (không chỉ nguồn đang xem), đánh dấu nguồn nào đang mở. Vì phía
//   client không "thêm" gì của riêng nó cả — nó chỉ đồng bộ với danh sách của
//   host — nên các nút là:
//     Refresh    — hỏi lại host (LIST_SOURCES) xem giờ nó chia sẻ những gì.
//     View       — mở nguồn đang chọn trong danh sách.
//     Stop       — dừng xem nguồn đang chọn.
//     Disconnect — ngắt cả phiên, quay về màn hình chính.
//   Không cần hộp thoại chọn nguồn thứ hai như bản đầu (nút "Add" cũ): danh sách
//   nằm ngay đây, chọn dòng rồi bấm View là xong.
//
// HỎI HOST KHÔNG CHẶN THREAD UI
//   QueryHostSources chặn tới ~3s (phát lại LIST_SOURCES qua UDP). Chạy nó ngay
//   trên thread UI là cửa sổ đơ — nên Refresh phóng một thread hỏi riêng, xong
//   thì post WM_APP về; nút Refresh bị khoá trong lúc chờ nên không bao giờ có
//   hai thread hỏi cùng lúc.
//
// MÔ HÌNH LUỒNG — y hệt SessionWindow, đọc giải thích đầy đủ ở đó
//   Thread UI riêng tự bơm message; nói chuyện với vòng Main của RunClient qua
//   hộp thư có mutex: adds_/removes_/stopReq_ đi xuống, SetRows(nguồn đang xem)
//   đi lên, timer ~300ms đổ lại listbox khi có thay đổi.
//
// LIÊN QUAN: ui/SessionWindow.h (bản đối xứng + lý do thiết kế), ClientLoop.cpp
//            (đầu kia của hộp thư), ClientLoop.h (QueryHostSources), ui/SessionRow.h
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

#include "net/UdpSocket.h" // NetAddr
#include "ui/SessionRow.h"
#include "deskhub/wire/Wire.h" // SourceInfo

class ViewerWindow {
public:
    ViewerWindow() = default;
    ~ViewerWindow() {
        Stop();
    }
    ViewerWindow(const ViewerWindow&) = delete;
    ViewerWindow& operator=(const ViewerWindow&) = delete;

    // Mở cửa sổ trên thread UI riêng, trả về ngay. `server` để hiển thị và để
    // hỏi danh sách khi Refresh; `maxSources` = trần số nguồn xem cùng lúc.
    void Start(const NetAddr& server, size_t maxSources);

    // Đóng cửa sổ và join các thread. Gọi được nhiều lần / khi chưa Start.
    // Có thể chặn tới ~3s nếu một lần Refresh đang hỏi host dở chừng.
    void Stop();

    // True khi cửa sổ đang sống. False = tạo cửa sổ hỏng hoặc đã Stop —
    // vòng Main dựa vào đây để rơi về hành vi cũ (đóng hết preview là hết phiên).
    bool active() const {
        return active_.load(std::memory_order_acquire);
    }

    // Người dùng bấm Disconnect / đóng cửa sổ — vòng Main thấy là kết thúc phiên.
    bool stopRequested() const {
        return stopReq_.load(std::memory_order_acquire);
    }

    // Vòng Main đẩy danh sách nguồn ĐANG XEM (để đánh dấu trong danh sách của
    // host). So sánh với lần trước, chỉ đánh dấu dirty khi khác.
    void SetRows(std::vector<SessionSourceRow> rows);

    // Vòng Main rút các nguồn người dùng vừa yêu cầu mở (nút View).
    std::vector<deskhub::SourceInfo> TakeAdds();

    // Vòng Main rút các sourceId người dùng vừa yêu cầu dừng xem (nút Stop).
    std::vector<uint8_t> TakeRemoves();

private:
    void ThreadMain();
    LRESULT HandleMsg(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK WndProcThunk(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void RebuildList();
    void StartRefresh(bool manual);

    std::thread thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopReq_{false};
    // Stop() đặt cờ này TRƯỚC khi post message đóng — cùng lý do với SessionWindow:
    // Stop chạy sớm hơn lúc cửa sổ tạo xong thì join() treo vĩnh viễn.
    std::atomic<bool> quitReq_{false};
    std::atomic<HWND> hwnd_{nullptr}; // để Stop()/thread hỏi host post message về
    NetAddr server_{};
    size_t maxSources_ = 8;

    // --- Hộp thư giữa thread Main / thread hỏi host và thread UI (mutex m_) ---
    std::mutex m_;
    std::vector<SessionSourceRow> rows_; // Main ghi (nguồn đang xem), UI chép khi dirty
    bool dirty_ = false;
    std::vector<deskhub::SourceInfo> adds_; // UI ghi, Main rút
    std::vector<uint8_t> removes_;          // UI ghi, Main rút
    std::vector<deskhub::SourceInfo> queryResult_; // thread hỏi host ghi, UI chép
    bool queryOk_ = false;

    // --- Chỉ thread UI chạm ---
    std::vector<SessionSourceRow> uiViewRows_;    // nguồn đang xem (bản UI)
    std::vector<deskhub::SourceInfo> hostList_;   // danh sách host trả về lần Refresh cuối
    bool refreshing_ = false;                     // nút Refresh đang khoá
    bool manualRefresh_ = false;                  // chỉ báo lỗi khi người dùng tự bấm
    bool everRefreshed_ = false;                  // đã có kết quả lần nào chưa (placeholder)
    HWND list_ = nullptr;
    HWND btnRefresh_ = nullptr;

    // Thread hỏi host của lần Refresh gần nhất. UI thread tạo; join ở lần Refresh
    // kế (chắc chắn đã xong — nút bị khoá) hoặc ở Stop().
    std::thread queryThread_;
};
