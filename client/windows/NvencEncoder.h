#pragma once
//
// Backend encoder dung NVIDIA NVENC (Video Codec SDK).
//
// Vi sao NVENC cho Agent NVIDIA:
//   - Do tre thap nhat (preset ULTRA_LOW_LATENCY), dung cho game streaming.
//   - Xuat NAL Annex-B roi -> hop de packetize gui UDP (GD3).
//   - forceKeyframe (FORCEIDR) chuan -> phuc hoi khi client mat goi.
//   - Dang ky texture D3D11 truc tiep (zero-copy tu VRAM).
//
// Nap nvEncodeAPI64.dll DONG (di kem driver) nen khong can .lib; chi can header SDK.
// Giai doan 1: ghi NAL ra file .h264 (Annex-B) de kiem chung bang ffplay.
//
#include "IVideoEncoder.h"

class NvencEncoder : public IVideoEncoder {
public:
    NvencEncoder();
    ~NvencEncoder() override;

    bool Init(ID3D11Device* device, const EncoderConfig& cfg) override;
    bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) override;
    void Finish() override;
    const wchar_t* BackendName() const override { return L"NVENC (NVIDIA)"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
