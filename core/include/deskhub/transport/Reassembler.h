#pragma once
// =============================================================================
// Reassembler.h — ghép mảnh UDP thành frame hoàn chỉnh, phía CLIENT.
//
// NHIỆM VỤ
//   Đảo ngược việc Packetizer đã làm, nhưng trong điều kiện khắc nghiệt hơn nhiều:
//   UDP không bảo đảm gì cả, nên mảnh có thể đến LẠC THỨ TỰ, TRÙNG, hoặc MẤT HẲN.
//   Lớp này gom mảnh theo frameId, biết khi nào một frame đã đủ, và — quan trọng
//   nhất — biết khi nào nên TỪ BỎ một frame thay vì chờ mãi.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   UDP ~~~> **Reassembler** → IVideoDecoder → Renderer
//   Đây là nơi duy nhất trong client hiểu "gói" — từ PopReady trở đi mọi thứ làm
//   việc với frame nguyên vẹn.
//
// BA BÀI TOÁN PHẢI GIẢI CÙNG LÚC
//   1. GHÉP. Mảnh đến lộn xộn nên phải có chỗ chứa dở dang (Pending) cho nhiều
//      frame song song, tối đa kMaxPendingFrames = 4.
//   2. THỨ TỰ. H.264 dùng inter-frame: frame N tham chiếu frame N-1. Giải mã lệch
//      thứ tự cho ra hình vỡ, nên PopReady chỉ trả frame theo đúng frameId tăng dần
//      và không bao giờ nhảy cóc qua frame chưa xong.
//   3. TỪ BỎ ĐÚNG LÚC. Chờ một mảnh không bao giờ tới sẽ treo cả luồng hình. Nhưng
//      bỏ quá sớm thì phí một frame lẽ ra cứu được. Cân bằng này là phần "Chính
//      sách v1" ngay bên dưới.
//
// KHÔI PHỤC BẰNG FEC
//   Nếu host bật FEC, mỗi nhóm kFecGroupSize mảnh có kèm một gói parity. Thiếu
//   ĐÚNG một mảnh trong nhóm thì TryRecover dựng lại được bằng XOR ngược — frame
//   vẫn hoàn chỉnh, không phải bỏ và không phải xin IDR. Thiếu từ hai mảnh trở lên
//   thì parity vô dụng (một phương trình không giải nổi hai ẩn).
//
// VÌ SAO PHẢI CHỜ IDR SAU KHI MẤT FRAME
//   Encoder dùng GOP vô hạn (không tự phát IDR định kỳ, vì IDR nặng gấp nhiều lần
//   P-frame). Mất một frame nghĩa là mọi frame sau đó tham chiếu vào dữ liệu client
//   không có → giải mã tiếp chỉ sinh vỡ hình lem luốc kéo dài. Nên khi mất, lớp này
//   nuốt sạch frame non-IDR và bật cờ để client xin REQUEST_KEYFRAME.
//
// MÔ HÌNH LUỒNG
//   Thuần C++20, không thread, không đồng hồ — thời gian bơm từ ngoài qua `nowUs`
//   (nhờ vậy test tua nhanh thời gian được, không phải sleep thật). Dùng trên MỘT
//   thread (thread Recv của client).
//
// LIÊN QUAN: deskhub/transport/Packetizer.h (đầu kia), deskhub/control/LinkStats.h (đọc
//            stats() của lớp này), docs/06-phase3-transport.md §5
// =============================================================================
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
#include "deskhub/wire/Wire.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace deskhub {

class Reassembler {
public:
    struct Frame {
        uint32_t frameId = 0;
        uint64_t timestampUs = 0;
        bool     idr = false;
        // Thời điểm mảnh ĐẦU TIÊN của frame này tới — client trừ đi để đo thời gian
        // ghép (t_asm trong log chẩn đoán). Không dùng vào logic nào của lớp này.
        uint64_t firstSeenUs = 0;
        std::vector<uint8_t> nal; // Annex-B, bytes đúng như phía Packetizer đưa vào
    };

    // Vì sao một frame bị khai tử — phát ra qua onFrameDrop để client ghi log.
    enum class DropReason : uint8_t {
        Timeout,   // quá 2 khoảng frame kể từ mảnh đầu mà vẫn thiếu mảnh
        Overtaken, // ≥2 frame mới hơn đã hoàn chỉnh vượt mặt
        Evicted,   // bị đẩy ra vì hàng chờ đầy (kMaxPendingFrames)
        PreIdr,    // frame LÀNH nhưng bị nuốt khi đang chờ IDR (không phải mất gói)
    };

