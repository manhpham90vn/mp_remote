// =============================================================================
// ViewerWindow.cpp — cài đặt cửa sổ quản lý phiên xem (vai trò + mô hình danh
// sách ở ViewerWindow.h; khuôn mẫu Win32 giống SessionWindow.cpp — thread UI
// riêng, LB_SETITEMDATA giữ sourceId, đọc giải thích đầy đủ ở file đó).
//
// DANH SÁCH LÀ PHÉP GỘP CỦA HAI NGUỒN DỮ LIỆU
//   hostList_   — host đang chia sẻ gì (kết quả Refresh, có thể cũ vài phút).
//   uiViewRows_ — mình đang xem gì (vòng Main đẩy lên ~500ms một lần).
//   RebuildList đi qua hostList_ và gắn hậu tố "— viewing"/"— connecting..." cho
//   nguồn nào đang mở; nguồn đang xem mà KHÔNG còn trong hostList_ (host vừa tắt
//   nó, hoặc chưa Refresh lần nào) vẫn được liệt kê thêm ở cuối — không thì
//   người dùng không có dòng nào để chọn khi muốn Stop nó.
//
// LIÊN QUAN: ui/ViewerWindow.h, ui/SessionWindow.cpp (bản đối xứng phía host),
//            ClientLoop.h (QueryHostSources)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ui/ViewerWindow.h"

#include "ClientLoop.h" // QueryHostSources

namespace {

constexpr wchar_t kWndClass[] = L"DeskhubViewerWindow";

constexpr int kIdList = 100;
constexpr int kIdRefresh = 101;
constexpr int kIdView = 102;
constexpr int kIdStopSel = 103;
constexpr int kIdDisconnect = 104;

constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 300;              // nhịp UI chép rows_ khi dirty
constexpr UINT WM_APP_QUIT = WM_APP + 1;    // Stop() (thread Main) yêu cầu đóng cửa sổ
constexpr UINT WM_APP_REFRESH = WM_APP + 2; // thread hỏi host báo đã có kết quả

// Tên nguồn đi trên dây là UTF-8; control Win32 cần UTF-16.
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

} // namespace

void ViewerWindow::Start(const NetAddr& server, size_t maxSources) {
    server_ = server;
    maxSources_ = maxSources;
    thread_ = std::thread(&ViewerWindow::ThreadMain, this);
}

void ViewerWindow::Stop() {
    quitReq_.store(true, std::memory_order_release);
    if (HWND h = hwnd_.load(std::memory_order_acquire))
        PostMessageW(h, WM_APP_QUIT, 0, 0);
    if (thread_.joinable()) thread_.join();
    // Join SAU thread UI: từ đây không ai phóng thread hỏi host mới được nữa.
    if (queryThread_.joinable()) queryThread_.join();
    active_.store(false, std::memory_order_release);
}

void ViewerWindow::SetRows(std::vector<SessionSourceRow> rows) {
    std::lock_guard<std::mutex> lk(m_);
    if (rows == rows_) return;
    rows_ = std::move(rows);
    dirty_ = true;
}

std::vector<deskhub::SourceInfo> ViewerWindow::TakeAdds() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<deskhub::SourceInfo> out;
    out.swap(adds_);
    return out;
}

std::vector<uint8_t> ViewerWindow::TakeRemoves() {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<uint8_t> out;
    out.swap(removes_);
    return out;
}

