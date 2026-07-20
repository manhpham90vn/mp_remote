#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "MfEncoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <map>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

// In lỗi HRESULT rồi return false/void.
#define MF_CHECK(expr, msg)                                                       \
    do {                                                                          \
        HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                        \
            std::printf("[MfEncoder] %s failed: 0x%08lX\n", (msg),                \
                        (unsigned long)_hr);                                      \
            return false;                                                         \
        }                                                                         \
    } while (0)

// Biến thể trả -1 (dùng trong hàm trả int).
#define MF_CHECKI(expr, msg)                                                      \
    do {                                                                          \
        HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                        \
            std::printf("[MfEncoder] %s failed: 0x%08lX\n", (msg),                \
                        (unsigned long)_hr);                                      \
            return -1;                                                            \
        }                                                                         \
    } while (0)

struct MfEncoder::Impl {
    ComPtr<IMFActivate>           activate;     // giữ lại để tạo-lại transform khi cần (xin keyframe)
    ComPtr<IMFTransform>          mft;
    ComPtr<IMFMediaEventGenerator> events;      // chỉ dùng khi isAsync
    ComPtr<IMFDXGIDeviceManager>  deviceManager;
    ComPtr<ICodecAPI>             codecApi;     // rate control / force keyframe (không bắt buộc)

    ComPtr<ID3D11Device>          device;
    ComPtr<ID3D11DeviceContext>   context;
    ComPtr<ID3D11VideoDevice>     videoDevice;
    ComPtr<ID3D11VideoContext>    videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor>  vp;
    ComPtr<ID3D11Texture2D>       nv12Tex;       // scratch: BGRA (WGC) -> NV12 (đầu vào encoder)
    ComPtr<ID3D11VideoProcessorOutputView> vpOutView;
    // Cache input view theo texture nguồn: WGC dùng lại vài texture (pool depth 2).
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> vpInViews;

    EncoderConfig cfg{};
    UINT     resetToken = 0;
    bool     mfStarted = false;
    bool     streaming = false;
    bool     isAsync = false;
    bool     outputProvidesSamples = false;
    bool     haveFirstTs = false;
    uint64_t firstTsUs = 0;
    uint64_t frameCount = 0;
    uint64_t totalBytes = 0;
    FILE*    out = nullptr;
    std::vector<uint8_t> spsPps;  // extradata Annex-B (SPS+PPS), chèn trước mỗi IDR

    ~Impl() {
        if (mft && streaming) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        mft.Reset();
        events.Reset();
        codecApi.Reset();
        if (activate) activate->ShutdownObject();
        deviceManager.Reset();
        if (out) { std::fclose(out); out = nullptr; }
        if (mfStarted) MFShutdown();
    }

    GUID SubtypeFor(Codec c) const {
        return (c == Codec::HEVC) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    }

