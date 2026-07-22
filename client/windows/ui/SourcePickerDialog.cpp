// =============================================================================
// SourcePickerDialog.cpp — cài đặt hộp thoại chọn nguồn phía client.
//
// KHUÔN MẪU "HỘP THOẠI MODAL TỰ DỰNG"
//   Không dùng DialogBox của Win32 (đòi file tài nguyên .rc) mà tự tạo cửa sổ rồi
//   chạy vòng lặp message riêng cho tới khi `done`. Ba bước:
//     1. EnableWindow(owner, FALSE) — vô hiệu hoá cửa sổ cha, tạo cảm giác modal.
//     2. Vòng lặp message riêng chạy tới khi người dùng bấm OK/Huỷ.
//     3. EnableWindow(owner, TRUE) và trả kết quả.
//   WindowPickerDialog.cpp dùng đúng khuôn này — sửa một bên nên xem lại bên kia.
//
// FromUtf8 — VÌ SAO CẦN
//   Tên nguồn đi trên dây là UTF-8 (host có thể phục vụ client không phải Windows),
//   còn control Win32 cần UTF-16. Đây là chỗ đổi. Hàm đối xứng ToUtf8 nằm ở
//   WindowPickerDialog.cpp, nơi tên đi theo chiều ngược lại.
//
// LIÊN QUAN: ui/SourcePickerDialog.h (vai trò), ui/WindowPickerDialog.cpp (cùng
//            khuôn mẫu, chiều ngược lại), deskhub/wire/Wire.h (SourceInfo)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ui/SourcePickerDialog.h"

#include <string>

namespace {

constexpr wchar_t kWndClass[] = L"DeskhubSourcePicker";

constexpr int kIdList   = 300;
constexpr int kIdOk     = 301;
constexpr int kIdCancel = 302;
constexpr int kIdHint   = 303;

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

struct State {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    const std::vector<deskhub::SourceInfo>* sources = nullptr;
    std::vector<deskhub::SourceInfo> result;
    bool done = false;
};

void Confirm(State& st) {
    const LRESULT count = SendMessageW(st.list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0) return;

    std::vector<int> sel(static_cast<size_t>(count), 0);
    SendMessageW(st.list, LB_GETSELITEMS, WPARAM(count), (LPARAM)sel.data());

    st.result.clear();
    for (int i : sel)
        if (i >= 0 && size_t(i) < st.sources->size())
            st.result.push_back((*st.sources)[size_t(i)]);
    if (st.result.empty()) return;
    st.done = true;
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (State*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
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

bool ShowSourcePickerDialog(HWND owner, const std::vector<deskhub::SourceInfo>& sources,
                            std::vector<deskhub::SourceInfo>& outSelected) {
    outSelected.clear();
    if (sources.empty()) return false;
    // Host chỉ chia sẻ một thứ: không bắt người dùng bấm thêm một hộp thoại nữa.
    if (sources.size() == 1) {
        outSelected = sources;
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    constexpr int kW = 460, kH = 340;
    RECT ownerRect{};
    if (owner) GetWindowRect(owner, &ownerRect);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - kW) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kH) / 2;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRect(&wr, style, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kWndClass, L"What do you want to view?",
                                style, x, y, wr.right - wr.left, wr.bottom - wr.top,
                                owner, nullptr, wc.hInstance, nullptr);
    if (!dlg) return false;

    State st;
    st.hwnd = dlg;
    st.sources = &sources;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&st);

    const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, s | WS_CHILD | WS_VISIBLE, cx, cy, cw, ch,
                                  dlg, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    st.list = mk(L"LISTBOX", nullptr,
                 LBS_NOTIFY | LBS_HASSTRINGS | LBS_MULTIPLESEL | WS_VSCROLL | WS_BORDER,
                 12, 12, kW - 24, kH - 92, kIdList);
    for (const auto& s : sources) {
        wchar_t line[256];
        swprintf(line, 256, L"%ls (%ux%u)", FromUtf8(s.name).c_str(), s.width, s.height);
        SendMessageW(st.list, LB_ADDSTRING, 0, (LPARAM)line);
    }
    SendMessageW(st.list, LB_SETSEL, TRUE, 0);
    mk(L"STATIC", L"Each one you pick opens its own window.",
       0, 12, kH - 74, kW - 24, 18, kIdHint);
    mk(L"BUTTON", L"View", BS_DEFPUSHBUTTON, kW - 24 - 180, kH - 46, 86, 26, kIdOk);
    mk(L"BUTTON", L"Cancel", 0, kW - 24 - 88, kH - 46, 86, 26, kIdCancel);

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

    outSelected = std::move(st.result);
    return !outSelected.empty();
}
