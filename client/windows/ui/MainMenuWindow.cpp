// =============================================================================
// MainMenuWindow.cpp — cài đặt màn hình chính bằng Win32 thuần.
//
// VÌ SAO WIN32 THUẦN, KHÔNG DÙNG FILE .rc HAY FRAMEWORK GIAO DIỆN
//   Cửa sổ và các control đều tạo bằng CreateWindowExW trong mã. Đổi lại việc phải
//   tự tính toạ độ, ta được: không phụ thuộc thư viện ngoài, không có bước biên
//   dịch tài nguyên, và file exe chạy độc lập không cần cài gì. Với một giao diện
//   chỉ vài ô nhập và ít nút thì đó là đánh đổi đúng.
//
// HAI HỘP: HOST MODE và CLIENT MODE
//   Màn hình chia làm hai group box tách bạch để người dùng biết ngay mình đang làm
//   vai nào:
//     • "Host mode"   — chia sẻ máy này: hiện địa chỉ IP (kèm nút Copy), các thông
//                       số Port/FPS/Bitrate, và nút Share.
//     • "Client mode" — kết nối tới máy khác: ô nhập ip[:port], tuỳ chọn View only,
//                       và nút Connect.
//   Hai tuỳ chọn dùng chung (log chẩn đoán) và nút Exit nằm dưới cùng, ngoài hai hộp.
//   Port nằm TRONG hộp host = cổng máy này lắng nghe; phía client lấy cổng từ chính
//   ô địa chỉ ip[:port] nên hai vai không còn dùng lẫn một ô Port như trước.
//
// KHUÔN MẪU: CÁC HẰNG kId*
//   Mỗi control có một id số nguyên; WM_COMMAND báo về kèm id đó để biết ai vừa
//   được bấm. Gom hằng lên đầu file thay vì rải số trong mã. Các nút Copy dùng dải
//   id liên tiếp từ kIdCopyBase để một chỗ xử lý được mọi dòng IP.
//
// ĐƯỜNG RẼ HAI VAI
//   Nút Share   → WindowPickerDialog → RunAgent()  (chặn tới khi phiên kết thúc)
//   Nút Connect → QueryHostSources → SourcePickerDialog → RunClient()
//   Cả hai đều CHẶN: cửa sổ chính đứng yên trong lúc phiên chạy, và hiện lại khi
//   phiên kết thúc. Đơn giản hơn nhiều so với chạy nền, và đúng với thực tế người
//   dùng chỉ làm một việc tại một thời điểm.
//
// LIÊN QUAN: ui/MainMenuWindow.h (vai trò + lý do bỏ CLI), ui/WindowPickerDialog.h,
//            ui/SourcePickerDialog.h, AgentLoop.h, ClientLoop.h
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ui/MainMenuWindow.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "AgentLoop.h"
#include "ClientLoop.h"
#include "DiagLog.h"
#include "ElevatedShare.h"
#include "net/Firewall.h"
#include "net/NetInfo.h"
#include "ui/SourcePickerDialog.h"
#include "net/UdpSocket.h"
#include "ui/WindowPickerDialog.h"

namespace {

constexpr wchar_t kWndClass[] = L"DeskhubMainMenu";

constexpr int kIdEditPort    = 200;
constexpr int kIdEditFps     = 199;
constexpr int kIdEditBitrate = 198;
constexpr int kIdShare       = 201;
constexpr int kIdEditAddr    = 202;
constexpr int kIdConnect     = 203;
constexpr int kIdChkViewOnly = 204;
constexpr int kIdExit        = 205;
constexpr int kIdChkSaveLog  = 206;
// Dải id cho các nút Copy: một dòng IP một nút, id = kIdCopyBase + chỉ số dòng.
constexpr int kIdCopyBase    = 300;

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

// Đặt một chuỗi Unicode lên clipboard. Nếu SetClipboardData thành công thì hệ thống
// tiếp quản khối nhớ, ta không giải phóng nữa; ngược lại phải tự GlobalFree.
void CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            if (SetClipboardData(CF_UNICODETEXT, h)) h = nullptr; // clipboard sở hữu khối nhớ
        }
        if (h) GlobalFree(h);
    }
    CloseClipboard();
}

struct MenuState {
    HWND hwnd = nullptr;
    HWND editPort = nullptr;
    HWND editFps = nullptr;
    HWND editBitrate = nullptr;
    HWND editAddr = nullptr;
    HWND chkViewOnly = nullptr;
    HWND chkSaveLog = nullptr;
    std::vector<std::wstring> copyIps; // IP mỗi dòng, song song với các nút Copy
    bool quit = false;
};

