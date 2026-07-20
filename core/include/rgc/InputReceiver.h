#pragma once
// InputReceiver — phía host: nhận gói INPUT_EVENT, khử trùng và phát ra theo
// đúng thứ tự phát sinh ở client.
//
// Mỗi event mang seq riêng (= firstSeq của gói + chỉ số trong gói), nên:
//   - event đã áp dụng đến lại (do InputSender gửi lặp) → seq ≤ lastApplied → bỏ;
//   - gói đến trễ/đảo thứ tự → toàn bộ là seq cũ → bỏ, không "tua ngược" thao tác;
//   - nhảy seq → đếm được đúng số event mất thật sự (thống kê, không tự phục hồi).
//
// Thuần C++20, dùng trên MỘT thread (thread Recv của host).
#include "rgc/Wire.h"

#include <cstdint>
#include <functional>
#include <span>

namespace rgc {

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

} // namespace rgc
