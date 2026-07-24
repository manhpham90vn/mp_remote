// =============================================================================
// InputCapture.cpp — cài đặt việc bắt phím/chuột từ cửa sổ preview.
//
// HAI NGUỒN MESSAGE, DÙNG CHO HAI VIỆC KHÁC NHAU
//   Raw Input (WM_INPUT)  — bàn phím (cần scancode) và chuột ở chế độ TƯƠNG ĐỐI
//                           (cần delta thô, không qua tăng tốc con trỏ của Windows).
//   Message thường        — chuột ở chế độ TUYỆT ĐỐI (WM_MOUSEMOVE cho toạ độ
//                           client), nút chuột, con lăn.
//   Cả hai cùng chạy: đó là lý do KHÔNG dùng cờ RIDEV_NOLEGACY khi đăng ký.
//
// HAI CỜ RAW INPUT CỐ Ý KHÔNG DÙNG — mỗi cái vì một lý do riêng
//   RIDEV_NOLEGACY  — sẽ tắt message thường, mà ta còn cần chúng (xem trên), kể cả
//                     WM_CLOSE và việc kéo cửa sổ.
//   RIDEV_INPUTSINK — sẽ bắt input KỂ CẢ khi cửa sổ không focus. Không muốn: người
//                     dùng alt-tab ra ngoài thì phải gõ vào máy mình như bình thường.
//
// QUY ƯỚC "TIÊU THỤ" MESSAGE
//   OnMessage trả true nghĩa là Renderer bỏ qua message đó hoàn toàn. Khi đang bật
//   gửi input, ta nuốt gần như mọi thứ — kể cả ESC — để người dùng gõ vào MÁY KIA.
//   Riêng F9 luôn được xử lý tại chỗ vì đó là phím thoát hiểm (thả chuột).
//
// LIÊN QUAN: input/InputCapture.h (hai chế độ chuột + lý do), input/InputInjector.cpp
//            (đầu kia, đối xứng từng bước)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "input/InputCapture.h"

#include <windowsx.h> // GET_X_LPARAM / GET_XBUTTON_WPARAM / GET_WHEEL_DELTA_WPARAM

#include "deskhubp/Clock.h"

#include <cstdio>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace {

constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageMouse = 0x02;
constexpr USHORT kUsageKeyboard = 0x06;

constexpr int kToggleRelativeKey = VK_F9; // bật/tắt khóa chuột (chế độ tương đối)

// Tọa độ client -> 0..65535. Dùng (n-1) làm mẫu số để cạnh phải/dưới đạt đúng
// 65535, không bị hụt một pixel khi host quy đổi ngược.
int32_t Normalize(int v, uint32_t extent) {
    if (extent <= 1) return 0;
    if (v < 0) v = 0;
    if (uint32_t(v) >= extent) v = int(extent) - 1;
    return int32_t(int64_t(v) * 65535 / int64_t(extent - 1));
}

} // namespace

bool InputCapture::Attach(HWND hwnd, Sink sink) {
    if (!hwnd) return false;
    hwnd_ = hwnd;
    sink_ = std::move(sink);

    // Không dùng RIDEV_NOLEGACY: vẫn cần message thường (WM_MOUSEMOVE cho chế độ
    // tuyệt đối, WM_CLOSE, kéo cửa sổ...). Không dùng RIDEV_INPUTSINK: chỉ bắt
    // input khi cửa sổ preview đang focus - người dùng alt-tab ra ngoài thì gõ
    // vào máy mình như bình thường.
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = kUsagePageGeneric;
    rid[0].usUsage = kUsageMouse;
    rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = kUsagePageGeneric;
    rid[1].usUsage = kUsageKeyboard;
    rid[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        std::printf("[Input] RegisterRawInputDevices failed: %lu\n", GetLastError());
        return false;
    }
    attached_ = true;
    std::printf("[Input] Capturing keyboard + mouse input to send to host.\n");
    std::printf("[Input]   F9 = lock/release mouse (relative mode for FPS games)\n");
    return true;
}

