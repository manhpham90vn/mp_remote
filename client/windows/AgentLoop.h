#pragma once
// AgentLoop - vai trò HOST: capture + encoder (GD2) + Packetizer + HostSession
// (core) + UdpSocket. Xem docs/06-phase3-transport.md §4.
//
// GD6: chia sẻ NHIỀU nguồn cùng lúc (cửa sổ và/hoặc cả màn hình) trên MỘT cổng UDP.
// Mỗi nguồn có sourceId riêng và mỗi cặp (client, nguồn) là một phiên độc lập —
// xem chú thích ở rgc::SourceInfo (core/include/rgc/Wire.h) về lý do chọn cách này
// thay vì nhét streamId vào header video.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "WindowCapture.h"

struct AgentOptions {
    uint16_t port = 47777;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 20;
    bool     allowInput = true; // GD4: cho client điều khiển
};

// Một nguồn được chia sẻ. `name` là tên hiện ở danh sách phía client (UTF-8).
struct AgentSource {
    CaptureTarget target;
    std::string   name;
};

// Chạy agent phục vụ `sources` tới khi mọi nguồn đóng / Ctrl+C / lỗi.
// Trả về exit code cho main.
int RunAgent(std::span<const AgentSource> sources, const AgentOptions& opt);
