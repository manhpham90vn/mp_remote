#pragma once
// =============================================================================
// DiagLog.h — luôn chuyển toàn bộ log của tiến trình ra một file cạnh exe.
//
// NHIỆM VỤ
//   client.exe là app GUI thuần (không console). Mọi thứ chương trình in ra bằng
//   printf/wprintf trên stdout/stderr — kể cả dòng [DIAG] của docs/09 — được đổi
//   hướng vào một file ngay khi tiến trình khởi động và ở đó tới lúc thoát. Không
//   còn checkbox, không còn console: log LUÔN có sẵn khi cần gửi đi chẩn đoán.
//
// VÌ SAO REDIRECT NGAY LÚC KHỞI ĐỘNG, KHÔNG PHẢI KHI BẮT ĐẦU PHIÊN
//   Sự cố hay xảy ra ở khâu đàm phán/đầu phiên. Bật log muộn (khi người dùng ra
//   lệnh) thì đúng đoạn cần nhất lại chưa được ghi. Mở file một lần ở đầu wWinMain
//   phủ trọn mọi phiên Share/Connect của lần chạy này.
//
// VÌ SAO FREOPEN CHỨ KHÔNG TỰ MỞ MỘT FILE RIÊNG ĐỂ GHI
//   Log rải khắp chương trình bằng printf/wprintf trên stdout. freopen tóm đúng
//   cái stdout ấy — cùng một đường mà `> file` của cmd đi, tức con đường đã biết
//   chắc cho ra UTF-8 đọc được — nên không phải sửa hàng trăm chỗ gọi.
//
// MỖI TIẾN TRÌNH MỘT FILE
//   Tên: deskhub-<ngày>-<giờ>-<pid>.log. Có pid để instance thường và instance
//   admin (Share có điều khiển bung UAC — xem ElevatedShare.h) không ghi đè nhau
//   khi cùng khởi động trong một giây. Vai (agent/client) đã nằm trong từng dòng
//   log nên không cần đưa vào tên file.
//
// LIÊN QUAN: main.cpp (nơi gọi StartProcessLog), docs/09-diagnostics.md
// =============================================================================
#include <string>

// Mở file log cạnh exe và đổi hướng stdout+stderr vào đó cho tới hết tiến trình.
// Gọi MỘT LẦN ở đầu wWinMain. Trả false nếu không tạo được file (thư mục chỉ-đọc,
// ví dụ exe nằm trong Program Files) — chương trình vẫn chạy tiếp, chỉ là không có
// log. Khi `outPath` khác null, ghi đường dẫn file đã mở vào đó.
bool StartProcessLog(std::wstring* outPath = nullptr);
