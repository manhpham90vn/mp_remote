#pragma once
//
// InputInjector (GD4, phía HOST) - nhận rgc::InputEvent từ client và bơm vào
// máy này bằng SendInput.
//
// Ánh xạ tọa độ: client gửi tọa độ CHUẨN HÓA (0..65535) trong khung hình nó
// nhìn thấy = vùng client của cửa sổ đang chia sẻ. Host quy đổi ngược về pixel
// trong client rect của cửa sổ đích, rồi đổi sang tọa độ màn hình ảo cho
// SendInput. Nhờ vậy client thu nhỏ cửa sổ preview vẫn trỏ đúng chỗ.
//
// Bàn phím bơm bằng SCANCODE (KEYEVENTF_SCANCODE) chứ không bằng mã phím ảo:
// game dùng DirectInput/Raw Input đọc thẳng scancode, gửi vk không thôi thì
// game không thấy gì (đây là lý do phần lớn công cụ remote không điều khiển
// được game).
//
// Chống KẸT PHÍM: injector nhớ mọi phím/nút đang giữ. Client mất kết nối giữa
// lúc đang giữ W (chạy tới) mà không nhận được sự kiện nhả -> nhân vật chạy mãi.
// ReleaseAll() nhả hết, HostSession gọi khi BYE/timeout.
//
// Apply() được gọi từ luồng Recv của AgentLoop.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <map>
#include <set>

#include "rgc/Wire.h"

class InputInjector {
public:
    // `target` = cửa sổ đang chia sẻ (gốc tọa độ). Đưa lên foreground để input
    // tới đúng nó. Trả false nếu target không hợp lệ.
    bool Init(HWND target);

    // Bơm một event. Bỏ qua nếu đang tắt hoặc cửa sổ đích đã đóng.
    void Apply(const rgc::InputEvent& e);

    // Nhả mọi phím/nút còn đang giữ (mất kết nối, kết thúc phiên, tắt input).
    void ReleaseAll();

    void SetEnabled(bool on);
    bool enabled() const { return enabled_; }

    uint64_t applied() const { return applied_; }
    uint64_t skipped() const { return skipped_; }

    // Dev (--injecttest): gõ một chuỗi ASCII vào cửa sổ `target` bằng chính
    // đường bơm thật (scancode). Để kiểm chứng riêng nửa "bơm input" mà không
    // cần dùng mạng - xem docs/07-phase4-input.md §6.
    static int SelfTest(HWND target, const char* text);

private:
    // SendInput bơm vào CỬA SỔ ĐANG FOREGROUND, không phải vào một HWND cụ thể.
    // Nếu người ở máy host bấm sang ứng dụng khác, mọi phím/chuột từ client sẽ
    // rơi vào ứng dụng đó - vừa sai vừa nguy hiểm (người điều khiển từ xa gõ vào
    // trình duyệt, terminal... của chủ máy). Nên: chỉ bơm khi cửa sổ đang chia sẻ
    // thật sự đang foreground; không thì bỏ qua và nhả hết phím đang giữ.
    bool TargetHasFocus();

    void SendKey(int32_t vk, int32_t scan, bool down);
    void SendButton(rgc::MouseButton btn, bool down);
    void SendMoveAbsolute(int32_t nx, int32_t ny);
    void SendMoveRelative(int32_t dx, int32_t dy);

    HWND target_ = nullptr;
    bool enabled_ = true;
    bool hadFocus_ = true;   // để chỉ log một lần mỗi khi đổi trạng thái focus
    uint64_t applied_ = 0;
    uint64_t skipped_ = 0;   // event bị bỏ vì cửa sổ đích không còn foreground
    std::map<int32_t, int32_t> keysDown_;     // scancode (kèm bit E0) -> mã phím ảo
    std::set<rgc::MouseButton> buttonsDown_;
};
