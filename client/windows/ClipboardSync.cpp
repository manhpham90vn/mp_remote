// =============================================================================
// ClipboardSync.cpp — cài đặt theo dõi/đặt clipboard (xem .h về thiết kế).
//
// BỐ CỤC
//   Utf8/Utf16       — hai chiều đổi mã, clipboard Windows nói UTF-16.
//   ThreadMain       — thread riêng: tạo message-only window, nghe, bơm message.
//   OnClipboardChanged — đọc văn bản, lọc echo, báo onLocal.
//   ApplyRemote      — đặt văn bản từ máy kia vào clipboard.
//
// OpenClipboard CÓ THỂ TRƯỢT: clipboard là khoá toàn hệ thống, ứng dụng khác đang
// giữ (trình quản lý clipboard, chính app vừa copy) thì mở trượt. Thử lại vài lần
// cách quãng ngắn rồi bỏ — đồng bộ clipboard là best-effort, lần copy sau tự bù.
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ClipboardSync.h"

#include <cstdio>

#pragma comment(lib, "user32.lib")

namespace {

constexpr wchar_t kWndClass[] = L"DeskhubClipboardWnd";
constexpr UINT kMsgApplyRemote = WM_APP + 1; // lParam = std::wstring* (nhận sở hữu)

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
        nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n,
            nullptr, nullptr);
    return s;
}

std::wstring Utf16(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? size_t(n) : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Mở clipboard với vài lần thử lại — xem ghi chú đầu file.
bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

} // namespace

bool ClipboardSync::Start(OnLocalText onLocal) {
    if (thread_.joinable()) return true;
    onLocal_ = std::move(onLocal);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) return false;
    thread_ = std::thread([this] { ThreadMain(); });
    WaitForSingleObject(ready_, 3000);
    CloseHandle(ready_);
    ready_ = nullptr;
    return hwnd_.load() != nullptr;
}

void ClipboardSync::Stop() {
    if (!thread_.joinable()) return;
    if (HWND h = hwnd_.load()) PostMessageW(h, WM_CLOSE, 0, 0);
    thread_.join();
    hwnd_.store(nullptr);
}

void ClipboardSync::SetRemoteText(const std::string& utf8) {
    if (utf8.empty()) return;
    std::wstring w = Utf16(utf8);
    {
        // Trùng thứ mình vừa đặt/vừa đọc (echo hoặc nhiều phiên cùng gửi) — thôi.
        std::lock_guard<std::mutex> lk(mu_);
        if (w == lastText_) return;
    }
    HWND h = hwnd_.load();
    if (!h) return;
    // Chuyển sở hữu chuỗi sang thread clipboard; Post trượt thì tự dọn.
    auto* p = new std::wstring(std::move(w));
    if (!PostMessageW(h, kMsgApplyRemote, 0, (LPARAM)p)) delete p;
}

void ClipboardSync::ThreadMain() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClass;
    RegisterClassW(&wc); // lần hai trở đi ALREADY_EXISTS — không sao

    // HWND_MESSAGE: cửa sổ chỉ nhận message, không bao giờ hiện lên màn hình.
    HWND h = CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (h) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)this);
        if (!AddClipboardFormatListener(h)) {
            std::printf("[Clipboard] AddClipboardFormatListener failed: %lu\n",
                GetLastError());
            DestroyWindow(h);
            h = nullptr;
        }
    }
    hwnd_.store(h);
    SetEvent(ready_);
    if (!h) return;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveClipboardFormatListener(h);
    DestroyWindow(h);
}

LRESULT CALLBACK ClipboardSync::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = (ClipboardSync*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            if (self) self->OnClipboardChanged();
            return 0;
        case kMsgApplyRemote: {
            std::wstring* p = (std::wstring*)lp;
            if (self && p) self->ApplyRemote(*p);
            delete p;
            return 0;
        }
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ClipboardSync::OnClipboardChanged() {
    HWND h = hwnd_.load();
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return; // ảnh/file — bỏ qua
    if (!OpenClipboardRetry(h)) return;
    std::wstring text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(data)) {
            text.assign(p); // tới NUL — đúng quy ước CF_UNICODETEXT
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    if (text.empty()) return;

    {
        // Chính là thứ mình vừa đặt (echo của SetRemoteText) hoặc copy lặp — nuốt.
        std::lock_guard<std::mutex> lk(mu_);
        if (text == lastText_) return;
        lastText_ = text;
    }
    if (onLocal_) onLocal_(Utf8(text));
}

void ClipboardSync::ApplyRemote(const std::wstring& text) {
    // Ghi lastText_ TRƯỚC khi đặt: WM_CLIPBOARDUPDATE do chính mình gây ra sẽ về
    // ngay sau SetClipboardData, phải thấy chốt chống echo đã đóng rồi.
    {
        std::lock_guard<std::mutex> lk(mu_);
        lastText_ = text;
    }
    HWND h = hwnd_.load();
    if (!OpenClipboardRetry(h)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}
