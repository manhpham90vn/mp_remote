// =============================================================================
// DiagLog.cpp — cài đặt việc đổi hướng toàn bộ log của tiến trình sang file.
//
// VÌ SAO FREOPEN, VÌ SAO MỘT FILE MỖI TIẾN TRÌNH: xem DiagLog.h.
//
// ⚠ KHÔNG ĐỂ GHI LOG CHẶN LUỒNG NÓNG
//   Log LUÔN bật và [DIAG] in dày trên vòng recv/encode. Nếu để stdout unbuffered
//   thì mỗi printf là một lời gọi ghi đĩa NGAY trên luồng đó — gặp đĩa chậm hay AV
//   quét file là recv ngừng nghe, buffer UDP tràn, mất gói THẬT (đúng cái recv_stall
//   mà log định bắt). Nên:
//     1. stdout dùng BUFFER LỚN (_IOFBF): printf trên luồng nóng chỉ là memcpy vào
//        RAM, không đụng đĩa.
//     2. Một THREAD FLUSH NỀN xả buffer ra đĩa mỗi ~500 ms. Việc ghi đĩa (syscall)
//        nằm trên thread phụ này, không trên luồng chính; và nhờ xả đều nên buffer
//        gần như không bao giờ tự đầy giữa hai lần xả (tránh nốt lần flush hiếm hoi
//        rơi vào luồng nóng). Mất mát tối đa khi crash ~ một chu kỳ xả.
//     3. stderr để unbuffered: lỗi hiếm và cần chạm đĩa ngay, không lo chặn.
//
// KHÔNG CÓ ĐƯỜNG TRẢ LẠI (không Stop/restore): app không còn console để trả stdout
// về, file log sống trọn đời tiến trình — thoát bình thường thì CRT tự flush stdio,
// hệ điều hành đóng file. Thread flush là daemon detached, chết theo tiến trình.
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "DiagLog.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <io.h>
#include <thread>

namespace {

// Buffer của stdout: đủ lớn để vài giây log dồn dập không làm nó tự đầy giữa hai
// lần flush nền. Sống trọn đời tiến trình (BSS) vì stdout tham chiếu tới nó.
char g_logBuf[256 * 1024];

// Thư mục chứa exe. Log nằm cạnh exe để người dùng gửi kèm khỏi phải đi tìm.
std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

} // namespace

bool StartProcessLog(std::wstring* outPath) {
    SYSTEMTIME t{};
    GetLocalTime(&t);

    // Giờ ĐỊA PHƯƠNG chứ không phải UTC: người dùng đối chiếu log với "lúc nãy nó
    // giật khoảng 8 rưỡi", và họ đọc giờ trên đồng hồ máy mình. pid tách file của
    // instance thường khỏi instance admin khi cùng khởi động trong một giây.
    wchar_t name[80];
    swprintf(name, 80, L"deskhub-%04u%02u%02u-%02u%02u%02u-%lu.log",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        (unsigned long)GetCurrentProcessId());

    const std::wstring full = ExeDir() + name;

    if (!_wfreopen(full.c_str(), L"w", stdout)) return false;
    // Buffer lớn: hot path chỉ memcpy, không ghi đĩa từng dòng (xem đầu file).
    setvbuf(stdout, g_logBuf, _IOFBF, sizeof(g_logBuf));

    // stderr gộp chung file với stdout (một file để gửi), nhưng để unbuffered để
    // lỗi chạm đĩa ngay.
    _dup2(_fileno(stdout), _fileno(stderr));
    setvbuf(stderr, nullptr, _IONBF, 0);

    if (outPath) *outPath = full;

    std::printf("[DiagLog] %ls started %04u-%02u-%02u %02u:%02u:%02u\n",
        name, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    // Thread xả buffer ra đĩa định kỳ, KHÔNG trên luồng nóng. Detached: chạy tới khi
    // tiến trình thoát.
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::fflush(stdout);
        }
    }).detach();

    return true;
}