// Gộp hostList_ + uiViewRows_ thành listbox (xem chú thích đầu file), giữ nguyên
// dòng đang chọn theo sourceId (item data).
void ViewerWindow::RebuildList() {
    if (!list_) return;
    LONG_PTR selId = -1;
    const LRESULT cur = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    if (cur != LB_ERR) selId = (LONG_PTR)SendMessageW(list_, LB_GETITEMDATA, (WPARAM)cur, 0);

    // Tìm trạng thái xem của một sourceId trong bản UI của danh sách đang xem.
    auto viewState = [&](uint8_t id) -> const SessionSourceRow* {
        for (const auto& r : uiViewRows_)
            if (r.sourceId == id) return &r;
        return nullptr;
    };

    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    int i = 0;
    auto addLine = [&](const std::wstring& label, uint8_t id) {
        SendMessageW(list_, LB_ADDSTRING, 0, (LPARAM)label.c_str());
        SendMessageW(list_, LB_SETITEMDATA, (WPARAM)i, (LPARAM)id);
        if ((LONG_PTR)id == selId) SendMessageW(list_, LB_SETCURSEL, (WPARAM)i, 0);
        ++i;
    };

    for (const auto& si : hostList_) {
        wchar_t size[32];
        swprintf(size, 32, L"  (%ux%u)", unsigned(si.width), unsigned(si.height));
        std::wstring label = FromUtf8(si.name) + size;
        if (const SessionSourceRow* r = viewState(si.sourceId))
            label += r->pending ? L"  — connecting..." : L"  — viewing";
        addLine(label, si.sourceId);
    }
    // Nguồn đang xem nhưng không (còn) nằm trong danh sách host — vẫn phải hiện
    // để người dùng chọn được mà bấm Stop.
    for (const auto& r : uiViewRows_) {
        bool inHostList = false;
        for (const auto& si : hostList_)
            if (si.sourceId == r.sourceId) inHostList = true;
        if (!inHostList) addLine(r.label + L"  — viewing", r.sourceId);
    }

    if (i == 0) {
        SendMessageW(list_, LB_ADDSTRING, 0,
            everRefreshed_ ? (LPARAM)L"(the host is not sharing anything - press Refresh)"
                           : (LPARAM)L"(press Refresh to ask the host what it shares)");
        SendMessageW(list_, LB_SETITEMDATA, 0, (LPARAM)-1);
    }
}

// Phóng thread hỏi host. `manual` = do người dùng bấm (lỗi thì báo hộp thoại);
// lần hỏi tự động lúc mở cửa sổ thì im lặng — host bản cũ không biết
// LIST_SOURCES vẫn xem được bình thường, đừng dí hộp thoại lỗi vào mặt họ.
void ViewerWindow::StartRefresh(bool manual) {
    if (refreshing_) return;
    refreshing_ = true;
    manualRefresh_ = manual;
    if (btnRefresh_) {
        EnableWindow(btnRefresh_, FALSE);
        SetWindowTextW(btnRefresh_, L"Asking...");
    }
    // Thread lần trước chắc chắn đã xong: nút bị khoá suốt lúc nó chạy.
    if (queryThread_.joinable()) queryThread_.join();
    queryThread_ = std::thread([this] {
        std::vector<deskhub::SourceInfo> out;
        const bool ok = QueryHostSources(server_, out);
        {
            std::lock_guard<std::mutex> lk(m_);
            queryOk_ = ok;
            queryResult_ = std::move(out);
        }
        // Cửa sổ có thể đã đóng trong lúc hỏi (~3s) — post vào HWND null là no-op.
        if (HWND h = hwnd_.load(std::memory_order_acquire))
            PostMessageW(h, WM_APP_REFRESH, 0, 0);
    });
}

