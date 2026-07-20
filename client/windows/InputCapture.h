#pragma once
//
// InputCapture (GD4, phía CLIENT) - bắt phím/chuột trên cửa sổ preview rồi
// biến thành rgc::InputEvent để gửi đi.
//
// Hai chế độ chuột:
//   TUYỆT ĐỐI (mặc định): WM_MOUSEMOVE -> tọa độ client chuẩn hóa 0..65535.
//       Con trỏ chuột của người dùng và con trỏ trên máy host trùng nhau -
//       dùng cho ứng dụng cửa sổ, menu, game chiến thuật.
//   TƯƠNG ĐỐI (F9 bật/tắt): delta thô từ Raw Input, con trỏ bị khóa + ẩn.
//       Bắt buộc cho game FPS: game đọc chuột thô và tự kéo con trỏ về giữa,
//       nếu gửi tọa độ tuyệt đối thì camera sẽ giật liên tục.
//
// Bàn phím luôn lấy từ Raw Input để có SCANCODE. Game DirectInput/raw đọc
// scancode chứ không đọc mã phím ảo -> gửi vk không thôi là game không nhận.
//
// Toàn bộ chạy trên LUỒNG MESSAGE (main). Sink được gọi ngay trong WndProc nên
// phải nhẹ: chỉ đẩy vào hàng đợi, không gửi socket ở đây.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <functional>

#include "rgc/Wire.h"

class InputCapture {
public:
    using Sink = std::function<void(const rgc::InputEvent&)>;

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
    void Emit(rgc::InputType type, int32_t a, int32_t b, uint8_t state, uint8_t absolute);
    void OnRawInput(LPARAM lp);
    void EmitButton(rgc::MouseButton btn, bool down);

    HWND hwnd_ = nullptr;
    Sink sink_;
    bool enabled_  = false;
    bool relative_ = false;
    bool attached_ = false;
    int  buttonsDown_ = 0; // đếm nút đang giữ -> biết khi nào nhả SetCapture
};
