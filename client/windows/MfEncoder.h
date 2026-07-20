#pragma once
//
// Backend encoder dùng thẳng H.264/HEVC Encoder MFT (Media Foundation), không qua
// IMFSinkWriter/container - để xuất được NAL Annex-B thô cho callback onPacket (streaming).
//
// Vì sao MF cho agent không-NVIDIA (Intel QSV / AMD VCE / software fallback):
//   - Có sẵn trong Windows SDK, không cần SDK hãng thứ ba.
//   - MFTEnumEx tự tìm MFT phù hợp device D3D11 đang dùng (ưu tiên HW nhờ SORTANDFILTER).
//   - Đầu vào NV12 từ chính D3D11 device đang capture (VRAM) - tự chuyển màu BGRA->NV12
//     bằng D3D11 Video Processor (không đồng bộ CPU).
//   - MFT phần cứng thường BẤT ĐỒNG BỘ (async) - phải unlock + bắt sự kiện
//     (IMFMediaEventGenerator) trước khi gọi ProcessInput/ProcessOutput.
//   - SPS/PPS lấy từ MF_MT_MPEG_SEQUENCE_HEADER của kiểu đầu ra, tự chèn trước mỗi IDR
//     (tương đương repeatSPSPPS của NVENC) để client join/phục hồi giữa chừng decode được.
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
