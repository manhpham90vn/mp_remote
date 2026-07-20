#pragma once
//
// Backend encoder dùng NVIDIA NVENC (Video Codec SDK).
//
// Vì sao NVENC cho Agent NVIDIA:
//   - Độ trễ thấp nhất (preset ULTRA_LOW_LATENCY), dùng cho game streaming.
//   - Xuất NAL Annex-B rời -> hợp để packetize gửi UDP (GD3).
//   - forceKeyframe (FORCEIDR) chuẩn -> phục hồi khi client mất gói.
//   - Đăng ký texture D3D11 trực tiếp (zero-copy từ VRAM).
//
// Nạp nvEncodeAPI64.dll ĐỘNG (đi kèm driver) nên không cần .lib; chỉ cần header SDK.
// Giai đoạn 1: ghi NAL ra file .h264 (Annex-B) để kiểm chứng bằng ffplay.
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
