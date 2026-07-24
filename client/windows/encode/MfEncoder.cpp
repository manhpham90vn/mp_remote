// =============================================================================
// MfEncoder.cpp — cài đặt backend Media Foundation. File dài nhất dự án, và độ dài
// đó gần như hoàn toàn đến từ việc phải chiều theo sự khác nhau giữa các driver.
//
// BỐ CỤC
//   FindActivate()        — tìm MFT encoder D3D11-aware.
//   ConfigureTransform()  — đặt kiểu vào/ra, rate control, bật streaming.
//   ReinitTransform()     — dựng lại transform từ đầu (đường xin IDR dự phòng).
//   SetupColorConvert()   — dựng video processor BGRA → NV12.
//   ConvertToNv12()       — chạy chuyển màu cho một frame.
//   Encode()              — đường chính: chuyển màu → ProcessInput → rút output.
//   PullOneOutput()       — rút một sample, xử lý cả STREAM_CHANGE.
//   EmitSample()          — bóc NAL, chèn SPS/PPS, giao cho file/callback.
//
// ⚠ HAI ĐƯỜNG CHẠY SONG SONG: ĐỒNG BỘ VÀ BẤT ĐỒNG BỘ
//   Đây là điều quan trọng nhất cần nắm. MFT phần mềm thường ĐỒNG BỘ, MFT phần cứng
//   thường BẤT ĐỒNG BỘ, và hai loại có luật gọi khác hẳn nhau:
//
//     Đồng bộ  — gọi ProcessInput rồi ProcessOutput thoải mái theo ý mình.
//                (DrainOutputsSync: rút tới khi NEED_MORE_INPUT.)
//     Bất đồng bộ — TUYỆT ĐỐI không được gọi ProcessInput/ProcessOutput tuỳ ý. Phải
//                chờ MFT bắn sự kiện METransformNeedInput / METransformHaveOutput
//                rồi mới gọi tương ứng. Gọi sai lúc thì hỏng transform.
//                (WaitForNeedInputAsync.)
//
//   Cờ `isAsync` phân nhánh ở bốn chỗ: Encode(), PullOneOutput() (số lần thử lại),
//   Finish(), và ConfigureTransform() (phải MF_TRANSFORM_ASYNC_UNLOCK trước khi gọi
//   bất cứ hàm nào khác). Sửa bất kỳ chỗ nào phải nghĩ cho cả hai đường.
//
// BA CHỖ PHẢI ĐI ĐƯỜNG VÒNG VÌ DRIVER KHÔNG ĐÁNG TIN — đều đã kiểm chứng bằng thử
// nghiệm thật, đừng "dọn dẹp" chúng nếu chưa đo lại:
//   1. MF_SA_D3D11_AWARE đọc trên IMFActivate không đáng tin → phải activate rồi mới
//      đọc thuộc tính trên chính transform (FindActivate).
//   2. Force-keyframe qua ICodecAPI không có tác dụng trên vài driver (Intel QSV) →
//      đường lùi là dựng lại nguyên transform (ReinitTransform).
//   3. MFSampleExtension_CleanPoint không đáng tin → tự quét NAL tìm type 5 để biết
//      frame nào là IDR (ContainsIdrNal).
//
// LIÊN QUAN: encode/MfEncoder.h (vì sao dùng MFT trần), encode/NvencEncoder.cpp
//            (bản đối chiếu, đơn giản hơn nhiều), encode/IVideoEncoder.h
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "encode/MfEncoder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdio>
#include <map>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

// Gọi một hàm trả HRESULT; hỏng thì in mã lỗi rồi thoát hàm. Có macro vì file này
// gọi hàng trăm lời gọi COM và viết tay `if (FAILED(hr))` cho từng cái sẽ che mất
// mạch logic. Bọc trong do/while(0) để dùng được trong `if` không ngoặc.
//
// Biến thể: MF_CHECK trả false, MF_CHECKI trả -1 (cho hàm trả int).
#define MF_CHECK(expr, msg)                                        \
    do {                                                           \
        HRESULT _hr = (expr);                                      \
        if (FAILED(_hr)) {                                         \
            std::printf("[MfEncoder] %s failed: 0x%08lX\n", (msg), \
                (unsigned long)_hr);                               \
            return false;                                          \
        }                                                          \
    } while (0)

