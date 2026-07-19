#pragma once
//
// Interface encoder video - tach khoi backend cu the (Media Foundation / NVENC / ...).
// Cho phep doi backend theo GPU (chuoi NVIDIA -> Intel -> CPU) ma khong sua module goi.
//
// Giai doan 1: xuat ra FILE (.mp4/.h264) de kiem chung bang ffplay.
// Giai doan 3: se them duong callback tra NAL de goi len mang (khong doi interface nay nhieu).
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

// Nhan mot goi NAL Annex-B vua nen xong (1 frame). Chay tren luong goi Encode().
// `data` chi hop le trong pham vi callback - copy/tieu thu ngay.
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
    std::wstring outputPath = L"output.mp4";  // rong = khong ghi file
    // GD2+: duong NAL trong process (loopback) / len mang (GD3). Hien chi backend
    // NVENC ho tro; MF con ghi container qua SinkWriter nen Init that bai neu set.
    PacketHandler onPacket;
};

class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    // Khoi tao tren device DA CHON (chia se voi capture). Tra ve false neu backend khong dung duoc.
    virtual bool Init(ID3D11Device* device, const EncoderConfig& cfg) = 0;

    // Nen mot frame VRAM. `timestampUs` tu capture. `forceKeyframe` xin IDR (dung khi mat goi).
    virtual bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) = 0;

    // Flush + finalize (ghi xong file / dong stream).
    virtual void Finish() = 0;

    virtual const wchar_t* BackendName() const = 0;
};

// Factory: thu cac backend theo thu tu, tra ve cai dau tien Init thanh cong.
// Hien tai: Media Foundation (tu chon HW theo device: NVENC/QSV, hoac software).
std::unique_ptr<IVideoEncoder> CreateEncoder(ID3D11Device* device, const EncoderConfig& cfg);
