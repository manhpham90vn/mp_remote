#pragma once
// AgentLoop - vai tro HOST (--serve): capture + NVENC (GD2) + Packetizer +
// HostSession (core) + UdpSocket. Xem docs/06-phase3-transport.md §4.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

struct AgentOptions {
    uint16_t port = 47777;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 20;
};

// Chay agent phuc vu cua so `target` toi khi cua so dong / Ctrl+C / loi pipeline.
// Tra ve exit code cho main.
int RunAgent(HWND target, const AgentOptions& opt);
