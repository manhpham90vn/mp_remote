#pragma once
// =============================================================================
// VtDecoder.h — giải mã H.264 bằng bộ giải mã phần cứng của iOS.
//
// NHIỆM VỤ
//   Nhận frame H.264 Annex-B đã ghép đủ (từ Reassembler) và đưa nó lên màn hình.
//   Đối ứng iOS của MediaCodecDecoder (Android) và MfDecoder+Renderer (Windows).
//
// KHÁC BIỆT CĂN BẢN — VÀ NÓ CÓ LỢI (như bên Android)
//   Frame đi thẳng vào một AVSampleBufferDisplayLayer: layer TỰ giải mã phần cứng
//   VÀ hiển thị qua compositor, KHÔNG qua CPU và không cần lớp Renderer riêng như
//   Windows. Enqueue một CMSampleBuffer chính là toàn bộ thao tác "render" — đúng
//   vai trò mà AMediaCodec_releaseOutputBuffer(..., true) đảm nhận bên Android.
//
// VÌ SAO HEADER THUẦN C++, .mm RIÊNG
//   ClientLoop.cpp là C++ thuần (port sát bản Android) nên không được chứa Obj-C.
//   Header này phơi một lớp C++ trần; layer được truyền xuống dưới dạng void* và
//   bên trong VtDecoder.mm cast lại bằng __bridge. Nhờ vậy ClientLoop khỏi biết gì
//   về AVFoundation/VideoToolbox.
//
// ĐỊNH DẠNG BITSTREAM: Annex-B → AVCC
//   Stream của Deskhub là Annex-B (start code), IDR mang sẵn SPS/PPS in-band
//   (NVENC repeatSPSPPS). VideoToolbox lại đòi AVCC: tham số phải nằm trong một
//   CMVideoFormatDescription (dựng từ SPS/PPS), còn dữ liệu slice phải length-prefix
//   4 byte thay cho start code. VtDecoder.mm lo cả hai phép chuyển đó.
//
// MÔ HÌNH LUỒNG
//   Dùng trên MỘT thread (thread Decode). Init/Shutdown/Decode phải cùng thread đó.
//   Layer thuộc về main thread (SwiftUI) nên bàn giao nó phải qua cơ chế bắt tay ở
//   ClientLoop::SetWindow — y như Surface bên Android.
//
// LIÊN QUAN: ClientLoop.h (chủ sở hữu + luồng Decode), VtDecoder.mm (cài đặt),
//            client/android/.../decode/MediaCodecDecoder.h (bản song song)
// =============================================================================
#include <cstddef>
#include <cstdint>
#include <vector>

class VtDecoder {
public:
    VtDecoder() = default;
    ~VtDecoder();
    VtDecoder(const VtDecoder&) = delete;
    VtDecoder& operator=(const VtDecoder&) = delete;

    // `layer` là một AVSampleBufferDisplayLayer* đã bọc (__bridge void*). Nó phải
    // sống lâu hơn decoder — chủ sở hữu là DeskhubClient (giữ strong ref) cho tới
    // khi ClientLoop::SetWindow(nullptr) bắt tay xong. width/height chỉ để log; kích
    // thước thật lấy từ SPS trong stream.
    bool Init(void* layer, int width, int height);
    void Shutdown();
    bool IsOpen() const {
        return layer_ != nullptr;
    }

    // Nạp một frame Annex-B đã ghép đủ và hiển thị.
    // false = lỗi (layer failed / dựng CMSampleBuffer hỏng) -> caller dựng lại
    // decoder và xin IDR, y như đường lỗi của MediaCodecDecoder.
    bool Decode(const uint8_t* nal, size_t len, uint64_t ptsUs);

    // Số frame đã enqueue để hiển thị kể từ lần gọi trước.
    uint32_t TakeRenderedCount();

    // PTS (đồng hồ host) của frame vừa đưa lên gần nhất — mốc tính trễ e2e.
    //
    // GIỚI HẠN (docs/12 §2, §8): với AVSampleBufferDisplayLayer ta không có callback
    // "đã lên màn hình" cho từng frame, nên mốc này là lúc ENQUEUE chứ chưa phải lúc
    // hiển thị thật — e2e vì thế hụt một khoảng nhỏ (thời gian layer trình bày). Đây
    // là caveat đã biết của bản đầu; muốn chính xác thì chuyển sang nhánh
    // VTDecompressionSession (có callback thời điểm rõ ràng). 0 = chưa frame nào.
    uint64_t lastRenderedPtsUs() const {
        return lastRenderedPtsUs_;
    }

private:
    void* layer_ = nullptr;      // AVSampleBufferDisplayLayer* (không sở hữu)
    void* formatDesc_ = nullptr; // CMVideoFormatDescriptionRef (sở hữu, CFRetain)
    uint32_t rendered_ = 0;
    uint64_t lastRenderedPtsUs_ = 0;

    // SPS/PPS đang dùng — so byte để biết khi nào phải dựng lại formatDesc_.
    uint8_t sps_[256] = {};
    uint8_t pps_[256] = {};
    size_t spsLen_ = 0;
    size_t ppsLen_ = 0;

    // Reusable buffers to avoid heap allocation per frame at 60fps.
    std::vector<uint8_t> avcc_;
};
