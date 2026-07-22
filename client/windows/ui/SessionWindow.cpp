// =============================================================================
// SessionWindow.cpp — cài đặt cửa sổ quản lý phiên chia sẻ (xem vai trò + mô
// hình luồng ở SessionWindow.h).
//
// KHUÔN MẪU WIN32 giống MainMenuWindow.cpp: tự tạo control bằng CreateWindowExW,
// id số nguyên, WndProc rẽ nhánh theo WM_COMMAND. Khác một điểm: cửa sổ này chạy
// trên thread riêng với vòng bơm message của chính nó (ThreadMain), vì thread
// chính của RunAgent bận chặn ở recvfrom.
//
// LB_SETITEMDATA GIỮ sourceId
//   Danh sách đổ lại mỗi khi dirty; muốn giữ nguyên dòng đang chọn thì phải nhớ
//   "chọn nguồn NÀO" chứ không phải "chọn dòng THỨ MẤY" (thứ tự có thể đổi khi
//   nguồn bị tắt). sourceId nhét thẳng vào item data của từng dòng.
//
// LIÊN QUAN: ui/SessionWindow.h, ui/WindowPickerDialog.h (picker khi bấm Add),
//            AgentLoop.cpp (đầu kia của hộp thư)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ui/SessionWindow.h"

#include "ui/WindowPickerDialog.h"

namespace {

constexpr wchar_t kWndClass[] = L"DeskhubSessionWindow";

constexpr int kIdList = 100;
constexpr int kIdAdd = 101;
constexpr int kIdStopSel = 102;
constexpr int kIdStopAll = 103;

constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 300;           // nhịp UI chép rows_ khi dirty
constexpr UINT WM_APP_QUIT = WM_APP + 1; // Stop() (thread Recv) yêu cầu đóng cửa sổ

} // namespace

void SessionWindow::Start(uint16_t port, size_t maxSources) {
    port_ = port;
    maxSources_ = maxSources;
    thread_ = std::thread(&SessionWindow::ThreadMain, this);
}

void SessionWindow::Stop() {
    quitReq_.store(true, std::memory_order_release);
    if (HWND h = hwnd_.load(std::memory_order_acquire))
        PostMessageW(h, WM_APP_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
    active_.store(false, std::memory_order_release);
}

void SessionWindow::SetRows(std::vector<SessionSourceRow> rows) {
    std::lock_guard<std::mutex> lk(m_);
    if (rows == rows_) return;
    rows_ = std::move(rows);
    dirty_ = true;
}

std::vector<AgentSource> SessionWindow::TakeAdds() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<AgentSource> out;
    out.swap(adds_);
    return out;
}

std::vector<uint8_t> SessionWindow::TakeRemoves() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<uint8_t> out;
    out.swap(removes_);
    return out;
}

// Đổ uiRows_ vào listbox, giữ nguyên nguồn đang chọn (theo sourceId, không theo
// vị trí dòng — xem chú thích LB_SETITEMDATA ở đầu file).
void SessionWindow::RefreshList() {
    if (!list_) return;
    LONG_PTR selId = -1;
    const LRESULT cur = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (cur != LB_ERR) selId = (LONG_PTR)SendMessageW(list_, LB_GETITEMDATA, (WPARAM)cur, 0);

    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    if (uiRows_.empty()) {
        SendMessageW(list_, LB_ADDSTRING, 0,
            (LPARAM)L"(nothing is being shared - press Add to pick a source)");
        SendMessageW(list_, LB_SETITEMDATA, 0, (LPARAM)-1);
        return;
    }
    int i = 0;
    for (const auto& r : uiRows_) {
        SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)r.label.c_str());
        SendMessageW(list_, LB_SETITEMDATA, (WPARAM)i, (LPARAM)r.sourceId);
        if ((LONG_PTR)r.sourceId == selId) SendMessageW(list_, LB_SETCURSEL, (WPARAM)i, 0);
        ++i;
    }
}

