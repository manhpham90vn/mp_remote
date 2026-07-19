#pragma once
//
// Backend decoder dung Media Foundation (H.264 decoder MFT + D3D11VA).
//
// Vi sao MF cho decode:
//   - Co san trong Windows SDK, khong can SDK ngoai (khac voi NVDEC).
//   - MFT cua Microsoft la D3D11-aware: gan DXGI device manager la no giai ma
//     bang hardware (D3D11VA) va tra texture NV12 ngay trong VRAM - zero-copy
//     sang Renderer.
//   - CODECAPI_AVLowLatencyMode = TRUE de MFT tra frame ngay, khong giu buffer.
//
// Dung MFT truc tiep (ProcessInput/ProcessOutput dong bo) thay vi Source Reader
// vi dau vao la NAL tho tu encoder/mang, khong phai file container.
//
#include "IVideoDecoder.h"

class MfDecoder : public IVideoDecoder {
public:
    MfDecoder();
    ~MfDecoder() override;

    bool Init(ID3D11Device* device, const DecoderConfig& cfg,
              FrameHandler onFrame) override;
    bool Decode(const uint8_t* data, size_t size, uint64_t timestampUs) override;
    const wchar_t* BackendName() const override { return L"Media Foundation (D3D11VA)"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
