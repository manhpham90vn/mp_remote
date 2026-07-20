#pragma once
//
// Interface encoder video - tách khỏi backend cụ thể (Media Foundation / NVENC / ...).
// Cho phép đổi backend theo GPU (chuỗi NVIDIA -> Intel -> CPU) mà không sửa module gọi.
//
// Giai đoạn 1: xuất ra FILE (.mp4/.h264) để kiểm chứng bằng ffplay.
// Giai đoạn 3: sẽ thêm đường callback trả NAL để gọi lên mạng (không đổi interface này nhiều).
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

enum class Codec { H264, HEVC };
enum class RateControl { CBR, VBR };

// Nhận một gói NAL Annex-B vừa nén xong (1 frame). Chạy trên luồng gọi Encode().
// `data` chỉ hợp lệ trong phạm vi callback - copy/tiêu thụ ngay.
using PacketHandler = std::function<void(const uint8_t* data, size_t size,
                                         uint64_t timestampUs, bool keyframe)>;

struct EncoderConfig {
    Codec        codec = Codec::H264;
    uint32_t     width = 0;
    uint32_t     height = 0;
    uint32_t     fps = 60;
    uint32_t     bitrateBps = 20'000'000;
    RateControl  rc = RateControl::CBR;
    bool         lowLatency = true;
    std::wstring outputPath = L"output.mp4";  // rỗng = không ghi file
    // GD2+: đường NAL trong process (loopback) / lên mạng (GD3). Cả NVENC lẫn MF
    // (Encoder MFT thẳng, không qua SinkWriter) đều hỗ trợ.
    PacketHandler onPacket;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // Khởi tạo trên device ĐÃ CHỌN (chia sẻ với capture). Trả về false nếu backend không dùng được.
    virtual bool Init(ID3D11Device* device, const EncoderConfig& cfg) = 0;

    // Nén một frame VRAM. `timestampUs` từ capture. `forceKeyframe` xin IDR (dùng khi mất gói).
    virtual bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) = 0;

    // Flush + finalize (ghi xong file / đóng stream).
    virtual void Finish() = 0;

    virtual const wchar_t* BackendName() const = 0;
};

// Factory: thử các backend theo thứ tự, trả về cái đầu tiên Init thành công.
// Hiện tại: Media Foundation (tự chọn HW theo device: NVENC/QSV, hoặc software).
std::unique_ptr<IVideoEncoder> CreateEncoder(ID3D11Device* device, const EncoderConfig& cfg);