    // Bản khám nghiệm một frame vừa bị bỏ. `firstMissing`/`lastMissing` cho biết
    // chùm thiếu nằm ở ĐẦU hay ĐUÔI frame (lastMissing == total-1 nghĩa là thủng
    // đuôi — dấu hiệu đặc trưng của burst, xem docs/06 §7b). Với PreIdr thì
    // missing == 0 và hai trường vị trí vô nghĩa.
    struct FrameDropInfo {
        uint32_t   frameId = 0;
        DropReason reason  = DropReason::Timeout;
        uint16_t   missing = 0, total = 0;
        uint16_t   firstMissing = 0, lastMissing = 0;
        bool       idr = false;
        uint32_t   waitedMs = 0;  // từ mảnh đầu tới lúc khai tử
        uint32_t   bytesGot = 0;  // byte đã nhận được của frame này
    };

    // Callback chẩn đoán, tùy chọn. Core không I/O — client nối vào printf/logcat.
    // Gọi trên chính thread đang gọi Push/PopReady, tần suất = số frame bị bỏ (hiếm).
    std::function<void(const FrameDropInfo&)> onFrameDrop;

    struct Stats {
        // CHỈ gói dữ liệu (Push), không tính parity: caller lấy tỉ lệ mất gói từ
        // packetsLost/(packetsReceived+packetsLost), trộn parity vào mẫu số sẽ làm
        // tỉ lệ đó tụt xuống đúng lúc FEC đang bật — tức là đúng lúc đang mất gói.
        uint64_t packetsReceived = 0; // mọi gói đưa vào Push (kể cả trùng/muộn)
        uint64_t fecReceived     = 0; // gói parity đưa vào PushFec
        uint64_t framesCompleted = 0; // frame trả ra qua PopReady
        uint64_t framesDropped   = 0; // frame bỏ vì thiếu mảnh (mất gói thật)
        uint64_t framesSkipped   = 0; // frame lành nhưng bị nuốt khi chờ IDR
        uint64_t packetsLost     = 0; // ước lượng: mảnh còn thiếu của các frame đã bỏ
        uint64_t lossEvents      = 0;
        uint64_t packetsRecovered = 0; // mảnh dựng lại được từ parity FEC

        // Phân bố ĐỘ DÀI CHÙM gói mất liên tiếp trong một frame bị bỏ, thang gấp đôi:
        // [0]=1, [1]=2, [2]=3, [3]=4..7, [4]=8..15, [5]=16..31, [6]=≥32.
        // Đây là con số quyết định thiết kế FEC, không phải tỉ lệ mất gói: parity XOR
        // trên kFecGroupSize gói LIÊN TIẾP cứu được đúng một gói mỗi nhóm, nên chùm ≥2
        // (rơi trọn vào một nhóm) làm FEC vô dụng. Loss 5% toàn chùm-1 và loss 5% toàn
        // chùm-20 đòi hai cách chữa hoàn toàn khác nhau — cái đầu interleave là xong,
        // cái sau phải chặn từ gốc (pacing/bitrate) vì không parity nào gánh nổi.
        uint64_t lossRuns[7] = {};
        uint64_t lossRunMax  = 0; // chùm dài nhất từng thấy, tính bằng gói

        // Gói VỀ MUỘN: mảnh tới SAU khi frame của nó đã bị khai tử vì "mất gói".
        // Đây là phép đo phân xử câu hỏi mở của docs/06 §7b: nếu latePackets chiếm
        // phần lớn packetsLost thì "loss" thật ra là TỚI MUỘN — deadline ghép frame
        // hết hạn trước khi đuôi kịp tới — và cách chữa (nới deadline, giảm cỡ
        // frame) khác hẳn với mất gói thật (bitrate, FEC, đường truyền).
        uint64_t latePackets = 0;
        uint64_t lateMsSum   = 0; // tổng độ muộn — chia latePackets ra trung bình
        uint64_t lateMsMax   = 0;
    };

