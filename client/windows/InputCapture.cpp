#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "InputCapture.h"

#include <windowsx.h> // GET_X_LPARAM / GET_XBUTTON_WPARAM / GET_WHEEL_DELTA_WPARAM

#include "TimeUs.h"

#include <cstdio>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace {

constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageMouse       = 0x02;
constexpr USHORT kUsageKeyboard    = 0x06;

constexpr int kToggleRelativeKey = VK_F9; // bat/tat khoa chuot (che do tuong doi)

// Toa do client -> 0..65535. Dung (n-1) lam mau so de canh phai/duoi dat dung
// 65535, khong bi hut mot pixel khi host quy doi nguoc.
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

    // Khong dung RIDEV_NOLEGACY: van can message thuong (WM_MOUSEMOVE cho che do
    // tuyet doi, WM_CLOSE, keo cua so...). Khong dung RIDEV_INPUTSINK: chi bat
    // input khi cua so preview dang focus - nguoi dung alt-tab ra ngoai thi go
    // vao may minh nhu binh thuong.
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = kUsagePageGeneric;
    rid[0].usUsage     = kUsageMouse;
    rid[0].hwndTarget  = hwnd;
    rid[1].usUsagePage = kUsagePageGeneric;
    rid[1].usUsage     = kUsageKeyboard;
    rid[1].hwndTarget  = hwnd;
    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        std::printf("[Input] RegisterRawInputDevices that bai: %lu\n", GetLastError());
        return false;
    }
    attached_ = true;
    std::printf("[Input] Dang lai ban phim + chuot toi may host.\n");
    std::printf("[Input]   F9  = khoa/tha chuot (che do tuong doi cho game FPS)\n");
    std::printf("[Input]   F10 = tam dung/tiep tuc gui input\n");
    return true;
}

void InputCapture::Detach() {
    if (!attached_) return;
    SetRelativeMode(false);
    // Huy dang ky: usUsagePage/Usage nhu cu + RIDEV_REMOVE, hwndTarget phai NULL.
    RAWINPUTDEVICE rid[2] = {};
    rid[0].usUsagePage = kUsagePageGeneric;
    rid[0].usUsage     = kUsageMouse;
    rid[0].dwFlags     = RIDEV_REMOVE;
    rid[1].usUsagePage = kUsagePageGeneric;
    rid[1].usUsage     = kUsageKeyboard;
    rid[1].dwFlags     = RIDEV_REMOVE;
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

void InputCapture::SetRelativeMode(bool on) {
    if (relative_ == on) return;
    relative_ = on;
    if (on) {
        RECT r{};
        GetClientRect(hwnd_, &r);
        POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
        ClientToScreen(hwnd_, &tl);
        ClientToScreen(hwnd_, &br);
        RECT screen{ tl.x, tl.y, br.x, br.y };
        ClipCursor(&screen); // giu con tro trong cua so preview
        while (ShowCursor(FALSE) >= 0) {}
        SetCapture(hwnd_);
        std::printf("[Input] Da KHOA chuot (che do tuong doi). F9 de tha.\n");
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        if (GetCapture() == hwnd_ && buttonsDown_ == 0) ReleaseCapture();
        std::printf("[Input] Da THA chuot (che do tuyet doi).\n");
    }
}

void InputCapture::Emit(rgc::InputType type, int32_t a, int32_t b,
                        uint8_t state, uint8_t absolute) {
    if (!enabled_ || !sink_) return;
    rgc::InputEvent e;
    e.type        = type;
    e.timestampUs = QpcUs();
    e.a           = a;
    e.b           = b;
    e.state       = state;
    e.absolute    = absolute;
    sink_(e);
}

void InputCapture::EmitButton(rgc::MouseButton btn, bool down) {
    // Giu chuot khi nhan de van nhan duoc nut-nha ngoai vung cua so (keo tha).
    if (down) {
        if (buttonsDown_++ == 0) SetCapture(hwnd_);
    } else if (buttonsDown_ > 0) {
        if (--buttonsDown_ == 0 && !relative_ && GetCapture() == hwnd_) ReleaseCapture();
    }
    Emit(rgc::InputType::MouseButton, int32_t(btn), 0, down ? 1 : 0, 0);
}

void InputCapture::OnRawInput(LPARAM lp) {
    UINT size = 0;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0)
        return;
    // RAWINPUT co kich thuoc thay doi; buffer tinh du cho ca hai loai thiet bi.
    alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
    if (size > sizeof(buf)) return;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size)
        return;
    const RAWINPUT* ri = (const RAWINPUT*)buf;

    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& kb = ri->data.keyboard;
        if (kb.VKey == 0xFF) return; // phim gia cua ban phim (vd. nua Pause)
        const bool down = (kb.Flags & RI_KEY_BREAK) == 0;

        // Phim dieu khien cuc bo: xu ly o day, KHONG gui di.
        if (kb.VKey == kToggleRelativeKey) {
            if (down && enabled_) SetRelativeMode(!relative_);
            return;
        }
        if (kb.VKey == VK_F10) {
            if (down) {
                enabled_ = !enabled_;
                if (!enabled_) SetRelativeMode(false);
                std::printf("[Input] %s gui input.\n", enabled_ ? "TIEP TUC" : "TAM DUNG");
            }
            return;
        }

        int32_t scan = kb.MakeCode;
        if (kb.Flags & RI_KEY_E0) scan |= rgc::kScanExtended;
        Emit(rgc::InputType::Key, int32_t(kb.VKey), scan, down ? 1 : 0, 0);
        return;
    }

    if (ri->header.dwType == RIM_TYPEMOUSE && relative_) {
        const RAWMOUSE& m = ri->data.mouse;
        // Chuot tuyet doi (may ao/RDP/bang ve) khong cho delta -> bo qua, che do
        // tuyet doi (WM_MOUSEMOVE) van hoat dong dung cho cac thiet bi do.
        if (m.usFlags & MOUSE_MOVE_ABSOLUTE) return;
        if (m.lLastX || m.lLastY)
            Emit(rgc::InputType::MouseMove, m.lLastX, m.lLastY, 0, 0);
    }
}

