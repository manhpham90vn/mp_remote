#pragma once
//
// Kieu du lieu dung chung giua CaptureModule va cac module tieu thu (encoder, debug).
// Co tinh KHONG phu thuoc winrt - chi D3D11/COM thuan - de encoder khong phai keo winrt.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

// Mot frame vua bat duoc. `texture` nam trong VRAM (BGRA) va CHI hop le trong
// pham vi loi goi callback - consumer phai encode/copy ngay, khong duoc giu lai.
struct FrameInfo {
    ID3D11Texture2D* texture;      // BGRA8, con song trong VRAM khi callback chay
    uint32_t         width;
    uint32_t         height;
    uint64_t         timestampUs;  // thoi diem capture (SystemRelativeTime), micro giay
    uint64_t         frameId;      // so thu tu tang dan, bat dau tu 0
};
