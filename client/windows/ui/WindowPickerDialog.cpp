// =============================================================================
// WindowPickerDialog.cpp — cài đặt hộp thoại chọn nguồn chia sẻ phía host.
//
// KHUÔN MẪU "HỘP THOẠI MODAL TỰ DỰNG"
//   Giống hệt SourcePickerDialog.cpp: tự tạo cửa sổ, vô hiệu hoá cửa sổ cha, chạy
//   vòng lặp message riêng tới khi xong. Xem giải thích đầy đủ ở file đó.
//
// DỰNG DANH SÁCH: HAI NGUỒN GỘP LÀM MỘT
//   CollectMonitor (qua EnumDisplayMonitors) cho các màn hình, rồi
//   ListCapturableWindows() cho các cửa sổ. Cả hai đổ vào cùng một vector Entry,
//   mỗi Entry mang một CaptureTarget — kiểu đó vốn đã bọc được cả HWND lẫn
//   HMONITOR nên phần còn lại của chương trình không phải phân biệt.
//
// BA CHUỖI CHO MỘT NGUỒN, đừng nhầm lẫn
//   Entry::label — hiện trong listbox, có kèm kích thước cho người dùng dễ nhận ra.
//   Entry::name  — tên GỬI CHO CLIENT, không kèm kích thước (client tự biết).
//   ToUtf8       — đổi name sang UTF-8 trước khi lên dây, vì client có thể không
//                  phải máy Windows. Hàm đối xứng FromUtf8 ở SourcePickerDialog.cpp.
//
// LIÊN QUAN: ui/WindowPickerDialog.h (vai trò + lý do gộp checkbox),
//            capture/WindowFinder.h (nguồn danh sách), AgentLoop.h (AgentSource)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ui/WindowPickerDialog.h"

#include <string>
#include <vector>

#include "capture/WindowFinder.h"
#include "rgc/wire/Wire.h" // kMaxSources

namespace {

constexpr wchar_t kWndClass[] = L"RemoteGameWindowPicker";

constexpr int kIdList     = 100;
constexpr int kIdRefresh  = 101;
constexpr int kIdChkAllow = 102;
constexpr int kIdOk       = 103;
constexpr int kIdCancel   = 104;
constexpr int kIdHint     = 105;

// Tên nguồn đi trên dây là UTF-8 (client có thể không phải Windows).
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Một dòng trong danh sách: nguồn kèm nhãn hiển thị.
struct Entry {
    CaptureTarget target;
    std::wstring  label; // hiện trong listbox
    std::wstring  name;  // tên gửi cho client (không kèm kích thước)
};

BOOL CALLBACK CollectMonitor(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* out = (std::vector<Entry>*)lp;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, (MONITORINFO*)&mi)) return TRUE;

    const int w = mi.rcMonitor.right - mi.rcMonitor.left;
    const int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

    wchar_t name[128];
    swprintf(name, 128, L"Screen %zu%ls", out->size() + 1, primary ? L" (primary)" : L"");
    wchar_t label[200];
    swprintf(label, 200, L"[screen] %ls (%dx%d)", name, w, h);

    out->push_back(Entry{CaptureTarget::Monitor(mon), label, name});
    return TRUE;
}

std::vector<Entry> BuildEntries() {
    std::vector<Entry> entries;
    // Màn hình lên trước: "chia sẻ cả màn hình" là lựa chọn hay dùng nhất.
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, (LPARAM)&entries);

    for (const auto& w : ListCapturableWindows()) {
        wchar_t label[400];
        if (w.minimized) {
            swprintf(label, 400, L"[%ls] %ls (minimized)", w.exeName.c_str(), w.title.c_str());
        } else {
            swprintf(label, 400, L"[%ls] %ls (%ux%u)", w.exeName.c_str(), w.title.c_str(),
                     w.width, w.height);
        }
        entries.push_back(Entry{CaptureTarget::Window(w.hwnd), label, w.title});
    }
    return entries;
}

struct PickerState {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND chkAllow = nullptr;
    std::vector<Entry> entries;
    std::vector<AgentSource> result;
    bool allowInput = true;
    bool done = false;
};