bool InputCapture::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!attached_ || hwnd != hwnd_) return false;

    switch (msg) {
    case WM_INPUT:
        OnRawInput(lp);
        return false; // WM_INPUT PHAI di tiep toi DefWindowProc de he thong don

    case WM_MOUSEMOVE: {
        if (relative_) return true; // delta lay tu Raw Input roi
        RECT r{};
        GetClientRect(hwnd_, &r);
        Emit(rgc::InputType::MouseMove,
             Normalize(GET_X_LPARAM(lp), uint32_t(r.right - r.left)),
             Normalize(GET_Y_LPARAM(lp), uint32_t(r.bottom - r.top)), 0, 1);
        return true;
    }

    case WM_LBUTTONDOWN: EmitButton(rgc::MouseButton::Left, true);    return true;
    case WM_LBUTTONUP:   EmitButton(rgc::MouseButton::Left, false);   return true;
    case WM_RBUTTONDOWN: EmitButton(rgc::MouseButton::Right, true);   return true;
    case WM_RBUTTONUP:   EmitButton(rgc::MouseButton::Right, false);  return true;
    case WM_MBUTTONDOWN: EmitButton(rgc::MouseButton::Middle, true);  return true;
    case WM_MBUTTONUP:   EmitButton(rgc::MouseButton::Middle, false); return true;
    case WM_XBUTTONDOWN:
        EmitButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? rgc::MouseButton::X1
                                                      : rgc::MouseButton::X2, true);
        return true;
    case WM_XBUTTONUP:
        EmitButton(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? rgc::MouseButton::X1
                                                      : rgc::MouseButton::X2, false);
        return true;

    case WM_MOUSEWHEEL:
        Emit(rgc::InputType::MouseWheel, 0, GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return true;

    // Phim da lay qua WM_INPUT; nuot message thuong de Renderer khong dong cua so
    // khi nguoi dung bam ESC trong game o may kia. (WM_CLOSE van dong duoc.)
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
        return enabled_;

    case WM_KILLFOCUS:
        // Mat focus khi dang khoa chuot -> tha ra, khong thi nguoi dung ket con tro.
        SetRelativeMode(false);
        return false;
    }
    return false;
}
