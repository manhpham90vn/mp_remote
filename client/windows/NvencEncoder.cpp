#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "NvencEncoder.h"

#include <windows.h>
#include <d3d11.h>
#include <nvEncodeAPI.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

// Nạp DLL động: không link .lib, chỉ cần DLL đi kèm driver.
using PFN_CreateInstance = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
using PFN_MaxVersion = NVENCSTATUS(NVENCAPI*)(uint32_t*);

struct NvencEncoder::Impl {
    HMODULE                    dll = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nv{};
    void*                      enc = nullptr;         // encode session handle
    NV_ENC_OUTPUT_PTR          bitstream = nullptr;   // 1 buffer (đồng bộ, không B-frame)
    FILE*                      out = nullptr;
    EncoderConfig              cfg{};
    uint32_t                   width = 0, height = 0;
    uint64_t                   frameCount = 0;
    uint64_t                   totalBytes = 0;

    // Cache đăng ký theo con trỏ texture: WGC dùng lại vài texture (pool depth 2).
    std::map<ID3D11Texture2D*, NV_ENC_REGISTERED_PTR> registered;

    ~Impl() { Cleanup(); }

    bool Fail(const char* where, NVENCSTATUS s) {
        const char* msg = (nv.nvEncGetLastErrorString && enc) ? nv.nvEncGetLastErrorString(enc) : "";
        std::printf("[NVENC] %s failed: status=%d %s\n", where, (int)s, msg ? msg : "");
        return false;
    }

    bool Init(ID3D11Device* device, const EncoderConfig& c) {
        cfg = c;
        width = c.width;
        height = c.height;

        dll = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!dll) { std::printf("[NVENC] Failed to load nvEncodeAPI64.dll (NVIDIA driver missing?).\n"); return false; }