bool WantDiagLog(const MenuState& st) {
    return SendMessageW(st.chkSaveLog, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

// Phiên xong thì chỉ đường tới file: người dùng bật log là đang chuẩn bị gửi nó đi,
// mà log nằm cạnh exe — chỗ không phải ai cũng biết tìm.
void ReportDiagLog(HWND owner, const DiagLogRedirect& log, bool wanted) {
    if (!wanted) return;
    if (log.active()) {
        const std::wstring msg = L"Diagnostic log saved to:\n\n" + log.path();
        MessageBoxW(owner, msg.c_str(), L"Deskhub", MB_OK | MB_ICONINFORMATION);
    } else {
        MessageBoxW(owner,
                    L"Could not create the diagnostic log file next to the program.\n\n"
                    L"The session ran normally, but nothing was recorded. Try moving "
                    L"the program to a folder you can write to (for example Desktop).",
                    L"Deskhub", MB_OK | MB_ICONWARNING);
    }
}

void DoShare(MenuState& st) {
    std::vector<AgentSource> sources;
    bool allow = true;
    if (!ShowWindowPickerDialog(st.hwnd, sources, allow)) return;

    AgentOptions ao;
    ao.port = uint16_t(GetEditUint(st.editPort, kDefaultPort));
    ao.fps = GetEditUint(st.editFps, kDefaultFps);
    ao.bitrateMbps = GetEditUint(st.editBitrate, kDefaultBitrateMbps);
    ao.allowInput = allow;
    // Đặt trước nhánh elevate: nếu phiên nhảy sang instance admin thì cờ này là thứ
    // duy nhất mang yêu cầu ghi log đi cùng (ElevatedShare.cpp thêm --diag-log).
    ao.diagLog = WantDiagLog(st);

    // HAI lý do cần quyền admin, gộp vào MỘT lần bung UAC lúc bấm Share:
    //   • allowInput  — UIPI: input chỉ đi tới được cửa sổ của tiến trình có integrity
    //                   level không cao hơn mình. Host quyền thường vẫn capture được
    //                   game/app chạy admin nhưng SendInput bị nuốt IM LẶNG.
    //   • firewall    — thêm rule inbound để client không bị timeout; thêm rule đòi
    //                   admin, và chỉ cần khi rule chưa có sẵn (những lần sau bỏ qua).
    // Instance admin tiếp quản sẽ vừa gửi được input vừa thêm rule (AgentLoop gọi
    // EnsureHostFirewallRule khi mở socket).
    const bool needFirewall = !HostFirewallRulePresent();
    if ((ao.allowInput || needFirewall) && !IsProcessElevated()) {
        bool cancelled = false;
        if (RelaunchElevatedShare(sources, ao, cancelled)) {
            st.quit = true; // instance admin tiếp quản đúng phiên này
            return;
        }
        // Không nâng được quyền: nêu đúng hệ quả của TỪNG lý do đang áp dụng, để người
        // dùng biết chính xác cái gì sẽ không chạy.
        std::wstring msg = cancelled
            ? std::wstring(L"Continuing without administrator rights.\n\n")
            : std::wstring(L"Could not restart as administrator. Sharing continues "
                           L"without it.\n\n");
        if (needFirewall)
            msg += L"- Windows Firewall may block the other machine from connecting. "
                   L"If it cannot connect, allow client.exe through Windows Firewall for "
                   L"the network you are on, or run this program as administrator once.\n\n";
        if (ao.allowInput)
            msg += L"- Mouse/keyboard control will not reach apps that run as "
                   L"administrator (games with anti-cheat, elevated tools). Everything "
                   L"else still works.";
        MessageBoxW(st.hwnd, msg.c_str(), L"Deskhub", MB_OK | MB_ICONWARNING);
    }

    ShowWindow(st.hwnd, SW_HIDE);
    {
        DiagLogRedirect log; // scope riêng: trả stdout về console trước khi hiện menu
        if (ao.diagLog) log.Start(DiagRole::Agent);
        RunAgent(sources, ao);
        ShowWindow(st.hwnd, SW_SHOW);
        SetForegroundWindow(st.hwnd);
        ReportDiagLog(st.hwnd, log, ao.diagLog);
    }
}

void DoConnect(MenuState& st) {
    wchar_t buf[128] = {};
    GetWindowTextW(st.editAddr, buf, 128);
    const std::wstring waddr = Trim(buf);
    if (waddr.empty()) {
        MessageBoxW(st.hwnd, L"Enter the host machine's IP address first (e.g., 192.168.1.10).",
                    L"Deskhub", MB_OK | MB_ICONWARNING);
        return;
    }
    // Địa chỉ ip[:port] chỉ gồm ASCII - thu hẹp từng ký tự là an toàn.
    std::string addr;
    addr.reserve(waddr.size());
    for (wchar_t c : waddr) addr.push_back(char(c));

    // Cổng để kết nối lấy từ chính ô địa chỉ (ip:port); không gõ port thì dùng mặc
    // định. Vai client không còn phụ thuộc ô Port của hộp host.
    ClientOptions co;
    if (!ParseNetAddr(addr, kDefaultPort, co.server)) {
        const std::wstring msg = L"Invalid address: \"" + waddr +
            L"\"\n(e.g., 192.168.1.10 or 192.168.1.10:47777)";
        MessageBoxW(st.hwnd, msg.c_str(), L"Deskhub", MB_OK | MB_ICONERROR);
        return;
    }
    co.saveBmp = false;
    co.sendInput = SendMessageW(st.chkViewOnly, BM_GETCHECK, 0, 0) != BST_CHECKED;

    // Hỏi host đang chia sẻ những gì rồi cho chọn. Host bản cũ (hoặc sai IP /
    // firewall chặn) không trả lời -> cứ thử nguồn 0, ClientSession sẽ báo lỗi
    // kết nối cụ thể hơn nhiều so với một hộp thoại "không thấy host".
    std::vector<deskhub::SourceInfo> available;
    if (QueryHostSources(co.server, available) && !available.empty()) {
        if (!ShowSourcePickerDialog(st.hwnd, available, co.sources)) return;
    }

    ShowWindow(st.hwnd, SW_HIDE);
    {
        const bool wantLog = WantDiagLog(st);
        DiagLogRedirect log; // scope riêng: trả stdout về console trước khi hiện menu
        if (wantLog) log.Start(DiagRole::Client);
        RunClient(co);
        ShowWindow(st.hwnd, SW_SHOW);
        SetForegroundWindow(st.hwnd);
        ReportDiagLog(st.hwnd, log, wantLog);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (MenuState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wp);
        // Nút Copy: id nằm trong dải liên tiếp, quy về chỉ số dòng IP tương ứng.
        if (id >= kIdCopyBase && id < kIdCopyBase + (int)st->copyIps.size()) {
            CopyTextToClipboard(st->hwnd, st->copyIps[size_t(id - kIdCopyBase)]);
            return 0;
        }
        switch (id) {
        case kIdShare:   DoShare(*st);    return 0;
        case kIdConnect: DoConnect(*st);  return 0;
        case kIdExit:    st->quit = true; return 0;
        }
        break;
    }
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

    MenuState st;

    // Địa chỉ IPv4 của máy này - biết trước số dòng để tính chiều cao hộp host và
    // cả cửa sổ (mỗi adapter một dòng kèm nút Copy).
    const auto addrs = ListLocalIPv4();
    const int nRows = addrs.empty() ? 1 : (int)addrs.size();

    // Chọn sẵn cổng host: ưu tiên 47777, nếu bận (host cũ còn chạy ngầm) thì +1 dần
    // tới cổng trống kế tiếp. Dò NGAY LÚC NÀY để cổng hiển thị + copy + điền vào ô
    // Port khớp đúng cổng mà phiên Share sắp tới sẽ bind (AgentLoop dò lại cùng kiểu).
    const uint16_t freePort = FindFreeUdpPort(kDefaultPort, 64);
    const uint16_t sharePort = freePort ? freePort : kDefaultPort;
    const std::wstring sharePortText = std::to_wstring(sharePort);

    // --- Bố cục: hai group box xếp dọc, dưới cùng là tuỳ chọn chung + Exit ---
    constexpr int kW = 496;
    const int gx = 12;             // lề trái của group box
    const int gw = kW - 24;        // bề rộng group box
    const int ix = gx + 14;        // lề trái nội dung bên trong hộp
    const int iw = gw - 28;        // bề rộng nội dung bên trong hộp
    const int rowH = 22;           // cao mỗi dòng IP

    // Chiều cao hộp host phụ thuộc số dòng IP; hộp client cố định.
    const int hostTop = 8;
    const int settingsRel = 44 + nRows * rowH + 8;   // hàng Port/FPS/Bitrate (theo hostTop)
    const int shareRel    = settingsRel + 34;         // nút Share
    const int hostH       = shareRel + 32 + 12;
    const int clientTop   = hostTop + hostH + 10;
    const int clientH     = 118;
    const int saveLogY    = clientTop + clientH + 12;
    const int exitY       = saveLogY + 30;
    const int kH          = exitY + 44;

    const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    RECT wr{ 0, 0, kW, kH };
    AdjustWindowRect(&wr, style, FALSE);
    HWND hwnd = CreateWindowExW(0, kWndClass, L"Deskhub - stream & remotely control an application",
                                 style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 wr.right - wr.left, wr.bottom - wr.top,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;

    st.hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&st);

    const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD s, int cx, int cy, int cw, int ch, int id) {
        HWND c = CreateWindowExW(0, cls, text, s | WS_CHILD | WS_VISIBLE, cx, cy, cw, ch,
                                  hwnd, (HMENU)(INT_PTR)id, wc.hInstance, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };

    // Group box phải tạo TRƯỚC các control con để con vẽ đè lên khung (control tạo
    // sau nằm trên trong z-order).
    mk(L"BUTTON", L"Host mode - share an application on THIS machine",
       BS_GROUPBOX, gx, hostTop, gw, hostH, 0);

    // Địa chỉ máy này: đọc/copy cho người bên kia gõ vào ô Connect của họ. Nút Copy
    // lấy CẢ ip lẫn cổng (ip:port) để họ dán thẳng, không phải nhớ cổng riêng. Cổng
    // hiển thị = cổng phiên Share sắp dùng (đã dò ở trên).
    mk(L"STATIC", L"Others connect to you using one of these addresses:",
       SS_LEFT, ix, hostTop + 22, iw, 16, 0);

    constexpr int kCopyW = 60, kCopyH = 20;
    const int copyX = gx + gw - 14 - kCopyW;
    if (addrs.empty()) {
        mk(L"STATIC", L"(no network address found)", SS_LEFT,
           ix, hostTop + 44, iw, 18, 0);
    } else {
        st.copyIps.reserve(addrs.size());
        int i = 0;
        for (const auto& a : addrs) {
            const int rowY = hostTop + 44 + i * rowH;
            std::wstring wip(a.ip.begin(), a.ip.end());
            std::wstring addr = wip + L":" + sharePortText; // giá trị Copy = ip:port
            wchar_t line[192];
            swprintf(line, 192, L"%-20ls %ls", a.name.c_str(), addr.c_str());
            mk(L"STATIC", line, SS_LEFT | SS_ENDELLIPSIS,
               ix, rowY + 2, copyX - 8 - ix, 18, 0);
            mk(L"BUTTON", L"Copy", BS_PUSHBUTTON,
               copyX, rowY, kCopyW, kCopyH, kIdCopyBase + i);
            st.copyIps.push_back(std::move(addr));
            ++i;
        }
    }

    // Thông số phía host: Port máy này lắng nghe + FPS + Bitrate của luồng gửi đi.
    const int sy = hostTop + settingsRel;
    mk(L"STATIC", L"Port", SS_LEFT, ix, sy + 3, 32, 18, 0);
    st.editPort = mk(L"EDIT", sharePortText.c_str(), WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                      ix + 36, sy, 64, 24, kIdEditPort);
    mk(L"STATIC", L"FPS", SS_LEFT, ix + 116, sy + 3, 30, 18, 0);
    st.editFps = mk(L"EDIT", L"60", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                     ix + 148, sy, 48, 24, kIdEditFps);
    mk(L"STATIC", L"Bitrate (Mbps)", SS_LEFT, ix + 212, sy + 3, 90, 18, 0);
    st.editBitrate = mk(L"EDIT", L"20", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER,
                         ix + 304, sy, 48, 24, kIdEditBitrate);

    mk(L"BUTTON", L"Share...  (pick screens/windows to share)", BS_PUSHBUTTON,
       ix, hostTop + shareRel, iw, 32, kIdShare);

    // --- Hộp client: kết nối tới một máy khác ---
    mk(L"BUTTON", L"Client mode - connect to ANOTHER machine",
       BS_GROUPBOX, gx, clientTop, gw, clientH, 0);

    mk(L"STATIC", L"Host machine address (ip[:port]):", SS_LEFT,
       ix, clientTop + 24, iw, 16, 0);
    st.editAddr = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
                      ix, clientTop + 44, iw - 110, 26, kIdEditAddr);
    mk(L"BUTTON", L"Connect", BS_DEFPUSHBUTTON,
       ix + iw - 100, clientTop + 44, 100, 26, kIdConnect);
    st.chkViewOnly = mk(L"BUTTON", L"View only, don't send mouse/keyboard input",
                         BS_AUTOCHECKBOX, ix, clientTop + 80, iw, 20, kIdChkViewOnly);

    // --- Chung cho cả hai vai + Exit ---
    // Tick rồi bấm Share ra diag-agent-*.log, bấm Connect ra diag-client-*.log. Một
    // checkbox chung vì người dùng chỉ cần nhớ một thao tác ("bật cái này rồi tái
    // hiện lỗi"), còn tên file thì chương trình tự phân biệt.
    st.chkSaveLog = mk(L"BUTTON", L"Save diagnostic log to a file next to this program",
                        BS_AUTOCHECKBOX, gx, saveLogY, gw, 20, kIdChkSaveLog);

    mk(L"BUTTON", L"Exit", BS_PUSHBUTTON, gx, exitY, 100, 28, kIdExit);

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
