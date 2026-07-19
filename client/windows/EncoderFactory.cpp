// Factory chon backend encoder theo chuoi uu tien.
//
// Thu tu: NVENC (NVIDIA, do tre thap nhat) -> Media Foundation (NVIDIA/Intel/AMD/software).
// Khop chuoi phan cung NVIDIA -> Intel -> CPU: neu khong co NVIDIA, NVENC Init that bai
// va tu roi xuong MF (MF lai tu chon HW theo device, hoac software).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "IVideoEncoder.h"
#include "NvencEncoder.h"
#include "MfEncoder.h"

#include <cstdio>

std::unique_ptr<IVideoEncoder> CreateEncoder(ID3D11Device* device, const EncoderConfig& cfg) {
    // 1. NVENC truoc.
    {
        auto enc = std::make_unique<NvencEncoder>();
        if (enc->Init(device, cfg)) {
            std::printf("[Encoder] Dung backend: %ls\n", enc->BackendName());
            return enc;
        }
        std::printf("[Encoder] NVENC khong dung duoc, thu Media Foundation...\n");
    }
    // 2. Media Foundation (fallback).
    {
        auto enc = std::make_unique<MfEncoder>();
        if (enc->Init(device, cfg)) {
            std::printf("[Encoder] Dung backend: %ls\n", enc->BackendName());
            return enc;
        }
    }
    std::printf("[Encoder] Khong khoi tao duoc backend nao.\n");
    return nullptr;
}
