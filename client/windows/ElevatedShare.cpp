// =============================================================================
// ElevatedShare.cpp — cài đặt việc bàn giao phiên share sang instance admin.
//
// BÀI TOÁN TRUYỀN DỮ LIỆU QUA DÒNG LỆNH
//   Instance mới do UAC dựng lên là một TIẾN TRÌNH KHÁC, nên không chia sẻ được bộ
//   nhớ. Kênh duy nhất là dòng lệnh. Mà nội dung phải truyền lại là tên cửa sổ —
//   tiêu đề tự do do người dùng và ứng dụng khác đặt: có dấu tiếng Việt, khoảng
//   trắng, dấu nháy kép, ký tự điều khiển.
//
//   Luật quoting của CommandLineToArgvW nổi tiếng rắc rối (dấu gạch chéo ngược
//   trước dấu nháy có nghĩa đặc biệt, và số lượng lẻ/chẵn cho kết quả khác nhau).
//   Thay vì cố thoát chuỗi cho đúng, ta HEX HOÁ toàn bộ: mỗi token trên dòng lệnh
//   chỉ còn [0-9a-f], không còn ký tự nào có nghĩa đặc biệt để phải lo.
//   Đắt gấp đôi về độ dài, nhưng bỏ hẳn được cả một lớp lỗi.
//
// BỐ CỤC
//   HexEncode/HexDecode        — mã hoá tên nguồn cho an toàn qua dòng lệnh.
//   IsProcessElevated()        — tiến trình hiện tại có đang chạy admin không.
//   RelaunchElevatedShare()    — bung UAC, dựng dòng lệnh, khởi động instance mới.
//   ParseElevatedShareArgs()   — phía instance mới: đọc lại nguồn + tuỳ chọn.
//
// PHÂN BIỆT "NGƯỜI DÙNG BẤM NO" VỚI "LỖI THẬT"
//   RelaunchElevatedShare trả về cờ outCancelled riêng. Hai trường hợp này cần
//   phản ứng khác nhau: bấm No thì im lặng quay lại (người dùng đã quyết định),
//   còn lỗi thật thì phải báo cho người dùng biết vì sao không share được.
//
// LIÊN QUAN: ElevatedShare.h (vấn đề UIPI + luồng đầy đủ), main.cpp (đường vào
//            của instance admin), AgentLoop.h (AgentSource/AgentOptions)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "ElevatedShare.h"

#include <shellapi.h>

#include <cstdlib>
#include <cstring>

#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kFlagShare[] = L"--elevated-share";

// Tên nguồn là UTF-8 tự do (tiêu đề cửa sổ - có dấu, có khoảng trắng, có cả dấu
// nháy). Mã hex hoá cả chuỗi để mỗi token dòng lệnh chỉ còn [0-9a-f] - không phải
// đụng tới luật quoting của CommandLineToArgvW.
std::wstring HexEncode(const std::string& s) {
    static const wchar_t* kDigits = L"0123456789abcdef";
    std::wstring out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 0xF]);
    }
    return out;
}

bool HexDecode(const std::wstring& in, std::string& out) {
    if (in.size() % 2 != 0) return false;
    out.clear();
    out.reserve(in.size() / 2);
    for (size_t i = 0; i < in.size(); i += 2) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 16; ++k) {
            if (in[i]     == L"0123456789abcdef"[k]) hi = k;
            if (in[i + 1] == L"0123456789abcdef"[k]) lo = k;
        }
        if (hi < 0 || lo < 0) return false;
        out.push_back(char((hi << 4) | lo));
    }
    return true;
}

std::wstring SelfPath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(path, n);
}

// HWND/HMONITOR là handle cấp phiên đăng nhập (session-global), không phải
// per-process như HANDLE của kernel object - nên truyền giá trị sang instance
// admin cùng session vẫn trỏ đúng cửa sổ/màn hình đó.
std::wstring EncodeSource(const AgentSource& s) {
    const bool isWindow = s.target.hwnd != nullptr;
    const uintptr_t handle = isWindow ? (uintptr_t)s.target.hwnd : (uintptr_t)s.target.monitor;
    wchar_t buf[32];
    swprintf(buf, 32, L"%llx", (unsigned long long)handle);
    return std::wstring(isWindow ? L"w:" : L"m:") + buf + L":" + HexEncode(s.name);
}

