#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "MfDecoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

#define MFD_CHECK(expr, msg)                                                      \
    do {                                                                          \
        HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                        \
            std::printf("[MfDecoder] %s failed: 0x%08lX\n", (msg),                \
                        (unsigned long)_hr);                                      \
            return false;                                                         \
        }                                                                         \
    } while (0)

struct MfDecoder::Impl {
    ComPtr<IMFTransform>         mft;
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    FrameHandler                 onFrame;
    DecoderConfig                cfg{};
    UINT                         resetToken = 0;
    bool                         mfStarted = false;
    bool                         streaming = false;
    uint32_t                     outWidth = 0, outHeight = 0;
    uint64_t                     framesOut = 0;

    ~Impl() {
        if (mft && streaming) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        mft.Reset();
        deviceManager.Reset();
        if (mfStarted) MFShutdown();
    }

    GUID SubtypeFor(Codec c) const {
        return (c == Codec::HEVC) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    }

    bool Init(ID3D11Device* device, const DecoderConfig& c, FrameHandler handler) {
        cfg = c;
        onFrame = std::move(handler);

        // MFT + video processor chạm vào immediate context từ nhiều luồng -> bật khóa.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }

        MFD_CHECK(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
        mfStarted = true;

        // Tìm decoder MFT ĐỒNG BỘ (MFT async của vendor cần event-loop riêng, không cần
        // thiết: MFT của Microsoft vẫn decode hardware qua D3D11VA khi có device manager).
        MFT_REGISTER_TYPE_INFO inInfo{ MFMediaType_Video, SubtypeFor(cfg.codec) };
        MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_NV12 };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        MFD_CHECK(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                            MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                            &inInfo, &outInfo, &activates, &count),
                  "MFTEnumEx");
        if (count == 0) { std::printf("[MfDecoder] No decoder MFT found.\n"); return false; }
        HRESULT hr = activates[0]->ActivateObject(IID_PPV_ARGS(&mft));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        MFD_CHECK(hr, "ActivateObject");