LRESULT ViewerWindow::HandleMsg(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER: {
            if (wp != kTimerId) break;
            bool need = false;
            {
                std::lock_guard<std::mutex> lk(m_);
                if (dirty_) {
                    uiViewRows_ = rows_;
                    dirty_ = false;
                    need = true;
                }
            }
            if (need) RebuildList();
            return 0;
        }
        case WM_APP_REFRESH: {
            bool ok;
            {
                std::lock_guard<std::mutex> lk(m_);
                ok = queryOk_;
                hostList_ = queryResult_;
            }
            refreshing_ = false;
            if (btnRefresh_) {
                EnableWindow(btnRefresh_, TRUE);
                SetWindowTextW(btnRefresh_, L"Refresh");
            }
            if (ok) everRefreshed_ = true;
            RebuildList();
            if (!ok && manualRefresh_)
                MessageBoxW(h,
                    L"The host did not answer. It may have stopped sharing, "
                    L"or the network is blocking it.",
                    L"Deskhub", MB_OK | MB_ICONWARNING);
            return 0;
        }
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case kIdRefresh:
                    StartRefresh(true);
                    return 0;
                case kIdView: {
                    const LRESULT cur = SendMessageW(list_, LB_GETCURSEL, 0, 0);
                    const LONG_PTR id = cur == LB_ERR
                                            ? -1
                                            : (LONG_PTR)SendMessageW(list_, LB_GETITEMDATA, (WPARAM)cur, 0);
                    if (id < 0) {
                        MessageBoxW(h, L"Select a source in the list first.",
                            L"Deskhub", MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    for (const auto& r : uiViewRows_)
                        if (r.sourceId == uint8_t(id)) return 0; // đang xem rồi — bỏ qua
                    if (uiViewRows_.size() >= maxSources_) {
                        wchar_t m[128];
                        swprintf(m, 128, L"At most %zu sources can be viewed at once.",
                            maxSources_);
                        MessageBoxW(h, m, L"Deskhub", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    for (const auto& si : hostList_) {
                        if (si.sourceId != uint8_t(id)) continue;
                        std::lock_guard<std::mutex> lk(m_);
                        adds_.push_back(si);
                        break;
                    }
                    return 0;
                }
                case kIdStopSel: {
                    const LRESULT cur = SendMessageW(list_, LB_GETCURSEL, 0, 0);
                    const LONG_PTR id = cur == LB_ERR
                                            ? -1
                                            : (LONG_PTR)SendMessageW(list_, LB_GETITEMDATA, (WPARAM)cur, 0);
                    if (id < 0) {
                        MessageBoxW(h, L"Select a source in the list first.",
                            L"Deskhub", MB_OK | MB_ICONINFORMATION);
                        return 0;
                    }
                    bool viewing = false;
                    for (const auto& r : uiViewRows_)
                        if (r.sourceId == uint8_t(id)) viewing = true;
                    if (!viewing) return 0; // nguồn chưa mở — Stop không có gì để làm
                    std::lock_guard<std::mutex> lk(m_);
                    removes_.push_back(uint8_t(id));
                    return 0;
                }
                case kIdDisconnect:
                    // Ẩn ngay cho có phản hồi tức thì; vòng Main thấy cờ sẽ dọn
                    // phiên (BYE từng nguồn) rồi gọi Stop() đóng hẳn cửa sổ.
                    stopReq_.store(true, std::memory_order_release);
                    ShowWindow(h, SW_HIDE);
                    return 0;
                case kIdList:
                    // Đúp chuột một dòng = View nhanh dòng đó.
                    if (HIWORD(wp) == LBN_DBLCLK) {
                        SendMessageW(h, WM_COMMAND, kIdView, 0);
                        return 0;
                    }
                    break;
            }
            break;
        }
        case WM_CLOSE:
            // Đóng cửa sổ quản lý = ngắt kết nối (giống Disconnect).
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

LRESULT CALLBACK ViewerWindow::WndProcThunk(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    auto* self = (ViewerWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    return self->HandleMsg(h, msg, wp, lp);
}

void ViewerWindow::ThreadMain() {
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
    // Góc dưới-phải vùng làm việc — không đè lên các cửa sổ preview (chúng mở ở
    // vị trí mặc định phía trên-trái).
    const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT wr{0, 0, kW, kH};
    AdjustWindowRect(&wr, style, FALSE);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;

    wchar_t title[96];
    swprintf(title, 96, L"Deskhub - viewing %hs", server_.ToString().c_str());
    HWND hwnd = CreateWindowExW(0, kWndClass, title, style,
        wa.right - ww - 24, wa.bottom - wh - 24, ww, wh,
        nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) return; // active_ giữ false — ClientLoop rơi về hành vi cũ

    const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int cx, int cy, int cw,
                  int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, s | WS_CHILD | WS_VISIBLE, cx, cy, cw, ch,
            hwnd, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    mk(L"STATIC", L"Everything the host shares (select a row, then View or Stop):",
        SS_LEFT, 12, 10, kW - 24, 16, 0);
    list_ = mk(L"LISTBOX", nullptr,
        LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
        12, 30, kW - 24, kH - 116, kIdList);
    mk(L"STATIC", L"Refresh asks the host for its current share list.", SS_LEFT,
        12, kH - 78, kW - 24, 16, 0);
    btnRefresh_ = mk(L"BUTTON", L"Refresh", BS_PUSHBUTTON, 12, kH - 50, 90, 28, kIdRefresh);
    mk(L"BUTTON", L"View", BS_PUSHBUTTON, 110, kH - 50, 80, 28, kIdView);
    mk(L"BUTTON", L"Stop", BS_PUSHBUTTON, 198, kH - 50, 80, 28, kIdStopSel);
    mk(L"BUTTON", L"Disconnect", BS_PUSHBUTTON, kW - 12 - 130, kH - 50, 130, 28, kIdDisconnect);

    RebuildList();
    SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    hwnd_.store(hwnd, std::memory_order_release);
    active_.store(true, std::memory_order_release);
    // Stop() có thể đã chạy trong lúc cửa sổ đang được tạo — xem SessionWindow.cpp.
    if (quitReq_.load(std::memory_order_acquire)) DestroyWindow(hwnd);

    // Hỏi host ngay một lần cho danh sách có sẵn lúc mở — im lặng nếu host không
    // trả lời (bản cũ không biết LIST_SOURCES).
    StartRefresh(false);

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
    btnRefresh_ = nullptr;
}