void Repopulate(PickerState& st) {
    st.entries = BuildEntries();
    SendMessageW(st.list, LB_RESETCONTENT, 0, 0);
    if (st.entries.empty()) {
        SendMessageW(st.list, LB_ADDSTRING, 0, (LPARAM)L"(Nothing found to share)");
        EnableWindow(GetDlgItem(st.hwnd, kIdOk), FALSE);
        return;
    }
    for (const auto& e : st.entries)
        SendMessageW(st.list, LB_ADDSTRING, 0, (LPARAM)e.label.c_str());
    // Không chọn sẵn gì: buộc người dùng tự tick đúng thứ muốn chia sẻ thay vì vô ý
    // chia sẻ nhầm màn hình primary khi bấm Share ngay.
    EnableWindow(GetDlgItem(st.hwnd, kIdOk), TRUE);
}

void Confirm(PickerState& st) {
    const LRESULT count = SendMessageW(st.list, LB_GETSELCOUNT, 0, 0);
    if (count <= 0 || st.entries.empty()) return; // chưa chọn gì

    std::vector<int> sel(static_cast<size_t>(count), 0);
    SendMessageW(st.list, LB_GETSELITEMS, WPARAM(count), (LPARAM)sel.data());

    st.result.clear();
    for (int i : sel) {
        if (i < 0 || size_t(i) >= st.entries.size()) continue;
        const Entry& e = st.entries[size_t(i)];
        st.result.push_back(AgentSource{e.target, ToUtf8(e.name)});
    }
    if (st.result.empty()) return;

    // Mỗi nguồn là một pipeline capture+encode riêng - quá nhiều thì GPU không kham
    // nổi và kMaxSources cũng là trần của SOURCE_LIST trong một datagram.
    if (st.result.size() > rgc::kMaxSources) {
        wchar_t msg[160];
        swprintf(msg, 160, L"Please select at most %zu sources (you selected %zu).",
                 rgc::kMaxSources, st.result.size());
        MessageBoxW(st.hwnd, msg, L"RemoteGame", MB_OK | MB_ICONWARNING);
        st.result.clear();
        return;
    }

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

bool ShowWindowPickerDialog(HWND owner, std::vector<AgentSource>& outSources,
                            bool& outAllowInput) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc); // lần 2 trả ALREADY_EXISTS - không sao

    constexpr int kW = 520, kH = 420;
    RECT ownerRect{};
    if (owner) GetWindowRect(owner, &ownerRect);
    else SystemParametersInfoW(SPI_GETWORKAREA, 0, &ownerRect, 0);
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - kW) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - kH) / 2;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRect(&wr, style, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kWndClass,
                                L"Select what to share (screens and/or windows)",
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

    // LBS_MULTIPLESEL: click là bật/tắt một dòng. Dễ hiểu hơn LBS_EXTENDEDSEL
    // (đòi giữ Ctrl) với thao tác "tick những thứ muốn chia sẻ".
    st.list = mk(L"LISTBOX", nullptr,
                 LBS_NOTIFY | LBS_HASSTRINGS | LBS_MULTIPLESEL | WS_VSCROLL | WS_BORDER,
                 12, 12, kW - 24, kH - 122, kIdList);
    mk(L"STATIC", L"Click each screen or window you want to share (you can pick several).",
       0, 12, kH - 104, kW - 24, 18, kIdHint);
    st.chkAllow = mk(L"BUTTON", L"Allow the other person to control mouse/keyboard",
                      BS_AUTOCHECKBOX, 12, kH - 82, kW - 24, 20, kIdChkAllow);
    SendMessageW(st.chkAllow, BM_SETCHECK, BST_CHECKED, 0);
    mk(L"BUTTON", L"Refresh", 0, 12, kH - 52, 90, 26, kIdRefresh);
    mk(L"BUTTON", L"Share", BS_DEFPUSHBUTTON, kW - 24 - 180, kH - 52, 86, 26, kIdOk);
    mk(L"BUTTON", L"Cancel", 0, kW - 24 - 88, kH - 52, 86, 26, kIdCancel);

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

    outSources = std::move(st.result);
    outAllowInput = st.allowInput;
    return !outSources.empty();
}