// Biến thể trả -1 (dùng trong hàm trả int).
#define MF_CHECKI(expr, msg)                                       \
    do {                                                           \
        HRESULT _hr = (expr);                                      \
        if (FAILED(_hr)) {                                         \
            std::printf("[MfEncoder] %s failed: 0x%08lX\n", (msg), \
                (unsigned long)_hr);                               \
            return -1;                                             \
        }                                                          \
    } while (0)

struct MfEncoder::Impl {
    ComPtr<IMFActivate> activate; // giữ lại để tạo-lại transform khi cần (xin keyframe)
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events; // chỉ dùng khi isAsync
    ComPtr<IMFDXGIDeviceManager> deviceManager;
    ComPtr<ICodecAPI> codecApi; // rate control / force keyframe (không bắt buộc)

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11Texture2D> nv12Tex; // scratch: BGRA (WGC) -> NV12 (đầu vào encoder)
    ComPtr<ID3D11VideoProcessorOutputView> vpOutView;
    // Cache input view theo texture nguồn: WGC dùng lại vài texture (pool depth 2).
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> vpInViews;

    EncoderConfig cfg{};
    UINT resetToken = 0;
    bool mfStarted = false;
    bool streaming = false;
    bool isAsync = false;
    bool outputProvidesSamples = false;
    bool haveFirstTs = false;
    uint64_t firstTsUs = 0;
    // Số sự kiện METransformNeedInput đã tiêu thụ TRƯỚC bởi PumpAsyncEvents —
    // WaitForNeedInputAsync trừ dần thay vì chặn chờ sự kiện đã lấy mất.
    int needInputCredit = 0;
    ULONGLONG lastEncodeTickMs = 0; // phát hiện nhịp input thưa (nguồn tĩnh/keepalive)
    bool rcLogged = false;          // log kết quả CodecAPI đúng một lần (Reinit gọi lại)
    uint64_t frameCount = 0;
    uint64_t totalBytes = 0;
    FILE* out = nullptr;
    std::vector<uint8_t> spsPps; // extradata Annex-B (SPS+PPS), chèn trước mỗi IDR

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
        if (out) {
            std::fclose(out);
            out = nullptr;
        }
        if (mfStarted) MFShutdown();
    }

    GUID SubtypeFor(Codec c) const {
        return (c == Codec::HEVC) ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    }

    // Tìm + chọn IMFActivate D3D11-aware phù hợp (chỉ làm 1 lần, giữ lại trong `activate`
    // để tạo-lại transform rẻ khi cần xin keyframe - xem ReinitTransform()).
    bool FindActivate() {
        MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, SubtypeFor(cfg.codec)};
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
            if (!aware) {
                activates[i]->ShutdownObject();
                continue;
            }
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

        // MFT phần cứng thường là async, và ở trạng thái KHOÁ khi vừa tạo ra. Phải
        // mở khoá TRƯỚC khi gọi bất kỳ method nào khác, nếu không mọi lời gọi sau
        // đều trả lỗi. Xem mục "hai đường chạy song song" ở đầu file.
        UINT32 asyncFlag = 0;
        mftAttrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag);
        isAsync = asyncFlag != 0;
        if (isAsync) {
            MF_CHECK(mftAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "ASYNC_UNLOCK");
            MF_CHECK(mft.As(&events), "IMFMediaEventGenerator");
        }

        MF_CHECK(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager.Get()),
            "SET_D3D_MANAGER");

        // --- Kiểu ĐẦU RA đặt trước ĐẦU VÀO. Thứ tự này bắt buộc với encoder MFT:
        //     tập kiểu đầu vào hợp lệ phụ thuộc vào đích đã chọn, nên đặt ngược lại
        //     thì SetInputType sẽ bị từ chối. (Với decoder MFT thì ngược lại.) ---
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
        // Khai đúng range/matrix của NV12 mà ConvertToNv12 phát (BT.709 limited) để
        // encoder ghi VUI khớp — decoder khác (MediaCodec ở Android) đọc VUI này;
        // khai sai/bỏ trống là màu lệch ở client không dùng đường D3D11.
        inType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        MF_CHECK(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height),
            "FRAME_SIZE(in)");
        MF_CHECK(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg.fps, 1),
            "FRAME_RATE(in)");
        MF_CHECK(mft->SetInputType(0, inType.Get(), 0), "SetInputType");

        if (!SetupRateControl()) return false;

        // Ai cấp bộ nhớ cho sample đầu ra: MFT tự cấp, hay ta phải cấp sẵn rồi đưa
        // vào? Hai đường xử lý khác nhau ở PullOneOutput, và cờ này quyết định đi
        // đường nào. Hỏi lại mỗi lần đổi kiểu đầu ra (xem RenegotiateOutputType).
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
        // Credit NeedInput thuộc về hàng sự kiện của transform CŨ — mang sang
        // transform mới là ProcessInput chạy khi nó chưa sẵn sàng, hỏng dây chuyền
        // (đo 2026-07-21: 18,5s không gói nào ra sau một lần xin IDR).
        needInputCredit = 0;
        lastEncodeTickMs = 0;
        if (!activate) return false;
        activate->ShutdownObject();
        if (FAILED(activate->ActivateObject(IID_PPV_ARGS(&mft)))) {
            std::printf("[MfEncoder] Failed to recreate encoder for keyframe request.\n");
            return false;
        }
        spsPps.clear(); // transform mới - lấy lại extradata riêng của nó
        return ConfigureTransform();
    }

    bool Init(ID3D11Device* dev, const EncoderConfig& c) {
        cfg = c;
        device = dev;
        device->GetImmediateContext(&context);

        // BẮT BUỘC: D3D11 immediate context mặc định KHÔNG an toàn đa luồng, mà MFT
        // phần cứng chạy công việc trên thread riêng của nó còn ta gọi video
        // processor từ thread capture. Thiếu dòng này thì hỏng ngẫu nhiên — sai hình
        // hoặc sập, và rất khó tái hiện vì phụ thuộc thời điểm.
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
            if (!out) {
                std::printf("[MfEncoder] Failed to open output file.\n");
                return false;
            }
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
        // Log kết quả từng thuộc tính ĐÚNG MỘT LẦN (ReinitTransform gọi lại hàm này
        // mỗi lần xin IDR trên QSV): driver nuốt lệnh im lặng thì không bao giờ biết
        // vì sao rate control không ăn — đo 2026-07-21 VBV bị phớt lờ, IDR vẫn 195KB.
        const bool log = !rcLogged;
        auto report = [&](const char* name, const char* what) {
            if (log) std::printf("[MfEncoder] codecapi %s: %s\n", name, what);
        };
        auto setUI4 = [&](const GUID& api, ULONG val, const char* name) {
            if (!codecApi->IsSupported(&api)) {
                report(name, "NOT SUPPORTED");
                return;
            }
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = val;
            report(name, SUCCEEDED(codecApi->SetValue(&api, &v)) ? "ok" : "SetValue FAILED");
        };
        auto setBool = [&](const GUID& api, bool val, const char* name) {
            if (!codecApi->IsSupported(&api)) {
                report(name, "NOT SUPPORTED");
                return;
            }
            VARIANT v{};
            v.vt = VT_BOOL;
            v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
            report(name, SUCCEEDED(codecApi->SetValue(&api, &v)) ? "ok" : "SetValue FAILED");
        };
        setUI4(CODECAPI_AVEncCommonRateControlMode, (ULONG)eAVEncCommonRateControlMode_CBR,
            "RateControlMode=CBR");
        setUI4(CODECAPI_AVEncCommonMeanBitRate, (ULONG)cfg.bitrateBps, "MeanBitRate");
        setBool(CODECAPI_AVEncCommonLowLatency, true, "CommonLowLatency");
        setBool(CODECAPI_AVLowLatencyMode, true, "LowLatencyMode");
        setUI4(CODECAPI_AVEncMPVGOPSize, 0x7fffffff, "GOPSize"); // ~vô hạn, IDR theo yêu cầu
        // VBV ~2 frame (đơn vị BIT): trần cho cú dồn của một frame. Không có nó,
        // QSV đẻ IDR ~196 KB — gấp ~5 lần ngân sách frame @20Mbps/60fps (đo
        // 2026-07-21) — thành chùm 160+ gói, đúng thủ phạm burst-loss trên Wi-Fi
        // (docs/06 §7b). NVENC đã bị ép VBV 1 frame từ trước (NvencEncoder.cpp:148);
        // QSV cho 2 frame để IDR còn chỗ thở, chất lượng đỡ sụt ở cảnh động.
        const ULONG frameBits = (ULONG)(cfg.bitrateBps / (cfg.fps ? cfg.fps : 60));
        setUI4(CODECAPI_AVEncCommonBufferSize, frameBits * 2, "BufferSize(VBV)");
        // Mức đầy ban đầu của VBV: transform MỚI (mỗi lần xin IDR trên QSV là một
        // transform mới — ReinitTransform) khởi đầu với buffer rủng rỉnh nên IDR
        // đầu tiên vượt trần (đo 2026-07-21: 273KB dù trần 2 frame ~83KB). Khai
        // mức đầu = 1 frame để IDR mở màn cũng phải theo khuôn khổ.
        setUI4(CODECAPI_AVEncCommonBufferInLevel, frameBits, "BufferInLevel");
        rcLogged = true;
        return true;
    }

    // Đổi bitrate giữa chừng. cfg.bitrateBps được cập nhật để lần ReinitTransform
    // sau (force keyframe trên driver không hỗ trợ ICodecAPI) không quay về số cũ.
    bool SetBitrate(uint32_t bitrateBps) {
        if (!bitrateBps) return false;
        cfg.bitrateBps = bitrateBps;
        if (!codecApi || !codecApi->IsSupported(&CODECAPI_AVEncCommonMeanBitRate)) return false;
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = (ULONG)bitrateBps;
        return SUCCEEDED(codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v));
    }

    // Xin IDR. true = sẵn sàng nhận frame kế tiếp (có thể vừa tạo lại transform).
    // false = hỏng hoàn toàn.
    bool RequestKeyFrame() {
        if (codecApi && codecApi->IsSupported(&CODECAPI_AVEncVideoForceKeyFrame)) {
            VARIANT v{};
            v.vt = VT_UI4;
            v.ulVal = 1;
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

        // Đầu vào theo kích thước texture THẬT (có thể lẻ), đầu ra theo kích thước
        // nén (chẵn). Khai báo lệch làm CreateVideoProcessorInputView có thể từ chối
        // texture nguồn - enumerator này chính là thứ validate view ở ConvertToNv12.
        const uint32_t inW = cfg.srcWidth ? cfg.srcWidth : cfg.width;
        const uint32_t inH = cfg.srcHeight ? cfg.srcHeight : cfg.height;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = inW;
        cd.InputHeight = inH;
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

        RECT rect{0, 0, (LONG)cfg.width, (LONG)cfg.height};
        videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &rect);
        videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &rect);

        // Khai TƯỜNG MINH color space cho phép chuyển BGRA→NV12: vào là RGB full
        // range (0-255, ảnh chụp màn hình), ra là YUV BT.709 limited (16-235) —
        // quy ước video chuẩn mà decoder mặc định hiểu. Bỏ trống thì driver tự
        // đoán, và hai đầu đoán khác nhau là màu trôi cả khung (đen bị nâng,
        // sáng bị cắt) — Renderer phía client khai đúng bộ ngược lại.
        //
        // Driver đời mới (Intel WDDM 2.x+) BỎ QUA struct legacy bên dưới và chỉ
        // tôn trọng đường ColorSpace1 (DXGI_COLOR_SPACE_TYPE) — đo thực tế
        // 24/07/2026: chỉ set struct legacy thì màu không đổi gì. Gọi cả hai:
        // *1 cho driver mới, struct cho driver cũ chưa có ID3D11VideoContext1.
        ComPtr<ID3D11VideoContext1> vc1;
        if (SUCCEEDED(videoContext.As(&vc1))) {
            vc1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0,
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            vc1->VideoProcessorSetOutputColorSpace1(vp.Get(),
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        }
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
        inCs.RGB_Range = 0; // 0 = full 0-255
        videoContext->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
        outCs.YCbCr_Matrix = 1; // 1 = BT.709
        outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);

        // TẮT "video enhancement" tự động của driver (Intel ACE tăng sáng thích
        // ứng nội dung tối...): nó chen vào VideoProcessorBlt NGOÀI mọi khai báo
        // color space, làm ảnh stream sáng/tương phản lệch hẳn bản gốc dù chuỗi
        // range/matrix đã đúng. Phải tắt ở CẢ hai đầu (đo 24/07/2026).
        videoContext->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        return true;
    }

    // Chuyển một frame BGRA sang NV12 bằng GPU. Kết quả nằm ở nv12Tex (dùng lại
    // qua từng frame — không cấp phát gì trên đường nóng).
    //
    // Input view được nhớ theo con trỏ texture vì WGC chỉ luân phiên vài texture cố
    // định (frame pool sâu 2) — cùng lý do như map `registered` của NvencEncoder.
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
            if (data[i + 2] == 1)
                hdr = i + 3;
            else if (data[i + 2] == 0 && i + 4 < len && data[i + 3] == 1)
                hdr = i + 4;
            else
                continue;
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
        // Chèn SPS/PPS trước MỖI IDR — tương đương repeatSPSPPS của NVENC. Bắt buộc
        // với UDP: client vào giữa chừng hoặc vừa mất gói sẽ không có tham số giải
        // mã nếu chúng chỉ xuất hiện một lần ở đầu stream.
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
            if (ob.pEvents) {
                ob.pEvents->Release();
                ob.pEvents = nullptr;
            }

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 0;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (ob.pSample && outputProvidesSamples) ob.pSample->Release();
                if (!RenegotiateOutputType()) return -1;
                continue; // đồng bộ: thử lại ngay. Bất đồng bộ: hết lượt (maxAttempts=1), return 0.
            }
            if (FAILED(hr)) {
                if (ob.pSample && outputProvidesSamples) ob.pSample->Release();
                std::printf("[MfEncoder] ProcessOutput failed: 0x%08lX\n", (unsigned long)hr);
                return -1;
            }

            ComPtr<IMFSample> sample;
            if (outputProvidesSamples)
                sample.Attach(ob.pSample);
            else
                sample = ownedSample;
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
        // NeedInput đã được PumpAsyncEvents lấy trước rồi thì khỏi chặn chờ.
        if (needInputCredit > 0) {
            --needInputCredit;
            return true;
        }
        // Chờ CÓ HẠN (poll NO_WAIT + ngủ 1ms, trần 1s) thay vì GetEvent chặn vô
        // hạn: encode chạy dưới encMutex trên cả thread Recv (keepalive/IDR tĩnh),
        // GetEvent treo là thread Recv treo theo — đo 2026-07-21 thấy RTT nhảy
        // 0,7↔380ms và 18,5s không gói nào ra. Hết hạn = frame này hỏng, phiên sống.
        for (int waitedMs = 0;;) {
            ComPtr<IMFMediaEvent> ev;
            const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (waitedMs++ >= 1000) {
                    std::printf("[MfEncoder] Timed out waiting for encoder NeedInput.\n");
                    return false;
                }
                Sleep(1);
                continue;
            }
            MF_CHECK(hr, "GetEvent");
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

    // Vét sự kiện async NGAY SAU ProcessInput, không chặn (NO_WAIT).
    //
    // VÌ SAO PHẢI CÓ: không có nó, output chỉ được rút bên trong
    // WaitForNeedInputAsync của lần Encode KẾ TIẾP, mà QSV lại xếp sẵn nhiều
    // NeedInput lúc khởi động — output bị giam sau hàng sự kiện. Input càng thưa
    // thì trễ càng dồn: nguồn tĩnh + keepalive 2fps đo được e2e ~3,4s = ~7 frame
    // × 500ms (2026-07-21). Vét ở đây thì frame thoát ra trong vài ms sau khi nén
    // xong. NeedInput vét được cộng vào `needInputCredit` cho lần Encode sau.
    //
    // `waitForOutput` chỉ bật khi nhịp input thưa (>100ms — đường keepalive):
    // ngủ 1ms/vòng, tối đa ~30ms, chờ encoder nhả CHÍNH frame vừa đưa vào.
    // Ở 60fps không bao giờ ngủ — đường nóng giữ nguyên (bài học Pacer, docs/06).
    bool PumpAsyncEvents(bool waitForOutput) {
        int sleepBudgetMs = 30;
        for (;;) {
            ComPtr<IMFMediaEvent> ev;
            const HRESULT hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (!waitForOutput || sleepBudgetMs-- <= 0) return true;
                Sleep(1);
                continue;
            }
            if (FAILED(hr)) return true; // đường vét phụ — không giết phiên vì nó
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformNeedInput) {
                ++needInputCredit;
            } else if (met == METransformHaveOutput) {
                const int r = PullOneOutput();
                if (r < 0) return false;
                if (r > 0) waitForOutput = false; // frame đã ra — vét nốt rồi thôi chờ
            }
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

        // MF muốn mốc thời gian bắt đầu từ 0, còn capture giao ra đồng hồ hệ thống.
        // Nhớ mốc đầu tiên rồi trừ đi; EmitSample cộng lại để trả về thang gốc.
        // Đơn vị của MF là 100ns nên phải nhân 10 từ micro-giây.
        if (!haveFirstTs) {
            firstTsUs = timestampUs;
            haveFirstTs = true;
        }
        const LONGLONG timeHns = static_cast<LONGLONG>((timestampUs - firstTsUs) * 10ull);
        const LONGLONG durHns = static_cast<LONGLONG>(10'000'000ull / (cfg.fps ? cfg.fps : 60));
        sample->SetSampleTime(timeHns);
        sample->SetSampleDuration(durHns);

        // Đường đồng bộ: NOTACCEPTING nghĩa là MFT đang đầy output chưa ai lấy. Rút
        // sạch rồi thử lại một lần. Đường bất đồng bộ không gặp tình huống này vì
        // WaitForNeedInputAsync ở trên đã bảo đảm MFT đang sẵn sàng nhận.
        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (!isAsync && hr == MF_E_NOTACCEPTING) {
            if (!DrainOutputsSync()) return false;
            hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        MF_CHECK(hr, "ProcessInput");

        if (isAsync) {
            // Nhịp input thưa (>100ms = nguồn tĩnh/keepalive) thì đáng bỏ vài ms
            // chờ vét output của chính frame này — xem PumpAsyncEvents.
            const ULONGLONG nowMs = GetTickCount64();
            const bool sparse = lastEncodeTickMs && nowMs - lastEncodeTickMs > 100;
            lastEncodeTickMs = nowMs;
            return PumpAsyncEvents(sparse);
        }
        return DrainOutputsSync();
    }

    // Vét nốt những frame encoder còn giữ rồi đóng stream. DRAIN bảo MFT "không còn
    // input nữa, xuất hết ra đi".
    void Finish() {
        if (!streaming) return;
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        // Trần 256 vòng để không treo vĩnh viễn nếu MFT không bao giờ báo xong —
        // vét thiếu vài frame cuối lúc đóng phiên là vô hại, treo thì không.
        for (int i = 0; i < 256; ++i) {
            if (isAsync) {
                ComPtr<IMFMediaEvent> ev;
                if (FAILED(events->GetEvent(0, &ev))) break;
                MediaEventType met = MEUnknown;
                ev->GetType(&met);
                if (met == METransformHaveOutput) {
                    if (PullOneOutput() < 0) break;
                    continue;
                }
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
    if (!impl_->Init(device, cfg)) {
        impl_.reset();
        return false;
    }
    return true;
}

bool MfEncoder::Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) {
    return impl_ && impl_->Encode(frame, timestampUs, forceKeyframe);
}

bool MfEncoder::SetBitrate(uint32_t bitrateBps) {
    return impl_ && impl_->SetBitrate(bitrateBps);
}

void MfEncoder::Finish() {
    if (impl_) impl_->Finish();
}
