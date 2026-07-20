#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MainMenuWindow.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "AgentLoop.h"
#include "ClientLoop.h"
#include "NetInfo.h"
#include "SourcePickerDialog.h"
#include "UdpSocket.h"
#include "WindowPickerDialog.h"

namespace {

constexpr wchar_t kWndClass[] = L"RemoteGameMainMenu";

constexpr int kIdEditPort    = 200;
constexpr int kIdEditFps     = 199;
constexpr int kIdEditBitrate = 198;
constexpr int kIdShare       = 201;
constexpr int kIdEditAddr    = 202;
constexpr int kIdConnect     = 203;
constexpr int kIdChkViewOnly = 204;
constexpr int kIdExit        = 205;

// Giá trị mặc định khi ô trống/nhập sai - giống hệt mặc định --port/--fps/
// --bitrate cũ.
constexpr uint16_t kDefaultPort = 47777;
constexpr uint32_t kDefaultFps = 60;
constexpr uint32_t kDefaultBitrateMbps = 20;

std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\r' || s.back() == L'\n' || s.back() == L'\t'))
        s.pop_back();
    const size_t b = s.find_first_not_of(L" \t");
    return b == std::wstring::npos ? std::wstring() : s.substr(b);
}

uint32_t GetEditUint(HWND edit, uint32_t fallback) {
    wchar_t buf[16] = {};
    GetWindowTextW(edit, buf, 16);
    const int v = _wtoi(buf);
    return v > 0 ? (uint32_t)v : fallback;
}

struct MenuState {
    HWND hwnd = nullptr;
    HWND editPort = nullptr;
    HWND editFps = nullptr;
    HWND editBitrate = nullptr;
    HWND editAddr = nullptr;
    HWND chkViewOnly = nullptr;
    bool quit = false;
};

void DoShare(MenuState& st) {
    std::vector<AgentSource> sources;
    bool allow = true;
    if (!ShowWindowPickerDialog(st.hwnd, sources, allow)) return;

    AgentOptions ao;
    ao.port = uint16_t(GetEditUint(st.editPort, kDefaultPort));
    ao.fps = GetEditUint(st.editFps, kDefaultFps);
    ao.bitrateMbps = GetEditUint(st.editBitrate, kDefaultBitrateMbps);
    ao.allowInput = allow;

    ShowWindow(st.hwnd, SW_HIDE);
    RunAgent(sources, ao);
    ShowWindow(st.hwnd, SW_SHOW);
    SetForegroundWindow(st.hwnd);
}

