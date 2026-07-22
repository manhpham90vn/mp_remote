#pragma once
// =============================================================================
// BitrateController.h — chính sách chống nghẽn (congestion control) phía HOST.
//
// NHIỆM VỤ
//   Trả lời một câu hỏi duy nhất, mỗi giây một lần: "client vừa báo mất X% gói,
//   vậy nên phát ở bitrate nào và có cần bật FEC không?". Vào là gói Feedback, ra
//   là một BitrateDecision. Không đụng encoder, không đụng socket, không đọc đồng
//   hồ — nowUs do người gọi bơm vào.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   client: LinkStats → MakeFeedback → UDP
//                                       ~~~> host: HostSession::onFeedback
//                                              → **BitrateController::Update()**
//                                              → encoder.SetBitrate() + Packetizer.SetFecEnabled()
//
// CHIẾN LƯỢC: TỤT NHANH, LÊN CHẬM
//   Mất ≥5% → ×0.75 (lùi mạnh). Mất ≥2% → ×0.90 (lùi nhẹ). Đường truyền sạch và đã
//   nguội 2 giây → +5% trần. Đây là dạng AIMD quen thuộc: nghẽn thì phải thoát cho
//   nhanh vì hàng đợi đang đầy và mỗi mili-giây chậm trễ là thêm gói mất; còn dò
//   lên thì phải chậm, vì dò quá tay chỉ tạo ra đúng cái nghẽn vừa thoát khỏi.
//
// VÌ SAO ĐÁNG TÁCH THÀNH LỚP RIÊNG
//   Đây là chỗ duy nhất trong dự án có ngưỡng và trễ (×0.75/×0.90, +5% trần mỗi
//   giây, 2 giây nguội sau khi tụt, 5 giây sạch mới tắt FEC). Trước đây nó nằm
//   trong một lambda giữa hàm RunAgent 432 dòng nên không cách nào kiểm chứng
//   ngoài việc chạy thật rồi ngồi nhìn log.
//
// MỘT CONTROLLER CHO MỘT NGUỒN
//   Chia sẻ nhiều cửa sổ thì bitrate của chúng độc lập nhau, và client có thể chỉ
//   đang xem một trong số đó.
//
// LIÊN QUAN: deskhub/control/LinkStats.h (bên sinh ra Feedback ở phía client),
//            deskhub/session/HostSession.h (nơi gọi Update)
// =============================================================================
//
#include <cstdint>

#include "deskhub/wire/Wire.h"

namespace deskhub {

struct BitrateDecision {
    uint32_t bitrateBps   = 0;     // mức ĐỀ NGHỊ (đã kẹp trong [min,max])
    bool     changeBitrate = false; // false = giữ nguyên, đừng đàm phán lại rate control
    bool     fecEnabled   = false; // trạng thái FEC mong muốn sau lần cập nhật này
    bool     fecToggled   = false; // true = vừa đổi so với trước (để log một lần)
};

class BitrateController {
public:
    // `startBps` cũng là TRẦN: agent không bao giờ vượt mức người dùng đã chọn.
    BitrateController(uint32_t startBps, uint32_t minBps)
        : cur_(startBps), max_(startBps), min_(minBps) {}

    // Gọi mỗi khi nhận Feedback (client gửi ~1 lần/giây).
    //
    // Bitrate KHÔNG được commit ở đây: encoder có thể từ chối đổi. Caller gọi
    // SetBitrate của encoder trước, thành công thì gọi CommitBitrate() để controller
    // chốt. Thất bại thì bỏ qua — lần Feedback sau tính lại từ mức cũ.
    // Trạng thái FEC thì commit luôn vì không có đường thất bại.
    BitrateDecision Update(const Feedback& fb, uint64_t nowUs);

    void CommitBitrate(uint32_t bps) { cur_ = bps; }

    uint32_t bitrateBps() const { return cur_; }
    bool     fecEnabled() const { return fec_; }

private:
    uint32_t cur_;
    uint32_t max_;
    uint32_t min_;

    uint64_t lastDecreaseUs_ = 0;
    int      cleanSeconds_   = 0;
    bool     fec_            = false;
};

} // namespace deskhub
