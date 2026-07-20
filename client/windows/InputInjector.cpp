#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "InputInjector.h"

#include <cstdio>

#pragma comment(lib, "user32.lib")

namespace {

// Doi pixel man hinh -> toa do chuan hoa 0..65535 tren MAN HINH AO (toan bo
// cac man hinh ghep lai). MOUSEEVENTF_VIRTUALDESK bat buoc khi may nhieu man
// hinh, khong thi chuot bi ket o man hinh chinh.
void ScreenToVirtualDesk(int px, int py, LONG& nx, LONG& ny) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    nx = vw > 1 ? LONG(int64_t(px - vx) * 65535 / (vw - 1)) : 0;
    ny = vh > 1 ? LONG(int64_t(py - vy) * 65535 / (vh - 1)) : 0;
}

// Windows chan SetForegroundWindow tu process khong giu focus. Cach chuan (moi
// phan mem remote deu dung): tam GAN input queue cua minh vao luong dang so huu
// cua so foreground - luc do minh duoc tinh la "cung mot luong nhap lieu" nen
// SetForegroundWindow duoc phep. Gan xong phai go ra ngay, khong thi hai luong
// dung chung trang thai ban phim/focus va sinh loi kho hieu.
bool ForceForeground(HWND w) {
    if (SetForegroundWindow(w)) return true;

    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    const DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
    const DWORD myThread = GetCurrentThreadId();
    if (fgThread == myThread) return false;

    bool ok = false;
    if (AttachThreadInput(myThread, fgThread, TRUE)) {
        // Cua so bi thu nho/an thi SetForegroundWindow khong an thua.
        ShowWindow(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
        BringWindowToTop(w);
        ok = SetForegroundWindow(w) != FALSE;
        AttachThreadInput(myThread, fgThread, FALSE);
    }
    return ok;
}

DWORD ButtonFlag(rgc::MouseButton b, bool down) {
    switch (b) {
    case rgc::MouseButton::Left:   return down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;
    case rgc::MouseButton::Right:  return down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;
    case rgc::MouseButton::Middle: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    case rgc::MouseButton::X1:
    case rgc::MouseButton::X2:     return down ? MOUSEEVENTF_XDOWN      : MOUSEEVENTF_XUP;
    }
    return 0;
}

} // namespace

bool InputInjector::Init(HWND target) {
    if (!target || !IsWindow(target)) return false;
    target_ = target;
    // Dua cua so dich len truoc de input toi dung no. Windows han che
    // SetForegroundWindow tu process khong co focus -> best-effort, neu that bai
    // thi nguoi dung o may host tu bam vao cua so mot lan la xong.
    if (IsIconic(target_)) ShowWindow(target_, SW_RESTORE);
    if (!ForceForeground(target_))
        std::printf("[Inject] Khong dua duoc cua so dich len truoc - hay bam vao cua so "
                    "do mot lan o may nay (input chi bom khi no dang foreground).\n");
    return true;
}

void InputInjector::SetEnabled(bool on) {
    if (enabled_ == on) return;
    if (!on) ReleaseAll(); // tat giua chung khong duoc de ket phim
    enabled_ = on;
}

