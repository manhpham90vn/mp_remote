#pragma once
// =============================================================================
// Clock.h — đồng hồ đơn điệu (monotonic) đơn vị micro-giây, một tên cho mọi OS.
//
// NHIỆM VỤ
//   Cung cấp NowUs(): mốc thời gian dùng xuyên suốt dự án. Có ba chỗ tiêu thụ nó:
//     1. `nowUs` bơm vào core/ — Reassembler, ClientSession, HostSession,
//        BitrateController, LinkStats đều không tự đọc đồng hồ mà nhận từ ngoài.
//     2. timestamp của frame video phía agent (trường timestampUs trên wire).
//     3. đo RTT trong PING/PONG.
//
// VỊ TRÍ TRONG KIẾN TRÚC
//   core/ (thuần C++20, cấm đụng OS)  ←── nowUs ──  **platform/Clock.h**  ──→ OS API
//   Header này nằm ở platform/ chứ KHÔNG ở core/ chính vì nó include header hệ
//   điều hành. Ranh giới đó là điều kiện để core/ biên dịch được cho Windows,
//   Android NDK, iOS, Ubuntu bằng cùng một mã nguồn (xem core/CMakeLists.txt).
//
// VÌ SAO MỘT TÊN DUY NHẤT
//   Trước đây mỗi OS một bản riêng với TÊN KHÁC NHAU (Windows QpcUs(), Android
//   NowUs()) khiến code port qua lại phải sửa tay. Giờ khác biệt OS nằm gọn trong
//   #ifdef dưới đây, còn phía trên gọi NowUs() y hệt nhau ở mọi nền tảng.
//
// VÌ SAO ĐƠN ĐIỆU, KHÔNG PHẢI GIỜ THỰC
//   Cả hai nhánh đều dùng đồng hồ đơn điệu: nó chỉ tăng, không nhảy khi người dùng
//   chỉnh giờ hệ thống hay khi NTP đồng bộ lại. Điều đó là bắt buộc vì gần như mọi
//   phép tính thời gian trong dự án là phép TRỪ hai mốc (timeout phiên, RTT, độ dài
//   cửa sổ thống kê). Một cú nhảy giờ trên đồng hồ giờ thực sẽ làm timeout kích hoạt
//   sai hàng loạt, hoặc tệ hơn là cho ra hiệu ÂM rồi tràn thành số khổng lồ vì các
//   phép trừ này làm trên uint64_t.
//   Hệ quả: giá trị trả về KHÔNG có ý nghĩa lịch (không quy ra ngày giờ được), mốc 0
//   là tuỳ tiện và khác nhau giữa hai máy. Chỉ dùng nó cho hiệu số.
//
// GIỚI HẠN ĐÃ BIẾT
//   Cả hai nhánh ĐỨNG YÊN khi máy ngủ sâu (Windows: QPC dừng khi suspend; Linux:
//   CLOCK_MONOTONIC không đếm thời gian suspend, khác với CLOCK_BOOTTIME). Chấp
//   nhận được vì phiên stream cũng chết lúc đó — mất kết nối được phát hiện bằng
//   timeout ngay sau khi máy thức dậy.
//
// VÌ SAO MICRO-GIÂY
//   Mili-giây quá thô để đo RTT trong mạng LAN (thường dưới 1 ms) và để đánh dấu
//   frame ở 60 fps (16.7 ms/frame). Nano-giây thì thừa và tràn uint64 sau ~584 năm
//   — không phải vấn đề, nhưng cũng chẳng được gì. Micro-giây là mức vừa đủ.
//
// HEADER-ONLY, HÀM `inline`
//   Không có .cpp đi kèm, nên platform/ là INTERFACE library trong CMake. Hàm đủ
//   ngắn để trình dịch nội tuyến hẳn — điều này đáng kể vì NowUs() được gọi nhiều
//   lần mỗi vòng lặp mạng.
//
// LIÊN QUAN: platform/CMakeLists.txt, core/CMakeLists.txt (nguyên tắc core không
//            đụng OS), deskhub/session/*.h (nơi tiêu thụ nowUs)
// =============================================================================
#include <cstdint>

// ---------------------------------------------------------------------------
// Nhánh Windows: QueryPerformanceCounter (QPC).
//
// QPC là đồng hồ độ phân giải cao duy nhất Windows bảo đảm vừa đơn điệu vừa nhất
// quán giữa các nhân CPU. Không dùng GetTickCount64 (chỉ ~15 ms, quá thô) cũng
// không dùng rdtsc trực tiếp (lệch nhau giữa các nhân, đổi theo tần số CPU).
//
// Hai macro dưới đây phải đặt TRƯỚC #include <windows.h>:
//   WIN32_LEAN_AND_MEAN — cắt bớt phần lớn header con của Windows SDK. Header này
//     bị include ở rất nhiều nơi, mỗi lần kéo trọn windows.h là thêm thời gian dịch.
//   NOMINMAX — nếu không có, windows.h định nghĩa min/max thành MACRO và chúng sẽ
//     phá mọi lời gọi std::min/std::max ở các file include sau nó, với thông báo
//     lỗi rất khó lần ra nguyên nhân.
// ---------------------------------------------------------------------------
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

inline uint64_t NowUs() {
    // Tần số QPC cố định suốt phiên chạy của máy, nên hỏi đúng một lần rồi nhớ lại.
    // `static` cục bộ trong hàm inline: C++11 trở đi bảo đảm khởi tạo đúng một lần
    // và an toàn với đa luồng, nên không cần khoá dù nhiều thread cùng gọi.
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // Đổi số đếm (tick) sang micro-giây: ticks / freq * 1e6.
    //
    // Tách phần nguyên và phần dư TRƯỚC khi nhân, thay vì viết thẳng
    // c.QuadPart * 1'000'000 / freq.QuadPart — cách viết thẳng đó tràn uint64 sau
    // khoảng 5 giờ chạy với freq 10 MHz (giá trị phổ biến trên máy hiện đại), và
    // lúc tràn thì thời gian nhảy lùi làm mọi phép trừ cho ra số rác.
    //   phần nguyên: (ticks / freq) giây, nhân 1e6 → micro-giây, không mất mát;
    //   phần dư:     (ticks % freq) < freq nên nhân 1e6 không bao giờ tràn.
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1'000'000ull +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1'000'000ull / (uint64_t)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// Nhánh POSIX (Android, Linux, macOS, iOS): clock_gettime.
//
// CLOCK_MONOTONIC chứ không phải CLOCK_REALTIME (bị NTP kéo lùi) và cũng không
// phải CLOCK_BOOTTIME (có đếm cả thời gian máy ngủ) — xem mục GIỚI HẠN ĐÃ BIẾT ở
// đầu file về lý do không cần đếm thời gian ngủ.
// ---------------------------------------------------------------------------
#else
#include <ctime>

inline uint64_t NowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // timespec tách sẵn giây và nano-giây, nên không có nguy cơ tràn như nhánh QPC:
    // tv_nsec luôn < 1e9. Phép chia 1000 cắt cụt phần lẻ dưới micro-giây — không
    // đáng kể, và cắt cụt vẫn giữ tính đơn điệu (làm tròn thì không).
    return uint64_t(ts.tv_sec) * 1'000'000ull + uint64_t(ts.tv_nsec) / 1000ull;
}

#endif
