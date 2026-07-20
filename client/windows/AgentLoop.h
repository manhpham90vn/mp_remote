#pragma once
// AgentLoop - vai trò HOST (--serve): capture + NVENC (GD2) + Packetizer +
// HostSession (core) + UdpSocket. Xem docs/06-phase3-transport.md §4.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

struct AgentOptions {
    uint16_t port = 47777;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 20;
    bool     allowInput = true; // GD4: cho client điều khiển (--noinput để tắt)
};

// Chạy agent phục vụ cửa sổ `target` tới khi cửa sổ đóng / Ctrl+C / lỗi pipeline.
// Trả về exit code cho main.
int RunAgent(HWND target, const AgentOptions& opt);
