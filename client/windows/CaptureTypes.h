#pragma once
//
// Kiểu dữ liệu dùng chung giữa CaptureModule và các module tiêu thụ (encoder, debug).
// Cố tình KHÔNG phụ thuộc winrt - chỉ D3D11/COM thuần - để encoder không phải kéo winrt.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

// Một frame vừa bắt được. `texture` nằm trong VRAM (BGRA) và CHỈ hợp lệ trong
// phạm vi lời gọi callback - consumer phải encode/copy ngay, không được giữ lại.
struct FrameInfo {
    ID3D11Texture2D* texture;      // BGRA8, còn sống trong VRAM khi callback chạy
    uint32_t         width;
    uint32_t         height;
    uint64_t         timestampUs;  // thời điểm capture (SystemRelativeTime), micro giây
    uint64_t         frameId;      // số thứ tự tăng dần, bắt đầu từ 0
};
