#pragma once
//
// MediaCodecDecoder — đối ứng Android của MfDecoder bên Windows: H.264 Annex-B ->
// khung hình. Khác một điểm căn bản và có lợi: codec được configure THẲNG với
// ANativeWindow của Surface, nên frame giải mã đi từ bộ giải mã phần cứng ra màn
// hình qua hardware composer, không qua CPU và không cần Renderer riêng —
// AMediaCodec_releaseOutputBuffer(..., true) chính là "render".
//
// Dùng trên MỘT thread (thread Decode). Init/Shutdown cũng phải trên thread đó.
//
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include <cstddef>
#include <cstdint>

class MediaCodecDecoder {
public:
    MediaCodecDecoder() = default;
    ~MediaCodecDecoder();
    MediaCodecDecoder(const MediaCodecDecoder&) = delete;
    MediaCodecDecoder& operator=(const MediaCodecDecoder&) = delete;

    // `window` phải sống lâu hơn decoder (chủ sở hữu là main thread của app).
    bool Init(ANativeWindow* window, int width, int height);
    void Shutdown();
    bool IsOpen() const { return codec_ != nullptr; }

    // Nạp một frame Annex-B đã ghép đủ và vẽ các frame đã sẵn sàng.
    // false = lỗi codec -> caller dựng lại decoder và xin IDR.
    bool Decode(const uint8_t* nal, size_t len, uint64_t ptsUs);

    // Số frame đã thực sự đưa lên màn hình kể từ lần gọi trước.
    uint32_t TakeRenderedCount();

    // PTS (đồng hồ host) của frame vừa đưa lên màn hình gần nhất — mốc để tính trễ
    // e2e THẬT (tính lúc nạp vào codec sẽ bỏ sót cả phần decode + hiển thị).
    // 0 = chưa render frame nào.
    uint64_t lastRenderedPtsUs() const { return lastRenderedPtsUs_; }

private:
    // Rút mọi output đang sẵn sàng và render. false = lỗi.
    bool DrainOutput();

    AMediaCodec* codec_ = nullptr;
    bool     sentCsd_ = false;   // đã nạp SPS/PPS dưới cờ CODEC_CONFIG chưa
    uint32_t rendered_ = 0;
    uint64_t lastRenderedPtsUs_ = 0;
};
