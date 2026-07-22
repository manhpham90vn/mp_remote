#pragma once
// =============================================================================
// LinkStats.h — quy đổi bộ đếm tích luỹ thành số liệu MỘT CỬA SỔ, phía client.
//
// NHIỆM VỤ
//   Reassembler đếm mọi thứ theo kiểu TÍCH LUỸ từ đầu phiên (tổng gói nhận, tổng
//   gói mất...). Nhưng thứ có ý nghĩa để hiển thị và để báo cáo cho host lại là số
//   liệu của MỘT GIÂY VỪA RỒI. Lớp này giữ ảnh chụp lần trước, lấy hiệu, rồi chia
//   cho thời gian thật đã trôi để ra fps / kbps / % mất gói.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   Reassembler::Stats ──┐
//   bộ đếm byte video  ──┼─→ **LinkStats::Close()** → LinkWindow ─→ overlay/log
//   số frame đã render ──┘                                       └─→ MakeFeedback()
//                                                                     → host → BitrateController
//
// VÌ SAO TÁCH RA
//   Hai client (Windows, Android) trước đây chép nguyên khối tính lossPct và dựng
//   Feedback — giống nhau từng dòng, kể cả cái làm tròn +0.5. Chỉ phần IN RA là
//   khác nhau thật (printf/wchar overlay vs LOGI/logcat), nên chỗ đó vẫn để ở từng
//   client; phần TÍNH nằm hết ở đây và test được offline.
//
// Vì sao tách ra: hai client (Windows, Android) trước đây chép nguyên khối tính
// lossPct và dựng Feedback — giống nhau từng dòng, kể cả cái làm tròn +0.5. Chỉ
// phần IN RA là khác nhau thật (printf/wchar overlay vs LOGI/logcat), nên chỗ đó
// vẫn để ở từng client; phần TÍNH nằm hết ở đây và test được offline.
//
// MÔ HÌNH LUỒNG
//   Không có atomic/mutex trong này: bộ đếm byte và số frame đã render do luồng
//   khác ghi, client tự giữ atomic của mình rồi truyền số đã đọc vào Close(). Nhờ
//   vậy LinkStats thuần logic, không dính mô hình luồng của bất kỳ nền tảng nào.
//
// LIÊN QUAN: deskhub/transport/Reassembler.h (nguồn số liệu),
//            deskhub/control/BitrateController.h (bên nhận Feedback ở phía host)
// =============================================================================
#include <cstdint>

#include "deskhub/transport/Reassembler.h"
#include "deskhub/wire/Wire.h"

namespace deskhub {

// Số liệu dẫn xuất của một cửa sổ vừa đóng. Mọi trường "…InWindow" là CHÊNH LỆCH
// so với lần đóng trước, không phải tổng tích luỹ.
struct LinkWindow {
    double   secs    = 0.0; // độ dài thật của cửa sổ
    double   fps     = 0.0; // frame render được / giây
    double   kbps    = 0.0; // bitrate video nhận được
    double   lossPct = 0.0; // % gói dữ liệu mất (KHÔNG tính parity vào mẫu số)

    uint64_t packetsReceived  = 0;
    uint64_t packetsLost      = 0;
    uint64_t packetsRecovered = 0; // dựng lại được nhờ parity FEC
    uint64_t framesDropped    = 0;

    // Phân bố độ dài chùm mất, cùng thang với Reassembler::Stats::lossRuns.
    uint64_t lossRuns[7]  = {};
    uint64_t lossRunTotal = 0; // tổng số chùm — 0 nghĩa là giây vừa rồi không mất gói
    uint64_t lossRunMax   = 0; // chùm dài nhất TỪNG thấy (tích luỹ, không phải delta)

    // Gói "về muộn" trong cửa sổ (xem Reassembler::Stats::latePackets): gói của
    // frame đã khai tử mà còn lết về. lateMsAvg tính trên cửa sổ; lateMsMax tích luỹ.
    uint64_t latePackets = 0;
    double   lateMsAvg   = 0.0;
    uint64_t lateMsMax   = 0;
};

class LinkStats {
public:
    explicit LinkStats(uint64_t startUs, uint64_t windowUs = 1'000'000)
        : lastUs_(startUs), windowUs_(windowUs) {}

    // Đã đủ một cửa sổ chưa. Client gọi mỗi vòng lặp.
    bool Due(uint64_t nowUs) const { return nowUs - lastUs_ >= windowUs_; }

    // Đóng cửa sổ và mở cửa sổ mới tại nowUs. `videoBytes` / `renderedFrames` là
    // số đếm được TRONG cửa sổ này (client tự reset bộ đếm của nó sau khi gọi).
    LinkWindow Close(const Reassembler::Stats& cur, uint64_t videoBytes,
                     uint32_t renderedFrames, uint64_t nowUs);

private:
    Reassembler::Stats prev_{};
    uint64_t lastUs_;
    uint64_t windowUs_;
};

// Dựng gói Feedback từ cửa sổ vừa đóng. Client gửi cả khi lossPct == 0: host cần
// tín hiệu "đường thông" mới dám nới bitrate lên lại, im lặng bị hiểu là mất kết nối.
Feedback MakeFeedback(const LinkWindow& w, uint32_t rttUs);

} // namespace deskhub