bool DecodeSource(const std::wstring& tok, AgentSource& out) {
    if (tok.size() < 4 || tok[1] != L':') return false;
    const bool isWindow = tok[0] == L'w';
    if (!isWindow && tok[0] != L'm') return false;

    const size_t sep = tok.find(L':', 2);
    if (sep == std::wstring::npos) return false;

    const std::wstring hexHandle = tok.substr(2, sep - 2);
    const uintptr_t handle = (uintptr_t)wcstoull(hexHandle.c_str(), nullptr, 16);
    if (handle == 0) return false;
    if (!HexDecode(tok.substr(sep + 1), out.name)) return false;

    out.target = isWindow ? CaptureTarget::Window((HWND)handle)
                          : CaptureTarget::Monitor((HMONITOR)handle);
    return true;
}

} // namespace

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD len = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &len) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool RelaunchElevatedShare(std::span<const AgentSource> sources,
                           const AgentOptions& opt, bool& outCancelled) {
    outCancelled = false;

    const std::wstring exe = SelfPath();
    if (exe.empty()) return false;

    wchar_t nums[128];
    swprintf(nums, 128, L" --port %u --fps %u --bitrate %u",
             unsigned(opt.port), unsigned(opt.fps), unsigned(opt.bitrateMbps));

    std::wstring args = kFlagShare;
    args += nums;
    if (opt.allowInput) args += L" --allow-input";
    // Không truyền cờ này thì phiên host thật (chạy trong instance admin) không ghi
    // log — đúng cái bẫy đã làm diag-host.log ra 0 byte. Xem DiagLog.h.
    if (opt.diagLog)    args += L" --diag-log";
    for (const auto& s : sources) args += L" --src " + EncodeSource(s);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas"; // đây là thứ bung hộp thoại UAC
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) CloseHandle(sei.hProcess);
        return true;
    }
    outCancelled = GetLastError() == ERROR_CANCELLED; // người dùng bấm "No"
    return false;
}

bool ParseElevatedShareArgs(int adeskhub, wchar_t** argv,
                            std::vector<AgentSource>& outSources, AgentOptions& outOpt) {
    bool isShare = false;
    for (int i = 1; i < adeskhub; ++i)
        if (wcscmp(argv[i], kFlagShare) == 0) isShare = true;
    if (!isShare) return false;

    AgentOptions opt;
    opt.allowInput = false;
    std::vector<AgentSource> sources;

    for (int i = 1; i < adeskhub; ++i) {
        const std::wstring a = argv[i];
        const bool hasNext = (i + 1) < adeskhub;
        if (a == L"--allow-input") {
            opt.allowInput = true;
        } else if (a == L"--diag-log") {
            opt.diagLog = true;
        } else if (a == L"--port" && hasNext) {
            opt.port = uint16_t(wcstoul(argv[++i], nullptr, 10));
        } else if (a == L"--fps" && hasNext) {
            opt.fps = uint32_t(wcstoul(argv[++i], nullptr, 10));
        } else if (a == L"--bitrate" && hasNext) {
            opt.bitrateMbps = uint32_t(wcstoul(argv[++i], nullptr, 10));
        } else if (a == L"--src" && hasNext) {
            AgentSource s;
            // Cửa sổ có thể đã đóng trong lúc bung UAC - bỏ nguồn chết, còn nguồn
            // nào share nguồn đó.
            if (DecodeSource(argv[++i], s) &&
                (s.target.monitor != nullptr || IsWindow(s.target.hwnd)))
                sources.push_back(std::move(s));
        }
    }

    if (sources.empty()) return false;
    outSources = std::move(sources);
    outOpt = opt;
    return true;
}
