#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "WindowPickerDialog.h"

#include <string>
#include <vector>

#include "WindowFinder.h"

namespace {

constexpr wchar_t kWndClass[] = L"RemoteGameWindowPicker";

constexpr int kIdList     = 100;
constexpr int kIdRefresh  = 101;
constexpr int kIdChkAllow = 102;
constexpr int kIdOk       = 103;
constexpr int kIdCancel   = 104;

// Format giong het menu console cu: "[exe] title (WxH)" / "(thu nho)".
std::wstring FormatEntry(const WindowInfo& w) {
    wchar_t buf[400];
    if (w.minimized) {
        swprintf(buf, 400, L"[%ls] %ls (thu nho)", w.exeName.c_str(), w.title.c_str());
    } else {
        swprintf(buf, 400, L"[%ls] %ls (%ux%u)", w.exeName.c_str(), w.title.c_str(),
                 w.width, w.height);
    }
    return buf;
}

struct PickerState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND chkAllow = nullptr;
    std::vector<WindowInfo> windows;
    HWND result = nullptr;
    bool allowInput = true;
    bool done = false;
};

void Repopulate(PickerState& st) {
    st.windows = ListCapturableWindows();
    SendMessageW(st.list, LB_RESETCONTENT, 0, 0);
    if (st.windows.empty()) {
        SendMessageW(st.list, LB_ADDSTRING, 0, (LPARAM)L"(Khong tim thay cua so nao de chia se)");
        EnableWindow(GetDlgItem(st.hwnd, kIdOk), FALSE);
    } else {
        for (const auto& w : st.windows)
            SendMessageW(st.list, LB_ADDSTRING, 0, (LPARAM)FormatEntry(w).c_str());
        SendMessageW(st.list, LB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(st.hwnd, kIdOk), TRUE);
    }
}

void Confirm(PickerState& st) {
    const LRESULT sel = SendMessageW(st.list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || (size_t)sel >= st.windows.size()) return; // khong chon gi
    st.result = st.windows[(size_t)sel].hwnd;
    st.allowInput = SendMessageW(st.chkAllow, BM_GETCHECK, 0, 0) == BST_CHECKED;
    st.done = true;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (PickerState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case kIdRefresh: Repopulate(*st); return 0;
        case kIdOk: Confirm(*st); return 0;
        case kIdCancel: st->done = true; return 0;
        case kIdList:
            if (HIWORD(wp) == LBN_DBLCLK) { Confirm(*st); return 0; }
            break;
        }
        break;
    case WM_CLOSE:
        if (st) st->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool ShowWindowPickerDialog(HWND owner, HWND& outTarget, bool& outAllowInput) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc); // lan 2 tra ALREADY_EXISTS - khong sao

    constexpr int kW = 480, kH = 380;
    RECT ownerRect{};
    if (owner) GetWindowRect(owner, &ownerRect);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - kW) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kH) / 2;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRect(&wr, style, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kWndClass, L"Chon cua so de chia se",
                                style, x, y, wr.right - wr.left, wr.bottom - wr.top,
                                owner, nullptr, wc.hInstance, nullptr);
    if (!dlg) return false;

    PickerState st;
    st.hwnd = dlg;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&st);

    const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, s | WS_CHILD | WS_VISIBLE, cx, cy, cw, ch,
                                  dlg, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    st.list = mk(L"LISTBOX", nullptr,
                 LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
                 12, 12, kW - 24, kH - 100, kIdList);
    st.chkAllow = mk(L"BUTTON", L"Cho phep nguoi kia dieu khien chuot/ban phim",
                      BS_AUTOCHECKBOX, 12, kH - 82, kW - 24, 20, kIdChkAllow);
    SendMessageW(st.chkAllow, BM_SETCHECK, BST_CHECKED, 0);
    mk(L"BUTTON", L"Lam moi", 0, 12, kH - 52, 90, 26, kIdRefresh);
    mk(L"BUTTON", L"Chon", BS_DEFPUSHBUTTON, kW - 24 - 180, kH - 52, 86, 26, kIdOk);
    mk(L"BUTTON", L"Huy", 0, kW - 24 - 88, kH - 52, 86, 26, kIdCancel);

    Repopulate(st);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    MSG msg;
    BOOL got;
    while (!st.done && (got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (got == -1) break;
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    DestroyWindow(dlg);

    outTarget = st.result;
    outAllowInput = st.allowInput;
    return st.result != nullptr;
}
