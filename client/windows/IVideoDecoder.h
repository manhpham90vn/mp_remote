#pragma once
//
// Interface decoder video - đối xứng với IVideoEncoder, tách khỏi backend cụ thể.
//
// Giai đoạn 2 (loopback): nhận NAL Annex-B từ encoder trong CÙNG process, giải mã
// bằng hardware (D3D11VA qua Media Foundation) ra texture NV12 trong VRAM, đưa cho
// Renderer vẽ lên swapchain. Giai đoạn 3 sẽ nhận NAL từ mạng thay vì loopback -
// interface này không đổi.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>

#include "IVideoEncoder.h"  // dùng lại enum Codec

struct DecoderConfig {
    Codec    codec = Codec::H264;
    uint32_t width = 0;   // kích thước gợi ý; decoder tự đọc lại từ SPS khi stream đổi
    uint32_t height = 0;
    uint32_t fps = 60;
};

// Một frame vừa giải mã xong. `texture` là NV12 trong VRAM, thường là một phần tử
// của texture-array trong pool của decoder -> phải dùng kèm `subresource`.
// CHỈ hợp lệ trong phạm vi callback - render/copy ngay, không giữ lại.
struct DecodedFrame {
    ID3D11Texture2D* texture;      // NV12, còn sống trong VRAM khi callback chạy
    UINT             subresource;  // array slice trong pool của decoder
    uint32_t         width;
    uint32_t         height;
    uint64_t         timestampUs;  // timestamp truyền xuyên suốt từ capture
};

class IVideoDecoder {
public:
    // Callback mỗi khi giải mã xong một frame. Chạy trên luồng gọi Decode().
    using FrameHandler = std::function<void(const DecodedFrame&)>;

    virtual ~IVideoDecoder() = default;

    // Khởi tạo trên device dùng chung (cùng device với encoder ở loopback;
    // sang GD3 là device riêng của client). false nếu backend không dùng được.
    virtual bool Init(ID3D11Device* device, const DecoderConfig& cfg,
                      FrameHandler onFrame) = 0;

    // Nạp một gói NAL Annex-B (1 frame nén). Frame giải mã xong trả về qua onFrame
    // (có thể 0 hoặc nhiều frame mỗi lần gọi, tùy decoder giữ trễ bao nhiêu).
    virtual bool Decode(const uint8_t* data, size_t size, uint64_t timestampUs) = 0;

    virtual const wchar_t* BackendName() const = 0;
};

// Factory: hiện tại chỉ có backend Media Foundation (D3D11VA hardware decode).
std::unique_ptr<IVideoDecoder> CreateDecoder(ID3D11Device* device, const DecoderConfig& cfg,
                                             IVideoDecoder::FrameHandler onFrame);
