// =============================================================================
// InputInjector.cpp — cài đặt việc bơm input vào máy host bằng SendInput.
//
// BỐ CỤC
//   ScreenToVirtualDesk() — pixel màn hình → toạ độ chuẩn hoá của SendInput.
//   ForceForeground()     — kéo cửa sổ đích lên foreground (xem cảnh báo bên dưới).
//   Init/InitMonitor()    — chọn gốc toạ độ: cửa sổ, hay cả màn hình.
//   Apply()               — đường chính, phân nhánh theo loại event.
//   SendKey/SendButton/SendMove* — các lời gọi SendInput cụ thể.
//   ReleaseAll()          — nhả sạch phím/nút đang giữ.
//
// HAI TẦNG QUY ĐỔI TOẠ ĐỘ — dễ nhầm nếu không tách bạch
//   1. Client gửi 0..65535 tương đối với VÙNG CLIENT của cửa sổ nguồn.
//      → quy về pixel trong client rect, rồi ra pixel màn hình.
//   2. SendInput lại đòi 0..65535 tương đối với MÀN HÌNH ẢO (mọi màn hình ghép lại).
//      → ScreenToVirtualDesk làm bước này.
//   Hai thang cùng dải 0..65535 nhưng gốc và độ dài khác hẳn nhau; nhầm chúng cho
//   ra con trỏ lệch chỗ mà vẫn "trông có vẻ đúng" ở màn hình đơn.
//
// ⚠ ForceForeground DÙNG ĐÚNG MỘT THỦ THUẬT ĐƯỢC CÔNG NHẬN
//   Windows chặn SetForegroundWindow từ tiến trình không giữ focus. Cách vòng qua
//   (mọi phần mềm điều khiển từ xa đều dùng) là tạm GẮN input queue của mình vào
//   luồng đang sở hữu cửa sổ foreground. Bắt buộc phải gỡ ra ngay sau đó — để dính
//   thì hai luồng dùng chung trạng thái bàn phím và sinh lỗi rất khó hiểu.
//
// TRẠNG THÁI PHẢI GIỮ: keysDown_ VÀ buttonsDown_
//   Đây không phải tối ưu mà là yêu cầu đúng đắn: không nhớ thì không nhả được khi
//   mất kết nối, và phím kẹt là lỗi tệ nhất của cả hệ thống (xem InputInjector.h).
//   keysDown_ khoá theo SCANCODE kèm bit E0, không phải theo vk — hai phím khác
//   nhau có thể cùng vk (Ctrl trái/phải) nhưng scancode luôn phân biệt được.
//
// LIÊN QUAN: input/InputInjector.h (hai cơ chế an toàn + ánh xạ toạ độ),
//            input/InputCapture.cpp (đầu kia), docs/07-phase4-input.md
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "input/InputInjector.h"

#include <cstdio>

#include "input/LocalInputMonitor.h"
#include "deskhubp/Clock.h"

#pragma comment(lib, "user32.lib")

namespace {

// "Host thắng": sau lần chuột/phím VẬT LÝ gần nhất của người ngồi máy, input từ
// xa bị bỏ qua thêm quãng này nữa. Đủ dài để host thao tác liền mạch không bị
// remote chen vào, đủ ngắn để remote lấy lại quyền gần như ngay khi host buông
// tay (các tool VNC/AnyDesk dùng cùng cỡ ~1s cho heuristic này).
constexpr uint64_t kHostWinsGraceUs = 1'000'000;

// Đổi pixel màn hình -> tọa độ chuẩn hóa 0..65535 trên MÀN HÌNH ẢO (toàn bộ
// các màn hình ghép lại). MOUSEEVENTF_VIRTUALDESK bắt buộc khi máy nhiều màn
// hình, không thì chuột bị kẹt ở màn hình chính.
void ScreenToVirtualDesk(int px, int py, LONG& nx, LONG& ny) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    nx = vw > 1 ? LONG(int64_t(px - vx) * 65535 / (vw - 1)) : 0;
    ny = vh > 1 ? LONG(int64_t(py - vy) * 65535 / (vh - 1)) : 0;
}

