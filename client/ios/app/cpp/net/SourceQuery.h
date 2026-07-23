#pragma once
// =============================================================================
// SourceQuery.h — hỏi host đang chia sẻ những cửa sổ nào (LIST_SOURCES → SOURCE_LIST).
//                 Bản iOS, CHÉP từ client/android/.../net/SourceQuery.h.
//
// NHIỆM VỤ
//   Một lần trao đổi hỏi-đáp duy nhất, chạy TRƯỚC khi có phiên. Kết quả là danh
//   sách cửa sổ để người dùng chọn xem cái nào.
//
// VỊ TRÍ TRONG LUỒNG NGƯỜI DÙNG
//   ConnectView: gõ địa chỉ → **QuerySources()** → chọn nguồn → StreamView
//                                                               → ClientLoop::Start()
//
// VÌ SAO ĐỨNG NGOÀI ClientLoop
//   Nó chạy trước khi có phiên: mở socket riêng, không sessionId, không thread —
//   gọi xong là xong.
//
// TÍNH CHẤT CẦN BIẾT TRƯỚC KHI GỌI
//   - CHẶN tới ~3 giây (phát lại LIST_SOURCES vài lần vì UDP có thể mất gói).
//   - PHẢI gọi ngoài main thread. Chặn main thread 3 giây làm treo UI. Phía Swift
//     đã bọc trong Task.detached (DeskhubClient).
//
// Ý NGHĨA CỦA GIÁ TRỊ TRẢ VỀ
//   false = không mở được socket, hoặc host im lặng suốt 3 giây. Caller hiểu là
//   "host bản cũ / chỉ có một nguồn" và cứ xem nguồn 0 — KHÔNG coi là lỗi tử vong.
//
// LIÊN QUAN: deskhub/wire/Wire.h (BuildListSources/ParseSourceList), DeskhubClient.mm
//            (người gọi), client/android/.../net/SourceQuery.h (bản song song)
// =============================================================================
#include <vector>

#include "net/UdpSocket.h"

#include "deskhub/wire/Wire.h"

// CHẶN tới ~3 giây. Trả false nếu không mở được socket hoặc host im lặng — caller
// hiểu là "host bản cũ / chỉ có một nguồn" và cứ xem nguồn 0.
// Phải gọi ngoài main thread.
bool QuerySources(const NetAddr& server, std::vector<deskhub::SourceInfo>& out);