void InputInjector::SendKey(int32_t vk, int32_t scan, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (scan & 0xFF) {
        in.ki.wScan = WORD(scan & 0xFF);
        in.ki.dwFlags |= KEYEVENTF_SCANCODE;
        if (scan & rgc::kScanExtended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    } else {
        in.ki.wVk = WORD(vk); // client khong gui duoc scancode -> lui ve ma phim ao
    }
    SendInput(1, &in, sizeof(INPUT));
}

void InputInjector::SendButton(rgc::MouseButton btn, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = ButtonFlag(btn, down);
    if (btn == rgc::MouseButton::X1) in.mi.mouseData = XBUTTON1;
    if (btn == rgc::MouseButton::X2) in.mi.mouseData = XBUTTON2;
    if (in.mi.dwFlags) SendInput(1, &in, sizeof(INPUT));
}

void InputInjector::SendMoveAbsolute(int32_t nx, int32_t ny) {
    RECT rc{};
    if (!GetClientRect(target_, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 1 || h <= 1) return;

    // Chuan hoa (khung hinh client nhin thay) -> pixel trong client rect dich.
    POINT pt{ LONG(int64_t(nx) * (w - 1) / 65535),
              LONG(int64_t(ny) * (h - 1) / 65535) };
    if (!ClientToScreen(target_, &pt)) return;

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    ScreenToVirtualDesk(pt.x, pt.y, in.mi.dx, in.mi.dy);
    SendInput(1, &in, sizeof(INPUT));
}

void InputInjector::SendMoveRelative(int32_t dx, int32_t dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE; // khong ABSOLUTE = delta, dung cho game FPS
    in.mi.dx = dx;
    in.mi.dy = dy;
    SendInput(1, &in, sizeof(INPUT));
}

bool InputInjector::TargetHasFocus() {
    const HWND fg = GetForegroundWindow();
    // Cua so dich hoac cua so con/popup cua no (hop thoai, menu) deu tinh la dung.
    const bool ok = fg && (fg == target_ || GetAncestor(fg, GA_ROOT) == target_);
    if (ok != hadFocus_) {
        hadFocus_ = ok;
        if (ok) {
            std::printf("[Inject] Cua so chia se da co focus - input hoat dong tro lai.\n");
        } else {
            std::printf("[Inject] Cua so chia se MAT focus - tam bo qua input tu client "
                        "(khong bom nham vao ung dung khac).\n");
            ReleaseAll(); // dang giu W ma mat focus -> nha ra, khong de ket phim
        }
    }
    return ok;
}

void InputInjector::Apply(const rgc::InputEvent& e) {
    if (!enabled_ || !target_ || !IsWindow(target_)) return;
    if (!TargetHasFocus()) { ++skipped_; return; }
    ++applied_;

    switch (e.type) {
    case rgc::InputType::Key: {
        const bool down = e.state != 0;
        // Nho theo scancode de ReleaseAll nha dung phim da bom.
        if (down) keysDown_[e.b] = e.a;
        else      keysDown_.erase(e.b);
        SendKey(e.a, e.b, down);
        break;
    }
    case rgc::InputType::MouseMove:
        if (e.absolute) SendMoveAbsolute(e.a, e.b);
        else            SendMoveRelative(e.a, e.b);
        break;
    case rgc::InputType::MouseButton: {
        const auto btn = rgc::MouseButton(e.a);
        const bool down = e.state != 0;
        if (down) buttonsDown_.insert(btn);
        else      buttonsDown_.erase(btn);
        SendButton(btn, down);
        break;
    }
    case rgc::InputType::MouseWheel: {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = DWORD(e.b);
        SendInput(1, &in, sizeof(INPUT));
        break;
    }
    }
}

int InputInjector::SelfTest(HWND target, const char* text) {
    InputInjector inj;
    if (!inj.Init(target)) {
        std::printf("[InjectTest] Cua so dich khong hop le.\n");
        return 1;
    }
    Sleep(1000); // cho cua so len foreground + game ve trang thai on dinh
    // KHONG bom mu: neu cua so dich chua o foreground, phim se roi vao ung dung
    // dang foreground (terminal, trinh duyet...) - go bay vao dung noi khong ngo.
    if (!inj.TargetHasFocus()) {
        std::printf("[InjectTest] DUNG: cua so dich khong o foreground. Bam vao cua so do "
                    "roi chay lai (input se bi go nham vao ung dung dang mo).\n");
        return 1;
    }
    std::printf("[InjectTest] Cua so dich dang foreground - bat dau bom \"%s\".\n", text);

    // Dung dung duong ma client se gui: vk + scancode lay tu ban do ban phim
    // hien hanh, KHONG dung ky tu - giong het event that tu Raw Input.
    for (const char* p = text; *p; ++p) {
        const SHORT vkAll = VkKeyScanA(*p);
        if (vkAll == -1) continue;
        const int32_t vk = vkAll & 0xFF;
        const bool needShift = (vkAll >> 8) & 1;
        const int32_t scan = int32_t(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC));
        if (!scan) continue;

        rgc::InputEvent e;
        e.type = rgc::InputType::Key;
        if (needShift) {
            e.a = VK_SHIFT;
            e.b = int32_t(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
            e.state = 1;
            inj.Apply(e);
        }
        e.a = vk; e.b = scan;
        e.state = 1; inj.Apply(e);
        Sleep(40); // game doc ban phim theo frame - nhan/nha qua nhanh se bi bo qua
        e.state = 0; inj.Apply(e);
        if (needShift) {
            e.a = VK_SHIFT;
            e.b = int32_t(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
            e.state = 0;
            inj.Apply(e);
        }
        Sleep(15);
    }
    inj.ReleaseAll();
    std::printf("[InjectTest] Da bom %llu event (bo qua %llu vi mat focus). "
                "Kiem tra cua so dich.\n",
                (unsigned long long)inj.applied(), (unsigned long long)inj.skipped());
    return inj.applied() ? 0 : 1;
}

void InputInjector::ReleaseAll() {
    if (keysDown_.empty() && buttonsDown_.empty()) return;
    std::printf("[Inject] Nha %zu phim + %zu nut chuot con dang giu.\n",
                keysDown_.size(), buttonsDown_.size());
    for (const auto& [scan, vk] : keysDown_) SendKey(vk, scan, false);
    for (auto btn : buttonsDown_)            SendButton(btn, false);
    keysDown_.clear();
    buttonsDown_.clear();
}
