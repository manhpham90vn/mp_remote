#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MainMenuWindow.h"

#include <cstdlib>
#include <string>

#include "AgentLoop.h"
#include "ClientLoop.h"
#include "NetInfo.h"
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

// Gia tri mac dinh khi o trong/nhap sai - giong het mac dinh --port/--fps/
// --bitrate cu.
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
    HWND target = nullptr;
    bool allow = true;
    if (!ShowWindowPickerDialog(st.hwnd, target, allow)) return;

    AgentOptions ao;
    ao.port = uint16_t(GetEditUint(st.editPort, kDefaultPort));
    ao.fps = GetEditUint(st.editFps, kDefaultFps);
    ao.bitrateMbps = GetEditUint(st.editBitrate, kDefaultBitrateMbps);
    ao.allowInput = allow;

    ShowWindow(st.hwnd, SW_HIDE);
    RunAgent(target, ao);
    ShowWindow(st.hwnd, SW_SHOW);
    SetForegroundWindow(st.hwnd);
}

void DoConnect(MenuState& st) {
    wchar_t buf[128] = {};
    GetWindowTextW(st.editAddr, buf, 128);
    const std::wstring waddr = Trim(buf);
    if (waddr.empty()) {
        MessageBoxW(st.hwnd, L"Nhap dia chi IP cua may host truoc (vi du: 192.168.1.10).",
                    L"RemoteGame", MB_OK | MB_ICONWARNING);
        return;
    }
    // Dia chi ip[:port] chi gom ASCII - thu hep tung ky tu la an toan.
    std::string addr;
    addr.reserve(waddr.size());
    for (wchar_t c : waddr) addr.push_back(char(c));

    const uint16_t port = uint16_t(GetEditUint(st.editPort, kDefaultPort));
    ClientOptions co;
    if (!ParseNetAddr(addr, port, co.server)) {
        const std::wstring msg = L"Dia chi khong hop le: \"" + waddr +
            L"\"\n(vi du: 192.168.1.10 hoac 192.168.1.10:47777)";
        MessageBoxW(st.hwnd, msg.c_str(), L"RemoteGame", MB_OK | MB_ICONERROR);
        return;
    }
    co.saveBmp = false;
    co.sendInput = SendMessageW(st.chkViewOnly, BM_GETCHECK, 0, 0) != BST_CHECKED;

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
    HWND hwnd = CreateWindowExW(0, kWndClass, L"RemoteGame - stream & dieu khien ung dung tu xa",
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

    // Dia chi IP may nay - moi adapter mot dong, kem port de doc cho may kia go.
    // Dong port hien thi lay gia tri mac dinh ban dau; sua o Port o duoi se ap
    // dung cho phien Chia se/Ket noi tiep theo (khong tu cap nhat dong nay).
    std::wstring addrText = L"Dia chi MAY NAY (doc cho nguoi o may kia nhap vao):\r\n";
    const auto addrs = ListLocalIPv4();
    if (addrs.empty()) {
        addrText += L"(khong tim thay dia chi mang nao)";
    } else {
        for (const auto& a : addrs) {
            wchar_t line[160];
            std::wstring wip(a.ip.begin(), a.ip.end());
            swprintf(line, 160, L"  %-22ls %ls:%u\r\n", a.name.c_str(), wip.c_str(), kDefaultPort);
            addrText += line;
        }
    }
    mk(L"STATIC", addrText.c_str(), SS_LEFT, 16, 12, kW - 32, 92, 0);

    // Port / FPS / Bitrate - truoc day chi sua duoc bang co dong lenh.
    mk(L"STATIC", L"Port", SS_LEFT, 16, 118, 40, 20, 0);
    st.editPort = mk(L"EDIT", L"47777", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                      54, 116, 70, 24, kIdEditPort);
    mk(L"STATIC", L"FPS", SS_LEFT, 144, 118, 32, 20, 0);
    st.editFps = mk(L"EDIT", L"60", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                     176, 116, 50, 24, kIdEditFps);
    mk(L"STATIC", L"Bitrate (Mbps)", SS_LEFT, 244, 118, 92, 20, 0);
    st.editBitrate = mk(L"EDIT", L"20", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                         338, 116, 50, 24, kIdEditBitrate);

    mk(L"BUTTON", L"Chia se ung dung tren may nay (lam host)", BS_PUSHBUTTON,
       16, 156, kW - 32, 32, kIdShare);

    mk(L"STATIC", L"Ket noi toi may khac (ip[:port]):", SS_LEFT, 16, 204, kW - 32, 18, 0);
    st.editAddr = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 16, 224, kW - 32 - 110, 26, kIdEditAddr);
    mk(L"BUTTON", L"Ket noi", BS_DEFPUSHBUTTON, kW - 16 - 100, 224, 100, 26, kIdConnect);

    st.chkViewOnly = mk(L"BUTTON", L"Chi xem, khong gui input (ap dung khi Ket noi)",
                         BS_AUTOCHECKBOX, 16, 262, kW - 32, 20, kIdChkViewOnly);

    mk(L"BUTTON", L"Thoat", BS_PUSHBUTTON, 16, kH - 60, 100, 28, kIdExit);

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