LRESULT SessionWindow::HandleMsg(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER: {
            if (wp != kTimerId) break;
            // Chép rows_ ra bản UI khi có thay đổi; giữ mutex NGẮN, không đụng
            // listbox trong lúc khoá.
            bool need = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (dirty_) {
                    uiRows_ = rows_;
                    dirty_ = false;
                    need = true;
                }
            }
            if (need) RefreshList();
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case kIdAdd: {
                    if (uiRows_.size() >= maxSources_) {
                        wchar_t m[128];
                        swprintf(m, 128, L"At most %zu sources can be shared at once.",
                            maxSources_);
                        MessageBoxW(h, m, L"Deskhub", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    // Picker modal trên chính thread UI này; vòng Recv vẫn chạy
                    // bình thường trong lúc hộp thoại mở.
                    std::vector<AgentSource> picked;
                    if (ShowWindowPickerAddDialog(h, picked)) {
                        std::lock_guard<std::mutex> lk(m_);
                        for (auto& s : picked) adds_.push_back(std::move(s));
                    }
                    return 0;
                }
                case kIdStopSel: {
                    const LRESULT cur = SendMessageW(list_, LB_GETCURSEL, 0, 0);
                    if (cur == LB_ERR || uiRows_.empty()) {
                        MessageBoxW(h, L"Select a source in the list first.",
                            L"Deskhub", MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    const LONG_PTR id = (LONG_PTR)SendMessageW(list_, LB_GETITEMDATA, (WPARAM)cur, 0);
                    if (id >= 0) {
                        std::lock_guard<std::mutex> lk(m_);
                        removes_.push_back(uint8_t(id));
                    }
                    return 0;
                }
                case kIdStopAll:
                    // Ẩn ngay cho có phản hồi tức thì; vòng Recv thấy cờ trong
                    // ~100ms sẽ dọn phiên và gọi Stop() đóng hẳn cửa sổ.
                    stopReq_.store(true, std::memory_order_release);
                    ShowWindow(h, SW_HIDE);
                    return 0;
            }
            break;
        }
        case WM_CLOSE:
            // Đóng cửa sổ phiên = kết thúc chia sẻ (giống Stop sharing).
            stopReq_.store(true, std::memory_order_release);
            ShowWindow(h, SW_HIDE);
            return 0;
        case WM_APP_QUIT:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CALLBACK SessionWindow::WndProcThunk(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = (SessionWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->HandleMsg(h, msg, wp, lp);
}

void SessionWindow::ThreadMain() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc); // lần 2 trả ALREADY_EXISTS - không sao

    constexpr int kW = 460, kH = 330;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    // Góc dưới-phải vùng làm việc: không đè lên giữa màn hình đang được chia sẻ.
    const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT wr{0, 0, kW, kH};
    AdjustWindowRect(&wr, style, FALSE);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    wchar_t title[96];
    swprintf(title, 96, L"Deskhub - sharing (port %u)", unsigned(port_));
    HWND hwnd = CreateWindowExW(0, kWndClass, title, style,
        wa.right - ww - 24, wa.bottom - wh - 24, ww, wh,
        nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) return; // active_ giữ false — AgentLoop rơi về hành vi cũ

    const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, s | WS_CHILD | WS_VISIBLE, cx, cy, cw, ch,
            hwnd, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    mk(L"STATIC", L"Sources currently being shared:", SS_LEFT, 12, 10, kW - 24, 16, 0);
    list_ = mk(L"LISTBOX", nullptr,
        LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
        12, 30, kW - 24, kH - 116, kIdList);
    wchar_t hint[128];
    swprintf(hint, 128, L"Others connect to this machine on UDP port %u.", unsigned(port_));
    mk(L"STATIC", hint, SS_LEFT, 12, kH - 78, kW - 24, 16, 0);
    mk(L"BUTTON", L"Add...", BS_PUSHBUTTON, 12, kH - 50, 120, 28, kIdAdd);
    mk(L"BUTTON", L"Stop selected", BS_PUSHBUTTON, 140, kH - 50, 120, 28, kIdStopSel);
    mk(L"BUTTON", L"Stop sharing", BS_PUSHBUTTON, kW - 12 - 130, kH - 50, 130, 28, kIdStopAll);

    RefreshList();
    SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    hwnd_.store(hwnd, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    // Stop() có thể đã chạy trong lúc cửa sổ đang được tạo (hwnd_ còn null lúc
    // đó nên message đóng không gửi được) — kiểm tra cờ để không treo join().
    if (quitReq_.load(std::memory_order_acquire)) DestroyWindow(hwnd);

    MSG msg;
    BOOL got;
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (got == -1) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    hwnd_.store(nullptr, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    list_ = nullptr;
}