    // Tìm + chọn IMFActivate D3D11-aware phù hợp (chỉ làm 1 lần, giữ lại trong `activate`
    // để tạo-lại transform rẻ khi cần xin keyframe - xem ReinitTransform()).
    bool FindActivate() {
        MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, SubtypeFor(cfg.codec) };
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        MF_CHECK(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                           nullptr, &outInfo, &activates, &count),
                 "MFTEnumEx");
        if (count == 0) {
            std::printf("[MfEncoder] No encoder MFT found.\n");
            return false;
        }
        // MF_SA_D3D11_AWARE không đáng tin cậy trên attribute của IMFActivate (trước khi
        // activate) với vài driver - phải activate rồi đọc thuộc tính trên chính transform.
        for (UINT32 i = 0; i < count && !activate; ++i) {
            wchar_t name[256] = L"?";
            UINT32 nameLen = 0;
            activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, &nameLen);

            ComPtr<IMFTransform> candidate;
            ComPtr<IMFAttributes> candidateAttrs;
            if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(&candidate))) ||
                FAILED(candidate->GetAttributes(&candidateAttrs))) {
                std::wprintf(L"[MfEncoder] Found MFT: %ls (activate failed)\n", name);
                continue;
            }
            UINT32 aware = 0;
            candidateAttrs->GetUINT32(MF_SA_D3D11_AWARE, &aware);
            std::wprintf(L"[MfEncoder] Found MFT: %ls (D3D11-aware=%u)\n", name, aware);
            if (!aware) { activates[i]->ShutdownObject(); continue; }
            activate = activates[i];
            mft = candidate;
        }
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (!activate) {
            std::printf("[MfEncoder] No D3D11-aware encoder MFT available.\n");
            return false;
        }
        return true;
    }

    // Cấu hình transform hiện có trong `mft` (kiểu đầu vào/ra, rate control, D3D manager,
    // BEGIN_STREAMING). Dùng chung cho Init() lần đầu và ReinitTransform() (xin keyframe).
    bool ConfigureTransform() {
        ComPtr<IMFAttributes> mftAttrs;
        MF_CHECK(mft->GetAttributes(&mftAttrs), "GetAttributes");

        // MFT phần cứng thường là async - phải mở khóa TRƯỚC khi gọi method nào khác.
        UINT32 asyncFlag = 0;
        mftAttrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
        isAsync = asyncFlag != 0;
        if (isAsync) {
            MF_CHECK(mftAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "ASYNC_UNLOCK");
            MF_CHECK(mft.As(&events), "IMFMediaEventGenerator");
        }

        MF_CHECK(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager.Get()),
                 "SET_D3D_MANAGER");

        // --- Kiểu đầu ra: H.264/HEVC nén (đặt trước input - encoder cần biết đích) ---
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
        MF_CHECK(mft->SetOutputType(0, outType.Get(), 0), "SetOutputType");

        // --- Kiểu đầu vào: NV12 (encoder phần cứng không nhận BGRA thẳng - tự chuyển màu) ---
        ComPtr<IMFMediaType> inType;
        MF_CHECK(MFCreateMediaType(&inType), "MFCreateMediaType(in)");
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MF_CHECK(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height),
                 "FRAME_SIZE(in)");
        MF_CHECK(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg.fps, 1),
                 "FRAME_RATE(in)");
        MF_CHECK(mft->SetInputType(0, inType.Get(), 0), "SetInputType");

        if (!SetupRateControl()) return false;

        MFT_OUTPUT_STREAM_INFO si{};
        MF_CHECK(mft->GetOutputStreamInfo(0, &si), "GetOutputStreamInfo");
        outputProvidesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                               MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MF_CHECK(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0),
                 "NOTIFY_BEGIN_STREAMING");
        MF_CHECK(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0),
                 "NOTIFY_START_OF_STREAM");
        streaming = true;
        return true;
    }

    // Tạo lại transform từ đầu (cùng 1 IMFActivate) để xin IDR: vài driver (Intel QSV ở
    // đây) không hỗ trợ force-keyframe qua ICodecAPI lẫn các thủ thuật FLUSH/SetOutputType
    // giữa chừng (đã thử, không ăn hoặc làm hỏng transform) - nhưng transform MỚI luôn
    // phát IDR ở sample đầu tiên, nên đây là đường chắc chắn duy nhất còn lại.
    bool ReinitTransform() {
        if (mft && streaming) {
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        mft.Reset();
        events.Reset();
        codecApi.Reset();
        streaming = false;
        if (!activate) return false;
        activate->ShutdownObject();
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&mft)))) {
            std::printf("[MfEncoder] Failed to recreate encoder for keyframe request.\n");
            return false;
        }
        spsPps.clear();  // transform mới - lấy lại extradata riêng của nó
        return ConfigureTransform();
    }

    bool Init(ID3D11Device* dev, const EncoderConfig& c) {
        cfg = c;
        device = dev;
        device->GetImmediateContext(&context);

        // MFT + video processor chạm vào immediate context từ nhiều luồng -> bật khóa.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }

        MF_CHECK(MFStartup(MF_VERSION, MFSTARTUP_LITE), "MFStartup");
        mfStarted = true;

        if (!FindActivate()) return false;

        MF_CHECK(MFCreateDXGIDeviceManager(&resetToken, &deviceManager),
                 "MFCreateDXGIDeviceManager");
        MF_CHECK(deviceManager->ResetDevice(device.Get(), resetToken), "ResetDevice");

        if (!ConfigureTransform()) return false;
        if (!SetupColorConvert()) return false;

        // File debug tùy chọn: NAL Annex-B thô (giống NVENC), không còn là .mp4 container.
        if (!cfg.outputPath.empty()) {
            std::wstring path = cfg.outputPath;
            size_t dot = path.find_last_of(L'.');
            if (dot != std::wstring::npos && path.substr(dot) == L".mp4")
                path = path.substr(0, dot) + L".h264";
            out = _wfopen(path.c_str(), L"wb");
            if (!out) { std::printf("[MfEncoder] Failed to open output file.\n"); return false; }
        } else if (!cfg.onPacket) {
            std::printf("[MfEncoder] No outputPath or onPacket - no output destination.\n");
            return false;
        }

        std::printf("[MfEncoder] Initialized: %ux%u @%ufps, %.1f Mbps, %s%s -> %s\n",
                    cfg.width, cfg.height, cfg.fps, cfg.bitrateBps / 1e6,
                    cfg.codec == Codec::HEVC ? "HEVC" : "H264",
                    isAsync ? " (async MFT)" : " (sync MFT)",
                    out ? "file" : "callback");
        return true;
    }

    // Rate control / low-latency / force-keyframe qua ICodecAPI. Không bắt buộc: nếu MFT
    // không hỗ trợ ICodecAPI hoặc một thuộc tính nào đó, bỏ qua (không làm Init thất bại).
    bool SetupRateControl() {
        if (FAILED(mft.As(&codecApi))) {
            std::printf("[MfEncoder] Failed to get ICodecAPI - using MFT default parameters.\n");
            return true;
        }
        auto setUI4 = [&](const GUID& api, ULONG val) {
            if (!codecApi->IsSupported(&api)) return;
            VARIANT v{}; v.vt = VT_UI4; v.ulVal = val;
            codecApi->SetValue(&api, &v);
        };
        auto setBool = [&](const GUID& api, bool val) {
            if (!codecApi->IsSupported(&api)) return;
            VARIANT v{}; v.vt = VT_BOOL; v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
            codecApi->SetValue(&api, &v);
        };
        setUI4(CODECAPI_AVEncCommonRateControlMode, (ULONG)eAVEncCommonRateControlMode_CBR);
        setUI4(CODECAPI_AVEncCommonMeanBitRate, (ULONG)cfg.bitrateBps);
        setBool(CODECAPI_AVEncCommonLowLatency, true);
        setBool(CODECAPI_AVLowLatencyMode, true);
        setUI4(CODECAPI_AVEncMPVGOPSize, 0x7fffffff);  // ~vô hạn, IDR theo yêu cầu
        return true;
    }

    // Xin IDR. true = sẵn sàng nhận frame kế tiếp (có thể vừa tạo lại transform).
    // false = hỏng hoàn toàn.
    bool RequestKeyFrame() {
        if (codecApi && codecApi->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame)) {
            VARIANT v{}; v.vt = VT_UI4; v.ulVal = 1;
            if (SUCCEEDED(codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v))) return true;
        }
        // Driver này không hỗ trợ force qua ICodecAPI (đã xác nhận qua kiểm thử thật với
        // Intel QSV) - tạo lại transform từ đầu, transform mới luôn phát IDR ở frame đầu.
        return ReinitTransform();
    }

    // D3D11 Video Processor để chuyển BGRA (từ WGC) -> NV12 (định dạng encoder cần).
    bool SetupColorConvert() {
        MF_CHECK(device.As(&videoDevice), "ID3D11VideoDevice");
        MF_CHECK(context.As(&videoContext), "ID3D11VideoContext");

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = cfg.width;
        cd.InputHeight = cfg.height;
        cd.OutputWidth = cfg.width;
        cd.OutputHeight = cfg.height;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        MF_CHECK(videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum),
                 "CreateVideoProcessorEnumerator");

        UINT fmtFlags = 0;
        HRESULT hr = vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &fmtFlags);
        if (FAILED(hr) || !(fmtFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            std::printf("[MfEncoder] GPU cannot output NV12 from video processor.\n");
            return false;
        }

        MF_CHECK(videoDevice->CreateVideoProcessor(vpEnum.Get(), 0, &vp), "CreateVideoProcessor");

        D3D11_TEXTURE2D_DESC td{};
        td.Width = cfg.width;
        td.Height = cfg.height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        MF_CHECK(device->CreateTexture2D(&td, nullptr, &nv12Tex), "CreateTexture2D(NV12)");

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        od.Texture2D.MipSlice = 0;
        MF_CHECK(videoDevice->CreateVideoProcessorOutputView(nv12Tex.Get(), vpEnum.Get(), &od,
                                                              &vpOutView),
                 "CreateVideoProcessorOutputView");

        RECT rect{ 0, 0, (LONG)cfg.width, (LONG)cfg.height };
        videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &rect);
        videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &rect);
        return true;
    }

    bool ConvertToNv12(ID3D11Texture2D* bgra) {
        auto it = vpInViews.find(bgra);
        if (it == vpInViews.end()) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
            vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = 0;
            vd.Texture2D.ArraySlice = 0;
            ComPtr<ID3D11VideoProcessorInputView> view;
            MF_CHECK(videoDevice->CreateVideoProcessorInputView(bgra, vpEnum.Get(), &vd, &view),
                     "CreateVideoProcessorInputView");
            it = vpInViews.emplace(bgra, std::move(view)).first;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = it->second.Get();
        MF_CHECK(videoContext->VideoProcessorBlt(vp.Get(), vpOutView.Get(), 0, 1, &stream),
                 "VideoProcessorBlt");
        return true;
    }

    // Rút extradata SPS/PPS (Annex-B) từ kiểu đầu ra đã thương lượng. Gọi lần đầu khi
    // gặp IDR - vài encoder chỉ điền đầy đủ blob này sau khi đã bắt đầu nén.
    void CacheSpsPps() {
        ComPtr<IMFMediaType> curOut;
        if (FAILED(mft->GetOutputCurrentType(0, &curOut))) return;
        UINT32 size = 0;
        if (FAILED(curOut->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return;
        spsPps.resize(size);
        if (FAILED(curOut->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, spsPps.data(), size, nullptr)))
            spsPps.clear();
    }

    // Quét NAL Annex-B tìm nal_unit_type==5 (IDR slice). Dùng thay cho
    // MFSampleExtension_CleanPoint: attribute này không đáng tin cậy trên mọi driver
    // (vd Intel QSV chỉ đặt đúng cho sample đầu tiên, các IDR sau bỏ trống).
    static bool ContainsIdrNal(const uint8_t* data, size_t len) {
        for (size_t i = 0; i + 3 < len; ++i) {
            if (data[i] != 0 || data[i + 1] != 0) continue;
            size_t hdr;
            if (data[i + 2] == 1) hdr = i + 3;
            else if (data[i + 2] == 0 && i + 4 < len && data[i + 3] == 1) hdr = i + 4;
            else continue;
            if (hdr < len && (data[hdr] & 0x1F) == 5) return true;
            i = hdr;
        }
        return false;
    }

    // Rút NAL từ 1 sample đầu ra, chèn SPS/PPS trước IDR, đẩy vào file/callback.
    bool EmitSample(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        MF_CHECK(sample->ConvertToContiguousBuffer(&buffer), "ConvertToContiguousBuffer");
        BYTE* data = nullptr;
        DWORD len = 0;
        MF_CHECK(buffer->Lock(&data, nullptr, &len), "Lock(out)");

        const bool keyframe = ContainsIdrNal(data, len);
        if (keyframe && spsPps.empty()) CacheSpsPps();

        LONGLONG timeHns = 0;
        sample->GetSampleTime(&timeHns);
        const uint64_t tsUs = firstTsUs + (uint64_t)(timeHns / 10);
        const bool prependHeader = keyframe && !spsPps.empty();

        if (out) {
            if (prependHeader) std::fwrite(spsPps.data(), 1, spsPps.size(), out);
            std::fwrite(data, 1, len, out);
        }
        if (cfg.onPacket && len > 0) {
            if (prependHeader) {
                std::vector<uint8_t> withHeader;
                withHeader.reserve(spsPps.size() + len);
                withHeader.insert(withHeader.end(), spsPps.begin(), spsPps.end());
                withHeader.insert(withHeader.end(), data, data + len);
                cfg.onPacket(withHeader.data(), withHeader.size(), tsUs, keyframe);
            } else {
                cfg.onPacket(data, len, tsUs, keyframe);
            }
        }
        totalBytes += len;
        ++frameCount;
        if (frameCount <= 5 || frameCount % 60 == 0) {
            std::printf("[MfEncoder] frame %llu: %lu byte%s\n", (unsigned long long)frameCount,
                        len, keyframe ? " (IDR)" : "");
        }
        buffer->Unlock();
        return true;
    }

    // MFT đổi kiểu đầu ra (vd cần padding macroblock 16px cho kích thước lẻ) - lấy lại
    // kiểu nó đề nghị và chấp nhận (giữ nguyên, không tự sửa FRAME_SIZE/bitrate).
    bool RenegotiateOutputType() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            HRESULT hr = mft->GetOutputAvailableType(0, i, &t);
            if (hr == MF_E_NO_MORE_TYPES) break;
            if (FAILED(hr)) {
                std::printf("[MfEncoder] GetOutputAvailableType failed: 0x%08lX\n",
                            (unsigned long)hr);
                return false;
            }
            GUID sub{};
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub != SubtypeFor(cfg.codec)) continue;
            if (SUCCEEDED(mft->SetOutputType(0, t.Get(), 0))) {
                MFT_OUTPUT_STREAM_INFO si{};
                if (SUCCEEDED(mft->GetOutputStreamInfo(0, &si))) {
                    outputProvidesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                                           MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
                }
                return true;
            }
        }
        std::printf("[MfEncoder] Could not find a suitable output type after STREAM_CHANGE.\n");
        return false;
    }

    // Rút 1 sample đầu ra nếu có. 1 = đã phát (EmitSample), 0 = chưa có gì, -1 = lỗi.
    // MFT đồng bộ: STREAM_CHANGE cho phép gọi lại ProcessOutput ngay (cùng 1 lần gọi).
    // MFT bất đồng bộ: KHÔNG được gọi Process* ngoài sự kiện - renegotiate rồi return 0,
    // chờ sự kiện HaveOutput mới (MFT tự phát lại sau khi kiểu đã được chấp nhận).
    int PullOneOutput() {
        const int maxAttempts = isAsync ? 1 : 2;
        for (int attempt = 0; attempt < maxAttempts; ++attempt) {
            MFT_OUTPUT_DATA_BUFFER ob{};
            ob.dwStreamID = 0;
            ComPtr<IMFSample> ownedSample;
            if (!outputProvidesSamples) {
                MFT_OUTPUT_STREAM_INFO si{};
                MF_CHECKI(mft->GetOutputStreamInfo(0, &si), "GetOutputStreamInfo");
                ComPtr<IMFMediaBuffer> buf;
                MF_CHECKI(MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (1u << 20), &buf),
                          "MFCreateMemoryBuffer(out)");
                MF_CHECKI(MFCreateSample(&ownedSample), "MFCreateSample(out)");
                MF_CHECKI(ownedSample->AddBuffer(buf.Get()), "AddBuffer(out)");
                ob.pSample = ownedSample.Get();
            }
            DWORD status = 0;
            HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
            if (ob.pEvents) { ob.pEvents->Release(); ob.pEvents = nullptr; }

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 0;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (ob.pSample && outputProvidesSamples) ob.pSample->Release();
                if (!RenegotiateOutputType()) return -1;
                continue;  // đồng bộ: thử lại ngay. Bất đồng bộ: hết lượt (maxAttempts=1), return 0.
            }
            if (FAILED(hr)) {
                if (ob.pSample && outputProvidesSamples) ob.pSample->Release();
                std::printf("[MfEncoder] ProcessOutput failed: 0x%08lX\n", (unsigned long)hr);
                return -1;
            }

            ComPtr<IMFSample> sample;
            if (outputProvidesSamples) sample.Attach(ob.pSample);
            else sample = ownedSample;
            if (!sample) return 0;
            return EmitSample(sample.Get()) ? 1 : -1;
        }
        return 0;
    }

    // Đường đồng bộ: rút hết output đang có cho tới khi MFT báo hết (NEED_MORE_INPUT).
    bool DrainOutputsSync() {
        for (;;) {
            int r = PullOneOutput();
            if (r < 0) return false;
            if (r == 0) return true;
        }
    }

    // Đường bất đồng bộ: chờ tới khi MFT báo sẵn sàng nhận input, xử lý output rảnh
    // được báo dọc đường (không được gọi ProcessInput/Output ngoài sự kiện như thế này).
    bool WaitForNeedInputAsync() {
        for (;;) {
            ComPtr<IMFMediaEvent> ev;
            MF_CHECK(events->GetEvent(0, &ev), "GetEvent");
            MediaEventType met = MEUnknown;
            MF_CHECK(ev->GetType(&met), "GetType");
            if (met == METransformNeedInput) return true;
            if (met == METransformHaveOutput) {
                if (PullOneOutput() < 0) return false;
                continue;
            }
            // Các event khác (drain complete, marker...) - bỏ qua, chờ tiếp NeedInput.
        }
    }

    bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) {
        if (!streaming) return false;
        if (!ConvertToNv12(frame)) return false;
        if (forceKeyframe && !RequestKeyFrame()) return false;

        if (isAsync && !WaitForNeedInputAsync()) return false;

        ComPtr<IMFMediaBuffer> buffer;
        MF_CHECK(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Tex.Get(), 0, FALSE,
                                           &buffer),
                 "MFCreateDXGISurfaceBuffer");
        ComPtr<IMF2DBuffer2> buf2d;
        if (SUCCEEDED(buffer.As(&buf2d))) {
            DWORD len = 0;
            if (SUCCEEDED(buf2d->GetContiguousLength(&len))) buffer->SetCurrentLength(len);
        }

        ComPtr<IMFSample> sample;
        MF_CHECK(MFCreateSample(&sample), "MFCreateSample");
        MF_CHECK(sample->AddBuffer(buffer.Get()), "AddBuffer");

        if (!haveFirstTs) { firstTsUs = timestampUs; haveFirstTs = true; }
        const LONGLONG timeHns = static_cast<LONGLONG>((timestampUs - firstTsUs) * 10ull);
        const LONGLONG durHns = static_cast<LONGLONG>(10'000'000ull / (cfg.fps ? cfg.fps : 60));
        sample->SetSampleTime(timeHns);
        sample->SetSampleDuration(durHns);

        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (!isAsync && hr == MF_E_NOTACCEPTING) {
            if (!DrainOutputsSync()) return false;
            hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        MF_CHECK(hr, "ProcessInput");

        return isAsync ? true : DrainOutputsSync();
    }

    void Finish() {
        if (!streaming) return;
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        for (int i = 0; i < 256; ++i) {
            if (isAsync) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(events->GetEvent(0, &ev))) break;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformHaveOutput) { if (PullOneOutput() < 0) break; continue; }
                if (met == METransformDrainComplete) break;
            } else {
                int r = PullOneOutput();
                if (r < 0) break;
                if (r == 0) break;
            }
        }
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        streaming = false;
        if (out) std::fflush(out);
        std::printf("[MfEncoder] Encoded %llu frame, %.2f MB.\n",
                    (unsigned long long)frameCount, totalBytes / 1e6);
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
