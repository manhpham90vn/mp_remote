#pragma once
// =============================================================================
// AgentLoop.h — vai trò HOST: giao diện gọi vào phía chia sẻ.
//
// NHIỆM VỤ
//   Khai báo đúng ba thứ: nguồn chia sẻ (AgentSource), tuỳ chọn phiên
//   (AgentOptions), và hàm chạy phiên (RunAgent). Toàn bộ phần ghép nối nằm ở
//   AgentLoop.cpp — đọc header khối ở đó để hiểu kiến trúc luồng.
//
// VỊ TRÍ TRONG KIẾN TRÚC
//   MainMenuWindow → WindowPickerDialog → **RunAgent()**
//   RunAgent CHẶN tới khi người dùng kết thúc phiên / Ctrl+C / lỗi, rồi trả exit
//   code. Trong lúc chạy nó mở một cửa sổ quản lý phiên (ui/SessionWindow.h):
//   `sources` chỉ là danh sách BAN ĐẦU — người dùng thêm/bớt nguồn giữa phiên
//   bằng các nút Add / Stop selected trên cửa sổ đó.
//
// GĐ6: NHIỀU NGUỒN, MỘT CỔNG
//   Chia sẻ nhiều cửa sổ và/hoặc cả màn hình cùng lúc trên MỘT cổng UDP. Mỗi nguồn
//   có sourceId riêng, và mỗi cặp (client, nguồn) là một PHIÊN ĐỘC LẬP với
//   sessionId riêng — không nhét streamId vào header video. Lý do đầy đủ ở chú
//   thích của deskhub::SourceInfo trong core/include/deskhub/wire/Wire.h.
//
// LIÊN QUAN: AgentLoop.cpp (kiến trúc luồng + định tuyến gói), ClientLoop.h (phía
//            đối diện), capture/WindowCapture.h (CaptureTarget),
//            docs/06-phase3-transport.md §4
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "capture/WindowCapture.h"

struct AgentOptions {
    uint16_t port = 47777;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 20;
    bool allowInput = true; // GD4: cho client điều khiển
};

// Một nguồn được chia sẻ. `name` là tên hiện ở danh sách phía client (UTF-8).
struct AgentSource {
    CaptureTarget target;
    std::string name;
};

// Chạy agent phục vụ `sources` (danh sách ban đầu — thêm/bớt giữa phiên qua cửa
// sổ quản lý) tới khi người dùng kết thúc phiên / Ctrl+C / lỗi.
// Trả về exit code cho main.
int RunAgent(std::span<const AgentSource> sources, const AgentOptions& opt);
