#pragma once
// ClientLoop - vai tro CLIENT (--connect ip[:port]): UdpSocket + Reassembler +
// ClientSession (core) + MfDecoder + Renderer (GD2). Xem docs/06 §5.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

#include "UdpSocket.h"

struct ClientOptions {
    NetAddr server;        // dia chi agent (--connect)
    bool    saveBmp = false;
};

// Ket noi toi agent, hien video toi khi nguoi dung dong cua so / Ctrl+C /
// mat ket noi. Tra ve exit code cho main.
int RunClient(const ClientOptions& opt);