        // Kiểm tra driver đủ mới so với header.
        auto getMax = (PFN_MaxVersion)GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion");
        if (getMax) {
            uint32_t driverMax = 0;
            if (getMax(&driverMax) == NV_ENC_SUCCESS) {
                uint32_t needed = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
                if (driverMax < needed) {
                    std::printf("[NVENC] Driver older than header (driver=%u.%u < required=%u.%u).\n",
                        driverMax >> 4, driverMax & 0xf,
                        NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
                    return false;
                }
            }
        }

        auto createInstance = (PFN_CreateInstance)GetProcAddress(dll, "NvEncodeAPICreateInstance");
        if (!createInstance) { std::printf("[NVENC] Missing NvEncodeAPICreateInstance.\n"); return false; }

        nv = {};
        nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        NVENCSTATUS s = createInstance(&nv);
        if (s != NV_ENC_SUCCESS) { std::printf("[NVENC] CreateInstance status=%d\n", (int)s); return false; }

        // Mở session trên chính D3D11 device dùng chung với capture.
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
        sp.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        sp.device = device;
        sp.apiVersion = NVENCAPI_VERSION;
        s = nv.nvEncOpenEncodeSessionEx(&sp, &enc);
        if (s != NV_ENC_SUCCESS) { enc = nullptr; return Fail("OpenEncodeSessionEx", s); }

        const GUID codecGuid = (cfg.codec == Codec::HEVC) ? NV_ENC_CODEC_HEVC_GUID
                                                          : NV_ENC_CODEC_H264_GUID;
        const GUID presetGuid = NV_ENC_PRESET_P4_GUID;
        const NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

        // Lấy cấu hình preset rồi tinh chỉnh cho low-latency.
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        s = nv.nvEncGetEncodePresetConfigEx(enc, codecGuid, presetGuid, tuning, &preset);
        if (s != NV_ENC_SUCCESS) return Fail("GetEncodePresetConfigEx", s);

        NV_ENC_CONFIG encCfg = preset.presetCfg;
        encCfg.gopLength = NVENC_INFINITE_GOPLENGTH;   // IDR theo yêu cầu, không định kỳ
        encCfg.frameIntervalP = 1;                     // không B-frame (độ trễ thấp)
        encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        encCfg.rcParams.averageBitRate = cfg.bitrateBps;
        encCfg.rcParams.vbvBufferSize = cfg.bitrateBps / (cfg.fps ? cfg.fps : 60); // ~1 frame
        encCfg.rcParams.vbvInitialDelay = encCfg.rcParams.vbvBufferSize;
        if (cfg.codec == Codec::HEVC) {
            encCfg.encodeCodecConfig.hevcConfig.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encCfg.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        } else {
            encCfg.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
            encCfg.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        }

        NV_ENC_INITIALIZE_PARAMS ip{};
        ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
        ip.encodeGUID = codecGuid;
        ip.presetGUID = presetGuid;
        ip.tuningInfo = tuning;
        ip.encodeWidth = width;
        ip.encodeHeight = height;
        ip.darWidth = width;
        ip.darHeight = height;
        ip.frameRateNum = cfg.fps ? cfg.fps : 60;
        ip.frameRateDen = 1;
        ip.enablePTD = 1;                 // NVENC tự quyết định loại picture
        ip.enableEncodeAsync = 0;         // đồng bộ cho đơn giản
        ip.encodeConfig = &encCfg;
        s = nv.nvEncInitializeEncoder(enc, &ip);
        if (s != NV_ENC_SUCCESS) return Fail("InitializeEncoder", s);

        // Buffer bitstream đầu ra.
        NV_ENC_CREATE_BITSTREAM_BUFFER cb{};
        cb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        s = nv.nvEncCreateBitstreamBuffer(enc, &cb);
        if (s != NV_ENC_SUCCESS) return Fail("CreateBitstreamBuffer", s);
        bitstream = cb.bitstreamBuffer;

        // NVENC xuất Annex-B thô. File .h264 là tùy chọn (rỗng = chỉ đi qua onPacket).
        std::wstring path = cfg.outputPath;
        if (!path.empty()) {
            size_t dot = path.find_last_of(L'.');
            if (dot != std::wstring::npos && path.substr(dot) == L".mp4") path = path.substr(0, dot) + L".h264";
            out = _wfopen(path.c_str(), L"wb");
            if (!out) { std::printf("[NVENC] Failed to open output file.\n"); return false; }
        } else if (!cfg.onPacket) {
            std::printf("[NVENC] No outputPath or onPacket - no output destination.\n");
            return false;
        }

        std::printf("[NVENC] Initialized: %ux%u @%ufps, %.1f Mbps, %s, ULTRA_LOW_LATENCY -> %ls\n",
            width, height, cfg.fps, cfg.bitrateBps / 1e6,
            cfg.codec == Codec::HEVC ? "HEVC" : "H264",
            path.empty() ? L"callback" : path.c_str());
        return true;
    }

    NV_ENC_REGISTERED_PTR RegisterTex(ID3D11Texture2D* tex) {
        auto it = registered.find(tex);
        if (it != registered.end()) return it->second;

        NV_ENC_REGISTER_RESOURCE rr{};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width = width;
        rr.height = height;
        rr.pitch = 0;
        rr.resourceToRegister = tex;
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;  // B8G8R8A8 từ WGC
        NVENCSTATUS s = nv.nvEncRegisterResource(enc, &rr);
        if (s != NV_ENC_SUCCESS) { Fail("RegisterResource", s); return nullptr; }
        registered[tex] = rr.registeredResource;
        return rr.registeredResource;
    }

