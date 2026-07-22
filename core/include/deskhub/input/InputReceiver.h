#pragma once
// =============================================================================
// InputReceiver.h — khử trùng và phát lại event input theo đúng thứ tự, phía HOST.
//
// NHIỆM VỤ
//   Đối tác của InputSender. Vì bên gửi cố tình gửi LẶP mỗi event khoảng ba lần
//   để chống kẹt phím, bên nhận bắt buộc phải có chỗ lọc — nếu không, một cú bấm
//   phím sẽ thành ba cú bấm và mọi thứ gõ ra đều nhân ba.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   InputSender (client) → UDP ~~~> HostSession → **InputReceiver** → InputInjector
//
// CƠ CHẾ: MỘT CON SỐ DUY NHẤT
//   Toàn bộ trạng thái của lớp này là lastAppliedSeq_ — seq của event cuối cùng đã
//   áp dụng. Mỗi event mang seq riêng (= firstSeq của gói + chỉ số trong gói), nên
//   luật rất gọn: seq ≤ lastApplied thì bỏ, ngược lại thì áp dụng và cập nhật mốc.
//   Từ luật đó suy ra ba hành vi mong muốn:
//     - event đã áp dụng đến lại (do gửi lặp)     → bỏ, đếm vào duplicates;
//     - gói đến trễ/đảo thứ tự (toàn seq cũ)      → bỏ, không "tua ngược" thao tác;
//     - nhảy seq                                  → đếm được đúng số event mất thật.
//
//   Không có bộ đệm sắp xếp lại, không có cửa sổ trượt: event input đã trễ thì
//   không còn giá trị, giữ lại để áp dụng sau chỉ làm con trỏ chuột giật lùi.
//
// KHÔNG TỰ PHỤC HỒI MẤT MÁT
//   Thống kê `lost` chỉ để quan sát. Việc chống mất gói nằm ở phía gửi (dư thừa +
//   phát lại); ở đây không có gì để yêu cầu gửi lại, và cũng không nên có — một
//   event input đến muộn vài trăm mili-giây thì tệ hơn là không đến.
//
// MÔ HÌNH LUỒNG
//   Thuần C++20, dùng trên MỘT thread (thread Recv của host).
//
// LIÊN QUAN: deskhub/input/InputSender.h (đầu kia), deskhub/session/HostSession.h
// =============================================================================
//
#include "deskhub/wire/Wire.h"

#include <cstdint>
#include <functional>
#include <span>

namespace deskhub {

class InputReceiver {
public:
    using ApplyFn = std::function<void(const InputEvent&)>;

    struct Stats {
        uint64_t packets = 0;
        uint64_t applied = 0;
        uint64_t duplicates = 0; // event đến lại do gửi lặp — bình thường, không phải lỗi
        uint64_t lost = 0;       // lỗ hổng seq không gói lặp nào bù được
    };

    // `payload` = phần sau header chung của gói INPUT_EVENT.
    // Trả true nếu gói hợp lệ (dù mọi event trong đó đều là bản lặp).
    bool HandlePacket(std::span<const uint8_t> payload, const ApplyFn& apply);

    void Reset(); // phiên mới: quên seq đã áp dụng

    const Stats& stats() const { return stats_; }

private:
    int64_t lastAppliedSeq_ = -1; // -1 = chưa nhận event nào
    Stats   stats_{};
};

} // namespace deskhub
