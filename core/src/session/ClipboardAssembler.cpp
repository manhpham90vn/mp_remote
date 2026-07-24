// =============================================================================
// ClipboardAssembler.cpp — cài đặt việc ghép mảnh clipboard (xem .h về chính sách).
//
// Một hàm duy nhất, đi theo thứ tự: lọc mảnh của update đã xong → mở update mới
// nếu cần → kiểm tra mảnh → tích luỹ → trả kết quả khi đủ. Mọi trường đếm do bên
// gửi khai đều được đối chiếu trước khi dùng — dữ liệu đến từ mạng.
// =============================================================================
#include "deskhub/session/ClipboardAssembler.h"

namespace deskhub {

std::optional<std::string> ClipboardAssembler::Push(const ClipboardChunkView& c) {
    // updateId cấp tăng dần ở bên gửi, nên so sánh CÓ DẤU trên hiệu (kiểu seq,
    // chịu được wraparound): mảnh của update đã áp dụng hoặc CŨ HƠN là mảnh lạc
    // trên đường — bỏ im lặng, tuyệt đối không cho nó thay bản đang ghép.
    if (haveDone_ && int32_t(c.updateId - doneId_) <= 0) return std::nullopt;
    if (active_ && c.updateId != id_ && int32_t(c.updateId - id_) < 0) return std::nullopt;

    // Update MỚI HƠN (hoặc mảnh đầu tiên từ trước tới nay): bỏ bản dở dang cũ.
    // Chỉ bản copy mới nhất có nghĩa nên không giữ song song nhiều bản.
    if (!active_ || c.updateId != id_) {
        active_ = true;
        id_ = c.updateId;
        count_ = c.chunkCount;
        received_ = 0;
        bytes_ = 0;
        parts_.assign(count_, {});
    }

    // chunkCount phải nhất quán giữa các mảnh của cùng update — lệch là gói hỏng.
    if (c.chunkCount != count_ || c.chunkIndex >= count_) return std::nullopt;
    std::string& slot = parts_[c.chunkIndex];
    if (!slot.empty()) return std::nullopt; // trùng

    bytes_ += c.data.size();
    if (bytes_ > kMaxClipboardBytes) { // khai điêu — huỷ cả update
        active_ = false;
        parts_.clear();
        return std::nullopt;
    }
    slot.assign(reinterpret_cast<const char*>(c.data.data()), c.data.size());
    if (++received_ < count_) return std::nullopt;

    // Đủ mảnh: nối theo thứ tự chunkIndex, chốt update này là "đã áp dụng".
    std::string text;
    text.reserve(bytes_);
    for (const std::string& s : parts_) text += s;
    parts_.clear();
    active_ = false;
    haveDone_ = true;
    doneId_ = id_;
    return text;
}

} // namespace deskhub