    // `frameIntervalUs`: khoảng cách frame kỳ vọng (1e6/fps) — mốc cho timeout bỏ frame.
    explicit Reassembler(uint64_t frameIntervalUs = 16'667)
        : frameIntervalUs_(frameIntervalUs ? frameIntervalUs : 16'667) {}

    void Push(const VideoPacketView& pkt, uint64_t nowUs);

    // Gói parity FEC. Nếu nhóm nó phủ đang thiếu ĐÚNG một mảnh thì mảnh đó được
    // dựng lại ngay tại đây và frame có thể hoàn chỉnh mà không cần xin IDR.
    void PushFec(const FecPacketView& pkt, uint64_t nowUs);

    // Frame kế tiếp theo thứ tự nếu đã đủ mảnh (gọi lặp tới khi trả nullopt).
    std::optional<Frame> PopReady(uint64_t nowUs);

    // true đúng MỘT lần sau mỗi đợt bỏ frame — caller xin keyframe.
    bool TakeLossEvent();

    // Đang nuốt frame chờ IDR (mới join hoặc vừa loss) — caller giữ yêu cầu keyframe.
    bool WaitingForIdr() const { return waitingForIdr_; }

    const Stats& stats() const { return stats_; }

    // Khoảng lặng DÀI NHẤT giữa hai gói video liên tiếp kể từ lần gọi trước (ms),
    // đọc-và-xoá. Client in mỗi 1s: gap ~trăm ms là dấu hiệu Wi-Fi nghẽn/power-save
    // — gói dồn cục ở đâu đó trên đường rồi mới về một thể.
    uint32_t TakeMaxGapMs() {
        const uint32_t g = maxGapMs_;
        maxGapMs_ = 0;
        return g;
    }

private:
    struct Pending {
        std::vector<std::vector<uint8_t>> pieces; // theo pktIndex; rỗng = chưa nhận
        // Parity đã nhận theo groupIndex (dữ liệu gồm cả 2 byte lenXor đứng đầu).
        // Giữ lại vì parity có thể tới TRƯỚC gói dữ liệu cuối của nhóm bị đảo thứ tự.
        std::map<uint8_t, std::vector<uint8_t>> parity;
        uint16_t pktCount = 0;
        uint16_t received = 0;
        uint64_t timestampUs = 0;
        bool     idr = false;
        uint64_t firstSeenUs = 0;
        size_t   bytes = 0;
        bool Complete() const { return pktCount != 0 && received == pktCount; }
    };
    using PendingMap = std::map<uint32_t, Pending>;

    void Drop(PendingMap::iterator it, DropReason reason, uint64_t nowUs);
    // Tạo/lấy chỗ ghép cho frame `id`; nullptr nếu gói thuộc frame đã phát/đã bỏ.
    Pending* Slot(uint32_t id, uint16_t pktCount, uint64_t timestampUs, uint64_t nowUs);
    // Thử dựng lại mảnh thiếu của nhóm `group` trong `f`. true = vừa khôi phục được.
    bool TryRecover(Pending& f, uint8_t group);
    // Gói thuộc frame ĐÃ khai tử vì mất gói — cập nhật thống kê "về muộn" (C2).
    void NoteLatePacket(uint32_t id, uint64_t nowUs);

    static constexpr size_t kMaxPendingFrames = 4;

    PendingMap pending_;
    uint64_t frameIntervalUs_;
    bool     waitingForIdr_ = true; // join giữa chừng: chờ IDR đầu tiên
    bool     lossEvent_ = false;
    bool     haveBarrier_ = false;  // đã có mốc frameId không nhận lùi
    uint32_t barrierId_ = 0;        // frame ≤ mốc này đã phát hoặc đã bỏ
    Stats    stats_;

    // "Nghĩa địa" — ring các frame vừa khai tử vì mất gói, để nhận diện gói của
    // chúng còn lết về sau đó (đo "mất thật vs tới muộn", xem Stats::latePackets).
    // 16 entry ≈ 0.25s @60fps — gói muộn hơn thế thì tính là mất luôn cũng đúng.
    struct Grave { uint32_t frameId = 0; uint64_t dropUs = 0; };
    static constexpr size_t kGraveyardSize = 16;
    Grave    graveyard_[kGraveyardSize];
    size_t   graveNext_ = 0;

    uint64_t lastPushUs_ = 0; // mốc đo khoảng lặng giữa hai gói video liên tiếp
    uint32_t maxGapMs_ = 0;
};

} // namespace deskhub
