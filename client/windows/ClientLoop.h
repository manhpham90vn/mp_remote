#pragma once
// ClientLoop - vai trò CLIENT: UdpSocket + Reassembler + ClientSession (core) +
// MfDecoder + Renderer (GD2). Xem docs/06 §5.
//
// GD6: xem NHIỀU nguồn của cùng một host cùng lúc. Mỗi nguồn là một phiên độc lập
// (socket riêng, ClientSession riêng, cửa sổ preview riêng) — xem chú thích ở
// rgc::SourceInfo về lý do không dùng streamId chung một phiên.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <vector>

#include "UdpSocket.h"
#include "rgc/Wire.h"

struct ClientOptions {
    NetAddr server;           // địa chỉ agent
    bool    saveBmp = false;
    bool    sendInput = true; // GD4: đẩy phím/chuột tới host
    // Nguồn muốn xem (từ QueryHostSources). Rỗng = xem nguồn 0 — giữ đường cũ chạy
    // được với host chỉ chia sẻ một thứ.
    std::vector<rgc::SourceInfo> sources;
};

// Hỏi host đang chia sẻ những gì. Trả false nếu host không trả lời trong ~3s
// (sai IP, firewall chặn, hoặc host bản cũ không biết LIST_SOURCES).
bool QueryHostSources(const NetAddr& server, std::vector<rgc::SourceInfo>& out);

// Kết nối tới agent, hiện video tới khi người dùng đóng hết cửa sổ preview /
// Ctrl+C / mất kết nối. Trả về exit code cho main.
int RunClient(const ClientOptions& opt);
