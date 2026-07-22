#pragma once
// =============================================================================
// SourceQuery.h — hỏi host đang chia sẻ những cửa sổ nào (LIST_SOURCES → SOURCE_LIST).
//
// NHIỆM VỤ
//   Một lần trao đổi hỏi-đáp duy nhất, chạy TRƯỚC khi có phiên. Kết quả là danh
//   sách cửa sổ để người dùng chọn xem cái nào. Bản port của QueryHostSources()
//   trong client/windows/ClientLoop.cpp.
//
// VỊ TRÍ TRONG LUỒNG NGƯỜI DÙNG
//   MainActivity: gõ địa chỉ → **QuerySources()** → chọn nguồn → StreamActivity
//                                                                  → ClientLoop::Start()
//
// VÌ SAO ĐỨNG NGOÀI ClientLoop
//   Nó chạy trước khi có phiên: mở socket riêng, không sessionId, không thread —
//   gọi xong là xong. Nhét vào ClientLoop chỉ tổ buộc lớp đó phải mang thêm một
//   trạng thái "chưa kết nối" mà nó không dùng đến ở bất cứ đâu khác.
//
// TÍNH CHẤT CẦN BIẾT TRƯỚC KHI GỌI
//   - CHẶN tới ~3 giây (phát lại LIST_SOURCES vài lần vì UDP có thể mất gói).
//     Người dùng đang đứng nhìn màn hình chờ, nên đừng nới hạn này lên.
//   - PHẢI gọi ngoài UI thread. Chặn UI thread 3 giây là đủ để Android dựng hộp
//     thoại ANR. Phía Kotlin đã bọc sẵn trong Dispatchers.IO (NativeClient.kt).
//
// Ý NGHĨA CỦA GIÁ TRỊ TRẢ VỀ
//   false = không mở được socket, hoặc host im lặng suốt 3 giây. Caller hiểu là
//   "host bản cũ / chỉ có một nguồn" và cứ xem nguồn 0 — KHÔNG coi là lỗi tử vong,
//   vì host đời trước GĐ6 hoàn toàn không biết thông điệp LIST_SOURCES.
//
// LIÊN QUAN: deskhub/wire/Wire.h (BuildListSources/ParseSourceList), JniBridge.cpp
//            (người gọi), client/windows/ClientLoop.cpp (bản song song)
// =============================================================================
#include <vector>

#include "net/UdpSocket.h"

#include "deskhub/wire/Wire.h"

// CHẶN tới ~3 giây. Trả false nếu không mở được socket hoặc host im lặng — caller
// hiểu là "host bản cũ / chỉ có một nguồn" và cứ xem nguồn 0.
// Phải gọi ngoài UI thread.
bool QuerySources(const NetAddr& server, std::vector<deskhub::SourceInfo>& out);
