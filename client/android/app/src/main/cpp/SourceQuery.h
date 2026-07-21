#pragma once
//
// SourceQuery — hỏi host đang chia sẻ những cửa sổ nào (LIST_SOURCES -> SOURCE_LIST).
// Bản port của QueryHostSources() trong client/windows/ClientLoop.cpp.
//
// Đứng ngoài ClientLoop vì nó chạy TRƯỚC khi có phiên: mở socket riêng, không
// sessionId, không thread — gọi xong là xong. Nhét vào ClientLoop chỉ tổ buộc lớp
// đó phải biết một trạng thái "chưa kết nối" mà nó không dùng đến.
//
#include <vector>

#include "UdpSocket.h"

#include "rgc/Wire.h"

// CHẶN tới ~3 giây. Trả false nếu không mở được socket hoặc host im lặng — caller
// hiểu là "host bản cũ / chỉ có một nguồn" và cứ xem nguồn 0.
// Phải gọi ngoài UI thread.
bool QuerySources(const NetAddr& server, std::vector<rgc::SourceInfo>& out);
