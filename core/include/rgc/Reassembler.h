#pragma once
// Reassembler — ghép các gói VIDEO_PACKET (UDP, có thể lạc thứ tự/mất/trùng)
// thành frame NAL hoàn chỉnh, TRẢ THEO THỨ TỰ frameId (H.264 inter-frame cần thứ tự).
//
// Chính sách v1 (docs/06-phase3-transport.md §5):
//   - Giữ tối đa 4 frame đang ghép; gói thuộc frame đã phát/đã bỏ → bỏ.
//   - Frame đầu hàng chưa đủ mảnh mà (a) đã quá 2 khoảng frame kể từ mảnh đầu, hoặc
//     (b) đã có ≥2 frame mới hơn hoàn chỉnh → bỏ frame đó, đánh dấu loss.
//   - Sau loss (và khi mới join): nuốt mọi frame không-IDR cho tới khi gặp IDR —
//     GOP vô hạn nên decode tiếp chỉ sinh vỡ hình; client phải xin IDR
//     (TakeLossEvent / WaitingForIdr → REQUEST_KEYFRAME).
//
// Thuần C++20, không thread, không đồng hồ — thời gian bơm từ ngoài qua `nowUs`.
// Dùng trên MỘT thread (thread Recv của client).
#include "rgc/Wire.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace rgc {

class Reassembler {
public:
    struct Frame {
        uint32_t frameId = 0;
        uint64_t timestampUs = 0;
        bool     idr = false;
        std::vector<uint8_t> nal; // Annex-B, bytes đúng như phía Packetizer đưa vào
    };

    struct Stats {
        uint64_t packetsReceived = 0; // mọi gói đưa vào Push (kể cả trùng/muộn)
        uint64_t framesCompleted = 0; // frame trả ra qua PopReady
        uint64_t framesDropped   = 0; // frame bỏ vì thiếu mảnh (mất gói thật)
        uint64_t framesSkipped   = 0; // frame lành nhưng bị nuốt khi chờ IDR
        uint64_t packetsLost     = 0; // ước lượng: mảnh còn thiếu của các frame đã bỏ
        uint64_t lossEvents      = 0;
    };

    // `frameIntervalUs`: khoảng cách frame kỳ vọng (1e6/fps) — mốc cho timeout bỏ frame.
    explicit Reassembler(uint64_t frameIntervalUs = 16'667)
        : frameIntervalUs_(frameIntervalUs ? frameIntervalUs : 16'667) {}

    void Push(const VideoPacketView& pkt, uint64_t nowUs);

    // Frame kế tiếp theo thứ tự nếu đã đủ mảnh (gọi lặp tới khi trả nullopt).
    std::optional<Frame> PopReady(uint64_t nowUs);

    // true đúng MỘT lần sau mỗi đợt bỏ frame — caller xin keyframe.
    bool TakeLossEvent();

    // Đang nuốt frame chờ IDR (mới join hoặc vừa loss) — caller giữ yêu cầu keyframe.
    bool WaitingForIdr() const { return waitingForIdr_; }

    const Stats& stats() const { return stats_; }

private:
    struct Pending {
        std::vector<std::vector<uint8_t>> pieces; // theo pktIndex; rỗng = chưa nhận
        uint16_t pktCount = 0;
        uint16_t received = 0;
        uint64_t timestampUs = 0;
        bool     idr = false;
        uint64_t firstSeenUs = 0;
        size_t   bytes = 0;
        bool Complete() const { return pktCount != 0 && received == pktCount; }
    };
    using PendingMap = std::map<uint32_t, Pending>;

    void Drop(PendingMap::iterator it, bool loss);

    static constexpr size_t kMaxPendingFrames = 4;

    PendingMap pending_;
    uint64_t frameIntervalUs_;
    bool     waitingForIdr_ = true; // join giữa chừng: chờ IDR đầu tiên
    bool     lossEvent_ = false;
    bool     haveBarrier_ = false;  // đã có mốc frameId không nhận lùi
    uint32_t barrierId_ = 0;        // frame ≤ mốc này đã phát hoặc đã bỏ
    Stats    stats_;
};

} // namespace rgc
