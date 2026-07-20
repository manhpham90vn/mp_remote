#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "WindowFinder.h"

#include <dwmapi.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "dwmapi.lib")

namespace {

struct WindowSearch {
    std::wstring targetExe;
    HWND         found = nullptr;
    LONG         bestArea = 0;
};

std::wstring BaseNameLower(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);
    return name;
}

// Tên exe (chữ thường) của process sở hữu cửa sổ; rỗng nếu không đọc được.
std::wstring ExeNameOfWindow(HWND hwnd, DWORD* outPid = nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (outPid) *outPid = pid;
    if (pid == 0) return {};

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) name = BaseNameLower(path);
    CloseHandle(proc);
    return name;
}

// UWP/app bị treo vẫn "visible" nhưng bị DWM cloak - không capture được.
bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
        && cloaked != 0;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lparam) {
    auto* search = reinterpret_cast<WindowSearch*>(lparam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    if (ExeNameOfWindow(hwnd) != search->targetExe) return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > search->bestArea) {
        search->bestArea = area;
        search->found = hwnd;
    }
    return TRUE;
}

BOOL CALLBACK ListProc(HWND hwnd, LPARAM lparam) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lparam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (IsCloaked(hwnd)) return TRUE;

    wchar_t title[256] = {};
    if (GetWindowTextW(hwnd, title, 256) == 0) return TRUE;

    DWORD pid = 0;
    std::wstring exe = ExeNameOfWindow(hwnd, &pid);
    if (exe.empty() || pid == GetCurrentProcessId()) return TRUE;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    const LONG w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return TRUE;

    WindowInfo info;
    info.hwnd = hwnd;
    info.exeName = std::move(exe);
    info.title = title;
    info.width = (uint32_t)w;
    info.height = (uint32_t)h;
    info.minimized = IsIconic(hwnd) != FALSE;
    out->push_back(std::move(info));
    return TRUE;
}

}  // namespace

HWND FindWindowByProcessName(const std::wstring& exeName) {
    WindowSearch search;
    search.targetExe = BaseNameLower(exeName);
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

std::vector<WindowInfo> ListCapturableWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(ListProc, reinterpret_cast<LPARAM>(&windows));
    std::sort(windows.begin(), windows.end(), [](const WindowInfo& a, const WindowInfo& b) {
        return (uint64_t)a.width * a.height > (uint64_t)b.width * b.height;
    });
    return windows;
}