    bool Encode(ID3D11Texture2D* frame, uint64_t timestampUs, bool forceKeyframe) {
        if (!enc) return false;

        NV_ENC_REGISTERED_PTR reg = RegisterTex(frame);
        if (!reg) return false;

        NV_ENC_MAP_INPUT_RESOURCE mp{};
        mp.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mp.registeredResource = reg;
        NVENCSTATUS s = nv.nvEncMapInputResource(enc, &mp);
        if (s != NV_ENC_SUCCESS) return Fail("MapInputResource", s);

        NV_ENC_PIC_PARAMS pp{};
        pp.version = NV_ENC_PIC_PARAMS_VER;
        pp.inputWidth = width;
        pp.inputHeight = height;
        pp.inputPitch = 0;
        pp.inputBuffer = mp.mappedResource;
        pp.bufferFmt = mp.mappedBufferFmt;
        pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pp.outputBitstream = bitstream;
        pp.inputTimeStamp = timestampUs;
        if (forceKeyframe) pp.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

        s = nv.nvEncEncodePicture(enc, &pp);
        bool ok = true;
        if (s == NV_ENC_SUCCESS) {
            ok = WriteOutput();
        } else if (s != NV_ENC_ERR_NEED_MORE_INPUT) {
            ok = Fail("EncodePicture", s);
        }

        nv.nvEncUnmapInputResource(enc, mp.mappedResource);
        return ok;
    }

    bool WriteOutput() {
        NV_ENC_LOCK_BITSTREAM lb{};
        lb.version = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = bitstream;
        NVENCSTATUS s = nv.nvEncLockBitstream(enc, &lb);
        if (s != NV_ENC_SUCCESS) return Fail("LockBitstream", s);

        const bool keyframe = (lb.pictureType == NV_ENC_PIC_TYPE_IDR);
        if (out) std::fwrite(lb.bitstreamBufferPtr, 1, lb.bitstreamSizeInBytes, out);
        if (cfg.onPacket && lb.bitstreamSizeInBytes > 0) {
            cfg.onPacket((const uint8_t*)lb.bitstreamBufferPtr, lb.bitstreamSizeInBytes,
                         lb.outputTimeStamp, keyframe);
        }
        totalBytes += lb.bitstreamSizeInBytes;
        ++frameCount;
        if (frameCount <= 5 || frameCount % 60 == 0) {
            std::printf("[NVENC] frame %llu: %u byte%s\n", (unsigned long long)frameCount,
                lb.bitstreamSizeInBytes, keyframe ? " (IDR)" : "");
        }
        nv.nvEncUnlockBitstream(enc, bitstream);
        return true;
    }

    void Finish() {
        if (!enc) return;
        // Gửi EOS để flush nốt.
        NV_ENC_PIC_PARAMS eos{};
        eos.version = NV_ENC_PIC_PARAMS_VER;
        eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        if (nv.nvEncEncodePicture(enc, &eos) == NV_ENC_SUCCESS) WriteOutput();
        if (out) std::fflush(out);
        std::printf("[NVENC] Encoded %llu frame, %.2f MB.\n",
            (unsigned long long)frameCount, totalBytes / 1e6);
    }

    void Cleanup() {
        if (enc) {
            for (auto& kv : registered) nv.nvEncUnregisterResource(enc, kv.second);
            registered.clear();
            if (bitstream) { nv.nvEncDestroyBitstreamBuffer(enc, bitstream); bitstream = nullptr; }
            nv.nvEncDestroyEncoder(enc);
            enc = nullptr;
        }
        if (out) { std::fclose(out); out = nullptr; }
        if (dll) { FreeLibrary(dll); dll = nullptr; }
    }
};

NvencEncoder::NvencEncoder() = default;
NvencEncoder::~NvencEncoder() = default;

bool NvencEncoder::Init(ID3D11Device* device, const EncoderConfig& cfg) {
    impl_ = std::make_unique<Impl>();
    if (!impl_->Init(device, cfg)) { impl_.reset(); return false; }
    return true;
}
bool NvencEncoder::Encode(ID3D11Texture2D* frame, uint64_t ts, bool forceKeyframe) {
    return impl_ && impl_->Encode(frame, ts, forceKeyframe);
}
void NvencEncoder::Finish() { if (impl_) impl_->Finish(); }