void InputCapture::Detach() {
    if (!attached_) return;
    SetRelativeMode(false);
    // Hủy đăng ký: usUsagePage/Usage như cũ + RIDEV_REMOVE, hwndTarget phải NULL.
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = kUsagePageGeneric;
    rid[0].usUsage = kUsageMouse;
    rid[0].dwFlags = RIDEV_REMOVE;
    rid[1].usUsagePage = kUsagePageGeneric;
    rid[1].usUsage = kUsageKeyboard;
    rid[1].dwFlags = RIDEV_REMOVE;
    RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));
    attached_ = false;
    hwnd_ = nullptr;
    sink_ = nullptr;
}

void InputCapture::SetEnabled(bool on) {
    if (enabled_ == on) return;
    enabled_ = on;
    if (!on) SetRelativeMode(false);
}

void InputCapture::ToggleRelativeMode() {
    if (enabled_) SetRelativeMode(!relative_);
}

// Vào/ra chế độ tương đối. Ba việc phải làm cùng lúc khi khoá chuột, và cả ba đều
// cần thiết: ClipCursor giữ con trỏ trong cửa sổ (không cho lạc sang màn hình
// khác), ShowCursor ẩn nó đi (game đã tự vẽ tâm ngắm rồi), SetCapture để vẫn nhận
// được message chuột khi con trỏ chạm mép.
//
// Vòng `while (ShowCursor(...))`: ShowCursor là BỘ ĐẾM chứ không phải cờ bật/tắt.
// Gọi một lần chưa chắc ẩn được nếu nơi khác đã tăng đếm lên; phải lặp tới khi
// dấu của bộ đếm đổi.
void InputCapture::SetRelativeMode(bool on) {
    if (relative_ == on) return;
    relative_ = on;
    if (on) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        POINT tl{r.left, r.top}, br{r.right, r.bottom};
        ClientToScreen(hwnd_, &tl);
        ClientToScreen(hwnd_, &br);
        RECT screen{tl.x, tl.y, br.x, br.y};
        ClipCursor(&screen); // giữ con trỏ trong cửa sổ preview
        while (ShowCursor(FALSE) >= 0) {}
        SetCapture(hwnd_);
        std::printf("[Input] Mouse LOCKED (relative mode). Press F9 to release.\n");
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        if (GetCapture() == hwnd_ && buttonsDown_ == 0) ReleaseCapture();
        std::printf("[Input] Mouse RELEASED (absolute mode).\n");
    }
}

void InputCapture::Emit(deskhub::InputType type, int32_t a, int32_t b,
    uint8_t state, uint8_t absolute) {
    if (!enabled_ || !sink_) return;
    deskhub::InputEvent e;
    e.type = type;
    e.timestampUs = NowUs();
    e.a = a;
    e.b = b;
    e.state = state;
    e.absolute = absolute;
    sink_(e);
}

// Đếm số nút đang giữ để biết lúc nào được nhả SetCapture. Không đếm mà nhả ngay
// khi có một nút lên thì thao tác kéo-thả bằng hai nút sẽ đứt giữa chừng — và tệ
// hơn, sự kiện nhả của nút còn lại rơi ra ngoài cửa sổ, gây kẹt nút ở máy host.
void InputCapture::EmitButton(deskhub::MouseButton btn, bool down) {
    // Giữ chuột khi nhấn để vẫn nhận được nút-nhả ngoài vùng cửa sổ (kéo thả).
    if (down) {
        if (buttonsDown_++ == 0) SetCapture(hwnd_);
    } else if (buttonsDown_ > 0) {
        if (--buttonsDown_ == 0 && !relative_ && GetCapture() == hwnd_) ReleaseCapture();
    }
    Emit(deskhub::InputType::MouseButton, int32_t(btn), 0, down ? 1 : 0, 0);
}