        // Gán D3D11 device -> decode hardware, output NV12 nằm ngay trong VRAM.
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft->GetAttributes(&attrs))) {
            UINT32 aware = 0;
            attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
            if (!aware) {
                std::printf("[MfDecoder] MFT does not support D3D11 - unusable.\n");
                return false;
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);  // trả frame ngay, không giữ buffer
        }
        MFD_CHECK(MFCreateDXGIDeviceManager(&resetToken, &deviceManager),
                  "MFCreateDXGIDeviceManager");
        MFD_CHECK(deviceManager->ResetDevice(device, resetToken), "ResetDevice");
        MFD_CHECK(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                      (ULONG_PTR)deviceManager.Get()),
                  "SET_D3D_MANAGER");

        // Kiểu đầu vào: H.264/HEVC elementary stream (Annex-B).
        ComPtr<IMFMediaType> inType;
        MFD_CHECK(MFCreateMediaType(&inType), "MFCreateMediaType(in)");
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, SubtypeFor(cfg.codec));
        if (cfg.width && cfg.height) {
            MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
        }
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg.fps ? cfg.fps : 60, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
        MFD_CHECK(mft->SetInputType(0, inType.Get(), 0), "SetInputType");

        if (!NegotiateOutputType()) return false;

        MFD_CHECK(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                  "NOTIFY_BEGIN_STREAMING");
        MFD_CHECK(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                  "NOTIFY_START_OF_STREAM");
        streaming = true;

        std::printf("[MfDecoder] Initialized: %s, D3D11VA, low-latency.\n",
                    cfg.codec == Codec::HEVC ? "HEVC" : "H264");
        return true;
    }

    // Chọn output NV12 trong danh sách MFT đề nghị. Gọi lại mỗi khi stream đổi
    // (MF_E_TRANSFORM_STREAM_CHANGE - ví dụ SPS báo kích thước khác).
    bool NegotiateOutputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
            if (hr == MF_E_NO_MORE_TYPES) break;
            MFD_CHECK(hr, "GetOutputAvailableType");
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                MFD_CHECK(mft->SetOutputType(0, t.Get(), 0), "SetOutputType");
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &outWidth, &outHeight);
                return true;
            }
        }
        std::printf("[MfDecoder] MFT does not offer NV12.\n");
        return false;
    }

    bool Decode(const uint8_t* data, size_t size, uint64_t timestampUs) {
        if (!streaming || !data || size == 0) return false;

        ComPtr<IMFMediaBuffer> buffer;
        MFD_CHECK(MFCreateMemoryBuffer((DWORD)size, &buffer), "MFCreateMemoryBuffer");
        BYTE* dst = nullptr;
        MFD_CHECK(buffer->Lock(&dst, nullptr, nullptr), "Lock");
        std::memcpy(dst, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD)size);

        ComPtr<IMFSample> sample;
        MFD_CHECK(MFCreateSample(&sample), "MFCreateSample");
        MFD_CHECK(sample->AddBuffer(buffer.Get()), "AddBuffer");
        sample->SetSampleTime((LONGLONG)(timestampUs * 10ull));  // 100ns
        sample->SetSampleDuration(10'000'000ll / (cfg.fps ? cfg.fps : 60));

        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // MFT đầy output -> rút hết rồi nạp lại.
            if (!DrainOutputs()) return false;
            hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        MFD_CHECK(hr, "ProcessInput");

        return DrainOutputs();
    }

    bool DrainOutputs() {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO si{};
            MFD_CHECK(mft->GetOutputStreamInfo(0, &si), "GetOutputStreamInfo");
            const bool mftProvides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            if (!mftProvides) {
                // Đường CPU (không D3D) không hỗ trợ ở GD2 - cần VRAM cho renderer.
                std::printf("[MfDecoder] MFT does not provide D3D samples - unsupported.\n");
                return false;
            }

            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = 0;
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!NegotiateOutputType()) return false;
                continue;
            }
            if (FAILED(hr)) {
                if (ob.pSample) ob.pSample->Release();
                std::printf("[MfDecoder] ProcessOutput failed: 0x%08lX\n", (unsigned long)hr);
                return false;
            }

            ComPtr<IMFSample> outSample;
            outSample.Attach(ob.pSample);  // MFT cấp sample, mình giữ 1 ref -> tự release
            if (outSample && !Deliver(outSample.Get())) return false;
        }
    }

    // Rút texture NV12 + subresource từ sample và gọi callback.
    bool Deliver(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        MFD_CHECK(sample->GetBufferByIndex(0, &buffer), "GetBufferByIndex");
        ComPtr<IMFDXGIBuffer> dxgiBuf;
        MFD_CHECK(buffer.As(&dxgiBuf), "IMFDXGIBuffer");

        ComPtr<ID3D11Texture2D> tex;
        MFD_CHECK(dxgiBuf->GetResource(IID_PPV_ARGS(&tex)), "GetResource");
        UINT subresource = 0;
        dxgiBuf->GetSubresourceIndex(&subresource);

        LONGLONG timeHns = 0;
        sample->GetSampleTime(&timeHns);

        DecodedFrame df{};
        df.texture = tex.Get();
        df.subresource = subresource;
        df.width = outWidth;
        df.height = outHeight;
        df.timestampUs = (uint64_t)timeHns / 10ull;
        ++framesOut;
        if (onFrame) onFrame(df);
        return true;
    }
};

MfDecoder::MfDecoder() = default;
MfDecoder::~MfDecoder() = default;

bool MfDecoder::Init(ID3D11Device* device, const DecoderConfig& cfg, FrameHandler onFrame) {
    impl_ = std::make_unique<Impl>();
    if (!impl_->Init(device, cfg, std::move(onFrame))) { impl_.reset(); return false; }
    return true;
}

bool MfDecoder::Decode(const uint8_t* data, size_t size, uint64_t timestampUs) {
    return impl_ && impl_->Decode(data, size, timestampUs);
}

// Factory (backend duy nhất hiện tại). Sang GD3 nếu cần NVDEC thì thêm vào đây.
std::unique_ptr<IVideoDecoder> CreateDecoder(ID3D11Device* device, const DecoderConfig& cfg,
                                             IVideoDecoder::FrameHandler onFrame) {
    auto dec = std::make_unique<MfDecoder>();
    if (dec->Init(device, cfg, std::move(onFrame))) {
        std::printf("[Decoder] Using backend: %ls\n", dec->BackendName());
        return dec;
    }
    std::printf("[Decoder] Failed to initialize any backend.\n");
    return nullptr;
}
