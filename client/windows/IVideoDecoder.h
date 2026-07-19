#pragma once
//
// Interface decoder video - doi xung voi IVideoEncoder, tach khoi backend cu the.
//
// Giai doan 2 (loopback): nhan NAL Annex-B tu encoder trong CUNG process, giai ma
// bang hardware (D3D11VA qua Media Foundation) ra texture NV12 trong VRAM, dua cho
// Renderer ve len swapchain. Giai doan 3 se nhan NAL tu mang thay vi loopback -
// interface nay khong doi.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>

#include "IVideoEncoder.h"  // dung lai enum Codec

struct DecoderConfig {
    Codec    codec = Codec::H264;
    uint32_t width = 0;   // kich thuoc goi y; decoder tu doc lai tu SPS khi stream doi
    uint32_t height = 0;
    uint32_t fps = 60;
};

// Mot frame vua giai ma xong. `texture` la NV12 trong VRAM, thuong la mot phan tu
// cua texture-array trong pool cua decoder -> phai dung kem `subresource`.
// CHI hop le trong pham vi callback - render/copy ngay, khong giu lai.
struct DecodedFrame {
    ID3D11Texture2D* texture;      // NV12, con song trong VRAM khi callback chay
    UINT             subresource;  // array slice trong pool cua decoder
    uint32_t         width;
    uint32_t         height;
    uint64_t         timestampUs;  // timestamp truyen xuyen suot tu capture
};

class IVideoDecoder {
public:
    // Callback moi khi giai ma xong mot frame. Chay tren luong goi Decode().
    using FrameHandler = std::function<void(const DecodedFrame&)>;

    virtual ~IVideoDecoder() = default;

    // Khoi tao tren device dung chung (cung device voi encoder o loopback;
    // sang GD3 la device rieng cua client). false neu backend khong dung duoc.
    virtual bool Init(ID3D11Device* device, const DecoderConfig& cfg,
                      FrameHandler onFrame) = 0;

    // Nap mot goi NAL Annex-B (1 frame nen). Frame giai ma xong tra ve qua onFrame
    // (co the 0 hoac nhieu frame moi lan goi, tuy decoder giu tre bao nhieu).
    virtual bool Decode(const uint8_t* data, size_t size, uint64_t timestampUs) = 0;

    virtual const wchar_t* BackendName() const = 0;
};

// Factory: hien tai chi co backend Media Foundation (D3D11VA hardware decode).
std::unique_ptr<IVideoDecoder> CreateDecoder(ID3D11Device* device, const DecoderConfig& cfg,
                                             IVideoDecoder::FrameHandler onFrame);
