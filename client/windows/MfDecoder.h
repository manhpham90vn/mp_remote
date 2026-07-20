#pragma once
//
// Backend decoder dùng Media Foundation (H.264 decoder MFT + D3D11VA).
//
// Vì sao MF cho decode:
//   - Có sẵn trong Windows SDK, không cần SDK ngoài (khác với NVDEC).
//   - MFT của Microsoft là D3D11-aware: gắn DXGI device manager là nó giải mã
//     bằng hardware (D3D11VA) và trả texture NV12 ngay trong VRAM - zero-copy
//     sang Renderer.
//   - CODECAPI_AVLowLatencyMode = TRUE để MFT trả frame ngay, không giữ buffer.
//
// Dùng MFT trực tiếp (ProcessInput/ProcessOutput đồng bộ) thay vì Source Reader
// vì đầu vào là NAL thô từ encoder/mạng, không phải file container.
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