void DoConnect(MenuState& st) {
    wchar_t buf[128] = {};
    GetWindowTextW(st.editAddr, buf, 128);
    const std::wstring waddr = Trim(buf);
    if (waddr.empty()) {
        MessageBoxW(st.hwnd, L"Enter the host machine's IP address first (e.g., 192.168.1.10).",
                    L"RemoteGame", MB_OK | MB_ICONWARNING);
        return;
    }
    // Địa chỉ ip[:port] chỉ gồm ASCII - thu hẹp từng ký tự là an toàn.
    std::string addr;
    addr.reserve(waddr.size());
    for (wchar_t c : waddr) addr.push_back(char(c));

    const uint16_t port = uint16_t(GetEditUint(st.editPort, kDefaultPort));
    ClientOptions co;
    if (!ParseNetAddr(addr, port, co.server)) {
        const std::wstring msg = L"Invalid address: \"" + waddr +
            L"\"\n(e.g., 192.168.1.10 or 192.168.1.10:47777)";
        MessageBoxW(st.hwnd, msg.c_str(), L"RemoteGame", MB_OK | MB_ICONERROR);
        return;
    }
    co.saveBmp = false;
    co.sendInput = SendMessageW(st.chkViewOnly, BM_GETCHECK, 0, 0) != BST_CHECKED;

    // Hỏi host đang chia sẻ những gì rồi cho chọn. Host bản cũ (hoặc sai IP /
    // firewall chặn) không trả lời -> cứ thử nguồn 0, ClientSession sẽ báo lỗi
    // kết nối cụ thể hơn nhiều so với một hộp thoại "không thấy host".
    std::vector<rgc::SourceInfo> available;
    if (QueryHostSources(co.server, available) && !available.empty()) {
        if (!ShowSourcePickerDialog(st.hwnd, available, co.sources)) return;
    }

    ShowWindow(st.hwnd, SW_HIDE);
    RunClient(co);
    ShowWindow(st.hwnd, SW_SHOW);
    SetForegroundWindow(st.hwnd);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (MenuState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case kIdShare:   DoShare(*st);   return 0;
        case kIdConnect: DoConnect(*st); return 0;
        case kIdExit:    st->quit = true; return 0;
        }
        break;
    case WM_CLOSE:
        if (st) st->quit = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

int RunMainMenuWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    constexpr int kW = 460, kH = 400;
    const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRect(&wr, style, FALSE);
    HWND hwnd = CreateWindowExW(0, kWndClass, L"RemoteGame - stream & remotely control an application",
                                 style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 wr.right - wr.left, wr.bottom - wr.top,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    MenuState st;
    st.hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);

    const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, s | WS_CHILD | WS_VISIBLE, cx, cy, cw, ch,
                                  hwnd, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    // Địa chỉ IP máy này - mỗi adapter một dòng, kèm port để đọc cho máy kia gõ.
    // Dòng port hiển thị lấy giá trị mặc định ban đầu; sửa ở Port ở dưới sẽ áp
    // dụng cho phiên Chia sẻ/Kết nối tiếp theo (không tự cập nhật dòng này).
    std::wstring addrText = L"THIS MACHINE's address (read aloud for the other person to type):\r\n";
    const auto addrs = ListLocalIPv4();
    if (addrs.empty()) {
        addrText += L"(no network address found)";
    } else {
        for (const auto& a : addrs) {
            wchar_t line[160];
            std::wstring wip(a.ip.begin(), a.ip.end());
            swprintf(line, 160, L"  %-22ls %ls:%u\r\n", a.name.c_str(), wip.c_str(), kDefaultPort);
            addrText += line;
        }
    }
    mk(L"STATIC", addrText.c_str(), SS_LEFT, 16, 12, kW - 32, 92, 0);

    // Port / FPS / Bitrate - trước đây chỉ sửa được bằng cờ dòng lệnh.
    mk(L"STATIC", L"Port", SS_LEFT, 16, 118, 40, 20, 0);
    st.editPort = mk(L"EDIT", L"47777", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                      54, 116, 70, 24, kIdEditPort);
    mk(L"STATIC", L"FPS", SS_LEFT, 144, 118, 32, 20, 0);
    st.editFps = mk(L"EDIT", L"60", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                     176, 116, 50, 24, kIdEditFps);
    mk(L"STATIC", L"Bitrate (Mbps)", SS_LEFT, 244, 118, 92, 20, 0);
    st.editBitrate = mk(L"EDIT", L"20", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                         338, 116, 50, 24, kIdEditBitrate);

    mk(L"BUTTON", L"Share an application on this machine (act as host)", BS_PUSHBUTTON,
       16, 156, kW - 32, 32, kIdShare);

    mk(L"STATIC", L"Connect to another machine (ip[:port]):", SS_LEFT, 16, 204, kW - 32, 18, 0);
    st.editAddr = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 16, 224, kW - 32 - 110, 26, kIdEditAddr);
    mk(L"BUTTON", L"Connect", BS_DEFPUSHBUTTON, kW - 16 - 100, 224, 100, 26, kIdConnect);

    st.chkViewOnly = mk(L"BUTTON", L"View only, don't send input (applies when connecting)",
                         BS_AUTOCHECKBOX, 16, 262, kW - 32, 20, kIdChkViewOnly);

    mk(L"BUTTON", L"Exit", BS_PUSHBUTTON, 16, kH - 60, 100, 28, kIdExit);

    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    BOOL got;
    while (!st.quit && (got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (got == -1) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    DestroyWindow(hwnd);
    return 0;
}
