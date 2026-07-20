#pragma once
// ClientLoop - vai trò CLIENT (--connect ip[:port]): UdpSocket + Reassembler +
// ClientSession (core) + MfDecoder + Renderer (GD2). Xem docs/06 §5.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

#include "UdpSocket.h"

struct ClientOptions {
    NetAddr server;        // địa chỉ agent (--connect)
    bool    saveBmp = false;
    bool    sendInput = true; // GD4: đẩy phím/chuột tới host (--noinput để tắt)
};

// Kết nối tới agent, hiện video tới khi người dùng đóng cửa sổ / Ctrl+C /
// mất kết nối. Trả về exit code cho main.
int RunClient(const ClientOptions& opt);