// Windows chặn SetForegroundWindow từ process không giữ focus. Cách chuẩn (mọi
// phần mềm remote đều dùng): tạm GẮN input queue của mình vào luồng đang sở hữu
// cửa sổ foreground - lúc đó mình được tính là "cùng một luồng nhập liệu" nên
// SetForegroundWindow được phép. Gắn xong phải gỡ ra ngay, không thì hai luồng
// dùng chung trạng thái bàn phím/focus và sinh lỗi khó hiểu.
bool ForceForeground(HWND w) {
    if (SetForegroundWindow(w)) return true;

    const HWND fg = GetForegroundWindow();
    if (!fg) return false;
    const DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
    const DWORD myThread = GetCurrentThreadId();
    if (fgThread == myThread) return false;

    bool ok = false;
    if (AttachThreadInput(myThread, fgThread, TRUE)) {
        // ⚠ CHỈ DÙNG BIẾN THỂ BẤT ĐỒNG BỘ ở đây. ShowWindow/BringWindowToTop
        // thường SendMessage ĐỒNG BỘ sang thread của cửa sổ đích — đích đang kẹt
        // trong vòng lặp modal (người ngồi máy host đang kéo cửa sổ / giữ menu,
        // chuyện chắc chắn xảy ra khi HAI BÊN cùng điều khiển) là thread Recv
        // đứng theo → video khựng cả phiên. Tệ hơn: vòng modal đó chỉ thoát khi
        // có mouse-up, mà mouse-up của client lại do chính thread Recv bơm →
        // hai bên đợi nhau VĨNH VIỄN (đơ toàn phiên, gặp thật 22/07/2026).
        // ShowWindowAsync + SWP_ASYNCWINDOWPOS chỉ POST yêu cầu rồi về ngay.
        ShowWindowAsync(w, IsIconic(w) ? SW_RESTORE : SW_SHOW);
        SetWindowPos(w, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        ok = SetForegroundWindow(w) != FALSE;
        AttachThreadInput(myThread, fgThread, FALSE);
    }
    return ok;
}

DWORD ButtonFlag(deskhub::MouseButton b, bool down) {
    switch (b) {
        case deskhub::MouseButton::Left: return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case deskhub::MouseButton::Right: return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case deskhub::MouseButton::Middle: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        case deskhub::MouseButton::X1:
        case deskhub::MouseButton::X2: return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    }
    return 0;
}

} // namespace

bool InputInjector::Init(HWND target) {
    if (!target || !IsWindow(target)) return false;
    target_ = target;
    // Đưa cửa sổ đích lên trước để input tới đúng nó. Windows hạn chế
    // SetForegroundWindow từ process không có focus -> best-effort, nếu thất bại
    // thì người dùng ở máy host tự bấm vào cửa sổ một lần là xong.
    // ShowWindowAsync, không ShowWindow: hàm này chạy trên thread Recv, mà bản
    // đồng bộ chặn đợi thread của cửa sổ đích — xem cảnh báo ở ForceForeground.
    if (IsIconic(target_)) ShowWindowAsync(target_, SW_RESTORE);
    if (!ForceForeground(target_))
        std::printf(
            "[Inject] Could not bring the target window to the foreground - please click "
            "that window once on this machine (input is only injected while it's foreground).\n");
    return true;
}

bool InputInjector::InitMonitor(HMONITOR monitor) {
    if (!monitor) return false;
    monitor_ = monitor;
    target_ = nullptr;
    return true;
}

bool InputInjector::FocusTarget() {
    // Cả màn hình đã được chia sẻ trọn vẹn -> không có cửa sổ riêng nào để kéo lên.
    if (monitor_) return true;
    if (!target_ || !IsWindow(target_)) return false;
    const HWND fg = GetForegroundWindow();
    if (fg && (fg == target_ || GetAncestor(fg, GA_ROOT) == target_)) return true;
    return ForceForeground(target_);
}

void InputInjector::SetEnabled(bool on) {
    if (enabled_ == on) return;
    if (!on) ReleaseAll(); // tắt giữa chừng không được để kẹt phím
    enabled_ = on;
}

// Ưu tiên scancode, lùi về mã phím ảo chỉ khi client không gửi được scancode.
// Thứ tự ưu tiên này là điểm mấu chốt của cả tính năng điều khiển game — xem
// InputInjector.h. KEYEVENTF_EXTENDEDKEY cho các phím có tiền tố E0 (mũi tên,
// Ctrl/Alt phải, phím trên cụm numpad): thiếu cờ này thì mũi tên hoá thành phím số.
void InputInjector::SendKey(int32_t vk, int32_t scan, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (scan & 0xFF) {
        in.ki.wScan = WORD(scan & 0xFF);
        in.ki.dwFlags |= KEYEVENTF_SCANCODE;
        if (scan & deskhub::kScanExtended) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    } else {
        in.ki.wVk = WORD(vk); // client không gửi được scancode -> lùi về mã phím ảo
    }
    SendInput(1, &in, sizeof(INPUT));
}

