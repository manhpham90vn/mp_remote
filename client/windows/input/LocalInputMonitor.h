#pragma once
// =============================================================================
// LocalInputMonitor.h — phát hiện người NGỒI TẠI MÁY đang dùng chuột/phím THẬT.
// Nửa "tai mắt" của cơ chế "host thắng" khi hai bên cùng điều khiển (GĐ4).
//
// VẤN ĐỀ
//   Input từ xa và input của người ngồi máy trộn thẳng vào nhau trong hàng đợi
//   của Windows: con trỏ bị giằng co, phím bổ trợ lây chéo (host giữ Ctrl thật
//   trong lúc remote gõ S thành Ctrl+S). Các tool thương mại xử lý bằng ưu tiên:
//   khi có tranh chấp, NGƯỜI NGỒI TẠI MÁY thắng — remote bị tạm ngắt một quãng
//   ngắn rồi tự có lại quyền.
//
// CÁCH LÀM
//   Hook toàn cục WH_KEYBOARD_LL + WH_MOUSE_LL ghi lại thời điểm sự kiện VẬT LÝ
//   gần nhất. Sự kiện do SendInput bơm (chính là input từ xa mình đang bơm) mang
//   cờ LLKHF_INJECTED/LLMHF_INJECTED — lọc bỏ, không thì tự bơm tự khoá vòng vô
//   tận. InputInjector::Apply đọc mốc này và bỏ qua input từ xa khi mốc còn mới.
//
// VÌ SAO CẦN THREAD RIÊNG
//   Hook LL đòi thread cài nó phải bơm message đều (Windows giao sự kiện qua
//   message loop của thread đó, thread lì quá lâu là hook bị hệ thống gỡ). Thread
//   Recv của AgentLoop chặn ở recvfrom nên không gánh được — thread ở đây chỉ
//   cài hook rồi GetMessage, không làm gì khác.
//
// GIỚI HẠN ĐÁNG BIẾT
//   Host chạy quyền thường thì hook LL không thấy input đi vào cửa sổ elevated
//   (UIPI) — "host thắng" không kích hoạt khi người ngồi máy đang gõ vào app
//   admin. Chấp nhận: cùng cấu hình đó SendInput của ta cũng không tới app admin.
//
// LIÊN QUAN: input/InputInjector.cpp (nơi đọc LastPhysicalUs), AgentLoop.cpp
//            (nơi Start/Stop theo phiên share), docs/07-phase4-input.md
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

class LocalInputMonitor {
public:
    LocalInputMonitor() = default;
    ~LocalInputMonitor() {
        Stop();
    }
    LocalInputMonitor(const LocalInputMonitor&) = delete;
    LocalInputMonitor& operator=(const LocalInputMonitor&) = delete;

    // Cài hook trên thread riêng, trả về ngay. Hook hỏng thì cơ chế "host thắng"
    // im lặng tắt (LastPhysicalUs giữ 0), mọi thứ khác chạy bình thường.
    void Start();

    // Gỡ hook và join thread. Gọi được nhiều lần / khi chưa Start.
    void Stop();

    // Mốc NowUs() của sự kiện chuột/phím VẬT LÝ gần nhất trên máy này;
    // 0 = chưa thấy gì / monitor không chạy. Đọc được từ bất kỳ thread nào.
    static uint64_t LastPhysicalUs();

private:
    void ThreadMain();

    std::thread thread_;
    // Stop() đặt cờ TRƯỚC khi post WM_QUIT — Stop chạy sớm hơn lúc thread kịp
    // ghi threadId_ thì message không gửi được, cờ là đường thoát duy nhất
    // (cùng khuôn với SessionWindow::quitReq_).
    std::atomic<bool> quit_{false};
    std::atomic<DWORD> threadId_{0};
};
