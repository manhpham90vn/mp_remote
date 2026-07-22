#pragma once
// =============================================================================
// InputCapture.h — bắt phím/chuột trên cửa sổ preview, phía CLIENT (GĐ4).
//
// NHIỆM VỤ
//   Biến thao tác của người dùng trên cửa sổ preview thành deskhub::InputEvent để gửi
//   sang host. Đối tác ở đầu kia là InputInjector.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   **InputCapture** → ClientSession::QueueInput → InputSender → UDP
//                                                    ~~~> InputReceiver → InputInjector
//
// HAI CHẾ ĐỘ CHUỘT — và vì sao phải có cả hai
//   TUYỆT ĐỐI (mặc định): WM_MOUSEMOVE → toạ độ client chuẩn hoá 0..65535.
//       Con trỏ của người dùng và con trỏ trên máy host trùng nhau. Đúng cho ứng
//       dụng cửa sổ, menu, game chiến thuật.
//   TƯƠNG ĐỐI (F9 bật/tắt): delta thô từ Raw Input, con trỏ bị khoá và ẩn.
//       BẮT BUỘC cho game FPS: game đọc chuột thô rồi tự kéo con trỏ về giữa màn
//       hình. Gửi toạ độ tuyệt đối vào loại game đó thì camera giật liên tục vì
//       hai bên liên tục đánh nhau về vị trí con trỏ.
//
// VÌ SAO BÀN PHÍM LUÔN QUA RAW INPUT
//   Cần SCANCODE, không phải mã phím ảo. Game dùng DirectInput/Raw Input đọc thẳng
//   scancode; gửi mỗi vk thì game không thấy gì. Đây chính là lý do phần lớn công
//   cụ điều khiển từ xa không chơi được game.
//
// ⚠ MÔ HÌNH LUỒNG
//   Toàn bộ chạy trên LUỒNG MESSAGE (main). `sink` được gọi ngay trong WndProc nên
//   phải NHẸ: chỉ đẩy vào hàng đợi, tuyệt đối không gửi socket ở đây — chặn WndProc
//   là đóng băng cả giao diện.
//
// LIÊN QUAN: input/InputInjector.h (đầu kia), decode/Renderer.h (nguồn message),
//            deskhub/input/InputSender.h, docs/07-phase4-input.md
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <functional>

#include "deskhub/wire/Wire.h"

class InputCapture {
public:
    using Sink = std::function<void(const deskhub::InputEvent&)>;

    // Đăng ký Raw Input cho cửa sổ preview. Gọi trên luồng sẽ bơm message.
    bool Attach(HWND hwnd, Sink sink);
    void Detach();

    // Gọi từ WndProc TRƯỚC khi Renderer xử lý. true = đã tiêu thụ message.
    bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Tạm dừng/tiếp tục gửi input (vd. chưa STREAMING, hoặc người dùng tắt).
    void SetEnabled(bool on);
    bool enabled() const { return enabled_; }
    bool relativeMode() const { return relative_; }

    // GD5: cùng đường với phím tắt F9/F10, để nút bấm trên overlay preview gọi
    // trực tiếp (không lặp lại logic).
    void ToggleRelativeMode(); // == F9
    void TogglePause();        // == F10

private:
    void SetRelativeMode(bool on);
    void Emit(deskhub::InputType type, int32_t a, int32_t b, uint8_t state, uint8_t absolute);
    void OnRawInput(LPARAM lp);
    void EmitButton(deskhub::MouseButton btn, bool down);

    HWND hwnd_ = nullptr;
    Sink sink_;
    bool enabled_  = false;
    bool relative_ = false;
    bool attached_ = false;
    int  buttonsDown_ = 0; // đếm nút đang giữ -> biết khi nào nhả SetCapture
};