void InputInjector::SendButton(deskhub::MouseButton btn, bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = ButtonFlag(btn, down);
    if (btn == deskhub::MouseButton::X1) in.mi.mouseData = XBUTTON1;
    if (btn == deskhub::MouseButton::X2) in.mi.mouseData = XBUTTON2;
    if (in.mi.dwFlags) SendInput(1, &in, sizeof(INPUT));
}

// Quy đổi hai tầng — xem sơ đồ ở đầu file. Nhánh trên cho nguồn là cả màn hình
// (gốc là rect monitor), nhánh dưới cho nguồn là một cửa sổ (gốc là client rect).
//
// Dùng (w-1) và 65535 làm mẫu số/tử số để hai đầu mút khớp chính xác: giá trị 65535
// phải rơi đúng vào pixel cuối cùng, không hụt một pixel. InputCapture chuẩn hoá
// theo đúng công thức nghịch đảo.
void InputInjector::SendMoveAbsolute(int32_t nx, int32_t ny) {
    POINT pt{};
    if (monitor_) {
        // Nguồn là cả màn hình: gốc tọa độ là rect của monitor trên desktop ảo.
        MONITORINFO mi{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(monitor_, &mi)) return;
        const int w = mi.rcMonitor.right - mi.rcMonitor.left;
        const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        if (w <= 1 || h <= 1) return;
        pt.x = mi.rcMonitor.left + LONG(int64_t(nx) * (w - 1) / 65535);
        pt.y = mi.rcMonitor.top + LONG(int64_t(ny) * (h - 1) / 65535);
    } else {
        RECT rc{};
        if (!GetClientRect(target_, &rc)) return;
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 1 || h <= 1) return;

        // Chuẩn hóa (khung hình client nhìn thấy) -> pixel trong client rect đích.
        pt.x = LONG(int64_t(nx) * (w - 1) / 65535);
        pt.y = LONG(int64_t(ny) * (h - 1) / 65535);
        if (!ClientToScreen(target_, &pt)) return;
    }

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    ScreenToVirtualDesk(pt.x, pt.y, in.mi.dx, in.mi.dy);
    SendInput(1, &in, sizeof(INPUT));
}

void InputInjector::SendMoveRelative(int32_t dx, int32_t dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE; // không ABSOLUTE = delta, dùng cho game FPS
    in.mi.dx = dx;
    in.mi.dy = dy;
    SendInput(1, &in, sizeof(INPUT));
}

// Chốt an toàn số 1 (xem InputInjector.h). Gọi trước MỌI lần bơm.
//
// Hàm này có TÁC DỤNG PHỤ có chủ ý: lần đầu phát hiện mất focus, nó gọi ReleaseAll.
// Bắt buộc — người ngồi ở máy host alt-tab đi trong lúc client đang giữ phím W thì
// sự kiện nhả sẽ bị bỏ qua ở các vòng sau, và phím kẹt lại vĩnh viễn.
// hadFocus_ chỉ để log một lần mỗi lần đổi trạng thái, không phải mỗi event.
bool InputInjector::TargetHasFocus() {
    // Chia sẻ cả màn hình: không có ứng dụng nào "ngoài phạm vi chia sẻ" để bảo vệ.
    if (monitor_) return true;
    const HWND fg = GetForegroundWindow();
    // Cửa sổ đích hoặc cửa sổ con/popup của nó (hộp thoại, menu) đều tính là đúng.
    const bool ok = fg && (fg == target_ || GetAncestor(fg, GA_ROOT) == target_);
    if (ok != hadFocus_) {
        hadFocus_ = ok;
        if (ok) {
            std::printf("[Inject] Shared window regained focus - input is active again.\n");
        } else {
            std::printf(
                "[Inject] Shared window LOST focus - ignoring input from client for now "
                "(avoid injecting into another application).\n");
            ReleaseAll(); // đang giữ W mà mất focus -> nhả ra, không để kẹt phím
        }
    }
    return ok;
}

