#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MfEncoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

using Microsoft::WRL::ComPtr;

// In loi HRESULT roi return false/void.
#define MF_CHECK(expr, msg)                                                       \
    do {                                                                          \
        HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                        \
            std::printf("[MfEncoder] %s that bai: 0x%08lX\n", (msg),              \
                        (unsigned long)_hr);                                      \
            return false;                                                         \
        }                                                                         \
    } while (0)

struct MfEncoder::Impl {
    ComPtr<IMFSinkWriter>        writer;
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    DWORD                        streamIndex = 0;
    EncoderConfig                cfg{};
    UINT                         resetToken = 0;

    bool     mfStarted = false;
    bool     writing = false;
    bool     haveFirstTs = false;
    uint64_t firstTsUs = 0;
    uint64_t frameCount = 0;

    ~Impl() {
        if (writing && writer) writer->Finalize();
        writer.Reset();
        deviceManager.Reset();
        if (mfStarted) MFShutdown();
    }

    GUID SubtypeFor(Codec c) const {
        return (c == Codec::HEVC) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    }

    bool Init(ID3D11Device* device, const EncoderConfig& c) {
        cfg = c;

        // SinkWriter ghi container ra file, chua co duong lay NAL tho (GD3 se lam
        // IMFByteStream tuy bien neu can) -> khong dung duoc cho loopback/streaming.
        if (cfg.onPacket) {
            std::printf("[MfEncoder] Chua ho tro onPacket (can NVENC cho loopback).\n");
            return false;
        }

        // Device phai bao ve da luong vi SinkWriter chay tren luong rieng.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }

        MF_CHECK(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
        mfStarted = true;

        // DXGI device manager -> SinkWriter dung GPU nay de encode/chuyen mau.
        MF_CHECK(MFCreateDXGIDeviceManager(&resetToken, &deviceManager),
                 "MFCreateDXGIDeviceManager");
        MF_CHECK(deviceManager->ResetDevice(device, resetToken), "ResetDevice");

        // Attributes: bat hardware transform, gan device manager, tat throttling (do tre thap).
        ComPtr<IMFAttributes> attrs;
        MF_CHECK(MFCreateAttributes(&attrs, 4), "MFCreateAttributes");
        attrs->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, deviceManager.Get());
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

        MF_CHECK(MFCreateSinkWriterFromURL(cfg.outputPath.c_str(), nullptr, attrs.Get(),
                                           &writer),
                 "MFCreateSinkWriterFromURL");

        // --- Kieu dau ra: H.264/HEVC nen ---
        ComPtr<IMFMediaType> outType;
        MF_CHECK(MFCreateMediaType(&outType), "MFCreateMediaType(out)");
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, SubtypeFor(cfg.codec));
        outType->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrateBps);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (cfg.codec == Codec::H264) {
            outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        }
        MF_CHECK(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height),
                 "FRAME_SIZE(out)");
        MF_CHECK(MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, cfg.fps, 1),
                 "FRAME_RATE(out)");
        MF_CHECK(MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1),
                 "PAR(out)");
        MF_CHECK(writer->AddStream(outType.Get(), &streamIndex), "AddStream");

        // --- Kieu dau vao: BGRA tu WGC (SinkWriter tu chen bo chuyen mau -> NV12) ---
        ComPtr<IMFMediaType> inType;
        MF_CHECK(MFCreateMediaType(&inType), "MFCreateMediaType(in)");
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);  // B8G8R8A8
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MF_CHECK(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height),
                 "FRAME_SIZE(in)");
        MF_CHECK(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg.fps, 1),
                 "FRAME_RATE(in)");
        MF_CHECK(writer->SetInputMediaType(streamIndex, inType.Get(), nullptr),
                 "SetInputMediaType");

        MF_CHECK(writer->BeginWriting(), "BeginWriting");
        writing = true;
        std::printf("[MfEncoder] Khoi tao xong: %ux%u @%ufps, %.1f Mbps -> %ls\n",
                    cfg.width, cfg.height, cfg.fps, cfg.bitrateBps / 1e6,
                    cfg.outputPath.c_str());
        return true;
    }

    // Boc texture D3D11 thanh IMFSample de dua vao SinkWriter.
    bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool /*forceKeyframe*/) {
        if (!writing) return false;

        ComPtr<IMFMediaBuffer> buffer;
        MF_CHECK(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), frame, 0, FALSE, &buffer),
                 "MFCreateDXGISurfaceBuffer");

        // Phai dat current length, neu khong encoder coi buffer rong.
        ComPtr<IMF2DBuffer2> buf2d;
        if (SUCCEEDED(buffer.As(&buf2d))) {
            DWORD len = 0;
            if (SUCCEEDED(buf2d->GetContiguousLength(&len))) buffer->SetCurrentLength(len);
        }

        ComPtr<IMFSample> sample;
        MF_CHECK(MFCreateSample(&sample), "MFCreateSample");
        MF_CHECK(sample->AddBuffer(buffer.Get()), "AddBuffer");

        // Timestamp tuong doi tu frame dau, don vi 100ns.
        if (!haveFirstTs) { firstTsUs = timestampUs; haveFirstTs = true; }
        const LONGLONG timeHns = static_cast<LONGLONG>((timestampUs - firstTsUs) * 10ull);
        const LONGLONG durHns = static_cast<LONGLONG>(10'000'000ull / (cfg.fps ? cfg.fps : 60));
        sample->SetSampleTime(timeHns);
        sample->SetSampleDuration(durHns);

        MF_CHECK(writer->WriteSample(streamIndex, sample.Get()), "WriteSample");
        ++frameCount;
        return true;
    }

    void Finish() {
        if (writing && writer) {
            writer->Finalize();
            writing = false;
            std::printf("[MfEncoder] Da ghi %llu frame, finalize xong.\n",
                        (unsigned long long)frameCount);
        }
    }
};

MfEncoder::MfEncoder() = default;
MfEncoder::~MfEncoder() = default;

bool MfEncoder::Init(ID3D11Device* device, const EncoderConfig& cfg) {
    impl_ = std::make_unique<Impl>();
    if (!impl_->Init(device, cfg)) { impl_.reset(); return false; }
    return true;
}

bool MfEncoder::Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) {
    return impl_ && impl_->Encode(frame, timestampUs, forceKeyframe);
}

void MfEncoder::Finish() { if (impl_) impl_->Finish(); }
