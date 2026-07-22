// =============================================================================
// main.cpp — điểm vào của client.exe. MỘT exe kiểu AnyDesk chứa CẢ HAI vai trò.
//
// NHIỆM VỤ
//   Dựng môi trường tiến trình rồi giao quyền cho màn hình chính. Bản thân file
//   này không có logic nghiệp vụ nào — nó chỉ làm bốn việc khởi tạo và rẽ nhánh.
//
// VÌ SAO MỘT EXE CHO CẢ HOST LẪN CLIENT
//   Người dùng tải một file, chạy lên, rồi tự chọn mình đang chia sẻ hay đang xem.
//   Hai exe riêng nghĩa là phải giải thích cho người dùng cài cái nào ở máy nào —
//   đúng thứ khiến phần mềm điều khiển từ xa khó dùng.
//
// BỐN VIỆC KHỞI TẠO, mỗi cái có lý do riêng (chi tiết ở từng dòng bên dưới)
//   1. DPI awareness — không có thì Windows co giãn giả và toạ độ chuột lệch hết.
//   2. UTF-8 cho console — để in được tiêu đề cửa sổ tiếng Việt.
//   3. Tắt buffer stdout — để log ra ngay cả khi bị redirect.
//   4. capture::InitRuntime() — khởi tạo WinRT, bắt buộc trước khi dùng WGC.
//
// HAI ĐƯỜNG VÀO
//   Bình thường          → RunMainMenuWindow().
//   Vừa được UAC nâng quyền → nhận lại nguồn qua dòng lệnh, vào thẳng RunAgent()
//                             rồi mới về màn hình chính. Xem ElevatedShare.h.
//
// APP GUI THUẦN, KHÔNG CONSOLE
//   Điểm vào là wWinMain (subsystem WINDOWS) — chạy exe không bung cửa sổ console.
//   Mọi log của chương trình đổ vào một file cạnh exe qua StartProcessLog(), mở
//   một lần ở đây và sống trọn đời tiến trình. Không còn stdout ra màn hình, không
//   còn checkbox bật/tắt log — xem DiagLog.h.
//
// GHI CHÚ LỊCH SỬ
//   Test offline của core từng nằm ở đây (`client.exe --nettest`). Nay là target
//   riêng — xem core/tests/CoreTests.cpp.
//
// LIÊN QUAN: ui/MainMenuWindow.h, ElevatedShare.h, AgentLoop.h, DiagLog.h,
//            capture/WindowCapture.h (InitRuntime)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <clocale>

#include "AgentLoop.h"
#include "DiagLog.h"
#include "ElevatedShare.h"
#include "ui/MainMenuWindow.h"
#include "capture/WindowCapture.h"

#include <shellapi.h>
#include <vector>

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Redirect toàn bộ log ra file NGAY từ đầu — trước mọi khởi tạo khác — để không
    // sót đoạn đầu nếu có sự cố sớm. App không có console nên đây là nơi log duy
    // nhất đi tới (xem DiagLog.h). Không tạo được file thì vẫn chạy tiếp, chỉ là
    // các printf rơi vào stdout không gắn đâu.
    StartProcessLog();

    // PER_MONITOR_AWARE_V2: nói với Windows rằng ta tự xử lý tỉ lệ DPI. Không khai
    // thì Windows co giãn giả cửa sổ, và GetClientRect trả kích thước ĐÃ co giãn —
    // toạ độ chuột quy đổi ra sẽ lệch trên mọi máy đặt tỉ lệ khác 100%.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // UTF-8 cho stream log để wprintf ghi đúng tiêu đề cửa sổ có dấu (tiếng Việt...).
    std::setlocale(LC_ALL, ".UTF8");
    capture::InitRuntime();

    // Instance này có thể là bản vừa được UAC nâng quyền từ nút Share (xem
    // ElevatedShare.h): nhận lại nguồn + thông số qua dòng lệnh và vào thẳng phiên
    // share, khỏi bắt người dùng chọn nguồn lần hai. Xong phiên thì về main menu
    // như một lần chạy bình thường.
    int wadeskhub = 0;
    if (wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wadeskhub)) {
        std::vector<AgentSource> sources;
        AgentOptions opt;
        const bool elevatedShare = ParseElevatedShareArgs(wadeskhub, wargv, sources, opt);
        LocalFree(wargv);
        if (elevatedShare) {
            // Instance này LÀ phiên host thật (đã tự mở file log riêng ở trên nhờ
            // pid trong tên file, không đè log của instance gốc).
            RunAgent(sources, opt);
            return RunMainMenuWindow();
        }
    }

    return RunMainMenuWindow();
}
