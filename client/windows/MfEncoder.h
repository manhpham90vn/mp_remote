#pragma once
//
// Backend encoder dung Media Foundation SinkWriter.
//
// Vi sao MF cho Giai doan 1:
//   - Co san trong Windows SDK, khong can tai NVIDIA Video Codec SDK.
//   - Tu chon hardware MFT theo device D3D11: NVENC (NVIDIA) / QSV (Intel), hoac software (CPU).
//     => hien thuc san chuoi NVIDIA -> Intel -> CPU chi bang viec chon adapter o GpuSelect.
//   - Nhan thang texture D3D11 (VRAM), SinkWriter tu chuyen mau sang NV12 bang GPU.
//
// Gioi han: xuat ra file container (.mp4). Duong tra NAL de streaming se them o GD3.
//
#include "IVideoEncoder.h"

class MfEncoder : public IVideoEncoder {
public:
    MfEncoder();
    ~MfEncoder() override;

    bool Init(ID3D11Device* device, const EncoderConfig& cfg) override;
    bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) override;
    void Finish() override;
    const wchar_t* BackendName() const override { return L"Media Foundation (HW/SW auto)"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