void InputCapture::OnRawInput(LPARAM lp) {
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
        return;
    // RAWINPUT có kích thước thay đổi theo loại thiết bị; +64 là dư cho cả hai loại
    // ta đăng ký. Đệm trên stack để không cấp phát trên đường nóng — chuột di chuyển
    // sinh hàng trăm message mỗi giây. alignas(8) vì RAWINPUT chứa trường 64-bit.
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
    if (size > sizeof(buf)) return;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size)
        return;
    const RAWINPUT* ri = (const RAWINPUT*)buf;

    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = ri->data.keyboard;
        if (kb.VKey == 0xFF) return; // phím giả của bàn phím (vd. nửa Pause)
        const bool down = (kb.Flags & RI_KEY_BREAK) == 0;

        // Phím điều khiển cục bộ: xử lý ở đây, KHÔNG gửi đi.
        if (kb.VKey == kToggleRelativeKey) {
            if (down) ToggleRelativeMode();
            return;
        }

        int32_t scan = kb.MakeCode;
        if (kb.Flags & RI_KEY_E0) scan |= deskhub::kScanExtended;
        Emit(deskhub::InputType::Key, int32_t(kb.VKey), scan, down ? 1 : 0, 0);
        return;
    }

    if (ri->header.dwType == RIM_TYPEMOUSE && relative_) {
        const RAWMOUSE& m = ri->data.mouse;
        // Chuột tuyệt đối (máy ảo/RDP/bảng vẽ) không cho delta -> bỏ qua, chế độ
        // tuyệt đối (WM_MOUSEMOVE) vẫn hoạt động đúng cho các thiết bị đó.
        if (m.usFlags & MOUSE_MOVE_ABSOLUTE) return;
        if (m.lLastX || m.lLastY)
            Emit(deskhub::InputType::MouseMove, m.lLastX, m.lLastY, 0, 0);
    }
}

bool InputCapture::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!attached_ || hwnd != hwnd_) return false;

    switch (msg) {
        case WM_INPUT:
            OnRawInput(lp);
            return false; // WM_INPUT PHẢI đi tiếp tới DefWindowProc để hệ thống dọn

        case WM_MOUSEMOVE: {
            if (relative_) return true; // delta lấy từ Raw Input rồi
            RECT r{};
            GetClientRect(hwnd_, &r);
            Emit(deskhub::InputType::MouseMove,
                Normalize(GET_X_LPARAM(lp), uint32_t(r.right - r.left)),
                Normalize(GET_Y_LPARAM(lp), uint32_t(r.bottom - r.top)), 0, 1);
            return true;
        }

        case WM_LBUTTONDOWN: EmitButton(deskhub::MouseButton::Left, true); return true;
        case WM_LBUTTONUP: EmitButton(deskhub::MouseButton::Left, false); return true;
        case WM_RBUTTONDOWN: EmitButton(deskhub::MouseButton::Right, true); return true;
        case WM_RBUTTONUP: EmitButton(deskhub::MouseButton::Right, false); return true;
        case WM_MBUTTONDOWN: EmitButton(deskhub::MouseButton::Middle, true); return true;
        case WM_MBUTTONUP: EmitButton(deskhub::MouseButton::Middle, false); return true;
        case WM_XBUTTONDOWN:
            EmitButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? deskhub::MouseButton::X1
                                                          : deskhub::MouseButton::X2,
                true);
            return true;
        case WM_XBUTTONUP:
            EmitButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? deskhub::MouseButton::X1
                                                          : deskhub::MouseButton::X2,
                false);
            return true;

        case WM_MOUSEWHEEL:
            Emit(deskhub::InputType::MouseWheel, 0, GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
            return true;

        // Phím đã lấy qua WM_INPUT; nuốt message thường để Renderer không đóng cửa sổ
        // khi người dùng bấm ESC trong game ở máy kia. (WM_CLOSE vẫn đóng được.)
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            return enabled_;

        case WM_KILLFOCUS:
            // Mất focus khi đang khóa chuột -> thả ra, không thì người dùng kẹt con trỏ.
            SetRelativeMode(false);
            return false;
    }
    return false;
}
