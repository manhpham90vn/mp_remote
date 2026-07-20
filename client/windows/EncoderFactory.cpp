// Factory chọn backend encoder theo chuỗi ưu tiên.
//
// Thứ tự: NVENC (NVIDIA, độ trễ thấp nhất) -> Media Foundation (NVIDIA/Intel/AMD/software).
// Khớp chuỗi phần cứng NVIDIA -> Intel -> CPU: nếu không có NVIDIA, NVENC Init thất bại
// và tự rơi xuống MF (MF lại tự chọn HW theo device, hoặc software).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "IVideoEncoder.h"
#include "NvencEncoder.h"
#include "MfEncoder.h"

#include <cstdio>

std::unique_ptr<IVideoEncoder> CreateEncoder(ID3D11Device* device, const EncoderConfig& cfg) {
    // 1. NVENC trước.
    {
        auto enc = std::make_unique<NvencEncoder>();
        if (enc->Init(device, cfg)) {
            std::printf("[Encoder] Using backend: %ls\n", enc->BackendName());
            return enc;
        }
        std::printf("[Encoder] NVENC unavailable, trying Media Foundation...\n");
    }
    // 2. Media Foundation (fallback).
    {
        auto enc = std::make_unique<MfEncoder>();
        if (enc->Init(device, cfg)) {
            std::printf("[Encoder] Using backend: %ls\n", enc->BackendName());
            return enc;
        }
    }
    std::printf("[Encoder] Failed to initialize any backend.\n");
    return nullptr;
}