void InputInjector::Apply(const deskhub::InputEvent& e) {
    if (!enabled_) return;
    if (!monitor_ && (!target_ || !IsWindow(target_))) return;
    if (!TargetHasFocus()) {
        ++skipped_;
        return;
    }

    // Chốt "HOST THẮNG" (chốt an toàn thứ ba, xem InputInjector.h): người ngồi
    // tại máy vừa động chuột/phím THẬT thì input từ xa nhường trong ~1s — hai
    // bên cùng thao tác thì người tại máy được ưu tiên, hết cảnh giằng con trỏ
    // và lây phím bổ trợ chéo. LocalInputMonitor đã lọc input tự bơm (cờ
    // injected) nên không có vòng tự-khoá; monitor không chạy thì mốc = 0 và
    // chốt này tự tắt. Vào trạng thái nhường là ReleaseAll ngay — remote đang
    // giữ phím mà bị nhường thì phím phải được nhả, không để kẹt.
    const uint64_t lastLocal = LocalInputMonitor::LastPhysicalUs();
    if (lastLocal && NowUs() - lastLocal < kHostWinsGraceUs) {
        if (!localSuppressed_) {
            localSuppressed_ = true;
            std::printf(
                "[Inject] Local user is active - pausing remote input (host wins).\n");
            ReleaseAll();
        }
        ++skipped_;
        return;
    }
    if (localSuppressed_) {
        localSuppressed_ = false;
        std::printf("[Inject] Local user idle - remote input resumed.\n");
    }
    ++applied_;

    switch (e.type) {
        case deskhub::InputType::Key: {
            const bool down = e.state != 0;
            // Nhớ theo scancode để ReleaseAll nhả đúng phím đã bơm.
            if (down)
                keysDown_[e.b] = e.a;
            else
                keysDown_.erase(e.b);
            SendKey(e.a, e.b, down);
            break;
        }
        case deskhub::InputType::MouseMove:
            if (e.absolute)
                SendMoveAbsolute(e.a, e.b);
            else
                SendMoveRelative(e.a, e.b);
            break;
        case deskhub::InputType::MouseButton: {
            const auto btn = deskhub::MouseButton(e.a);
            const bool down = e.state != 0;
            if (down)
                buttonsDown_.insert(btn);
            else
                buttonsDown_.erase(btn);
            SendButton(btn, down);
            break;
        }
        case deskhub::InputType::MouseWheel: {
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
        std::printf("[InjectTest] Invalid target window.\n");
        return 1;
    }
    Sleep(1000); // chờ cửa sổ lên foreground + game về trạng thái ổn định
    // KHÔNG bơm mù: nếu cửa sổ đích chưa ở foreground, phím sẽ rơi vào ứng dụng
    // đang foreground (terminal, trình duyệt...) - gõ bay vào đúng nơi không ngờ.
    if (!inj.TargetHasFocus()) {
        std::printf(
            "[InjectTest] STOP: target window is not foreground. Click that window "
            "then run again (input would be typed into whatever app is open).\n");
        return 1;
    }
    std::printf("[InjectTest] Target window is foreground - starting to inject \"%s\".\n", text);

    // Dùng đúng đường mà client sẽ gửi: vk + scancode lấy từ bản đồ bàn phím
    // hiện hành, KHÔNG dùng ký tự - giống hệt event thật từ Raw Input.
    for (const char* p = text; *p; ++p) {
        const SHORT vkAll = VkKeyScanA(*p);
        if (vkAll == -1) continue;
        const int32_t vk = vkAll & 0xFF;
        const bool needShift = (vkAll >> 8) & 1;
        const int32_t scan = int32_t(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC));
        if (!scan) continue;

        deskhub::InputEvent e;
        e.type = deskhub::InputType::Key;
        if (needShift) {
            e.a = VK_SHIFT;
            e.b = int32_t(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
            e.state = 1;
            inj.Apply(e);
        }
        e.a = vk;
        e.b = scan;
        e.state = 1;
        inj.Apply(e);
        Sleep(40); // game đọc bàn phím theo frame - nhấn/nhả quá nhanh sẽ bị bỏ qua
        e.state = 0;
        inj.Apply(e);
        if (needShift) {
            e.a = VK_SHIFT;
            e.b = int32_t(MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC));
            e.state = 0;
            inj.Apply(e);
        }
        Sleep(15);
    }
    inj.ReleaseAll();
    std::printf(
        "[InjectTest] Injected %llu events (skipped %llu due to lost focus). "
        "Check the target window.\n",
        (unsigned long long)inj.applied(), (unsigned long long)inj.skipped());
    return inj.applied() ? 0 : 1;
}

// Chốt an toàn số 2: nhả sạch mọi thứ đang giữ. Gọi khi mất kết nối (BYE/timeout),
// khi mất focus, và khi kết thúc phiên.
//
// Không kiểm tra TargetHasFocus ở đây — cố tình. Đúng lúc cần nhả nhất chính là
// lúc đã mất focus; đòi có focus mới cho nhả thì phím sẽ kẹt vĩnh viễn.
void InputInjector::ReleaseAll() {
    if (keysDown_.empty() && buttonsDown_.empty()) return;
    std::printf("[Inject] Releasing %zu keys + %zu mouse buttons still held.\n",
        keysDown_.size(), buttonsDown_.size());
    for (const auto& [scan, vk] : keysDown_) SendKey(vk, scan, false);
    for (auto btn : buttonsDown_) SendButton(btn, false);
    keysDown_.clear();
    buttonsDown_.clear();
}
