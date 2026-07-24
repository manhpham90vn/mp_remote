#pragma once
// =============================================================================
// ClipboardAssembler.h — ghép các mảnh CLIPBOARD thành một lần copy hoàn chỉnh.
//
// NHIỆM VỤ
//   Đối xứng với việc SendClipboard chia văn bản thành mảnh ≤ kMaxClipboardChunk.
//   UDP không bảo đảm gì nên mảnh có thể lạc thứ tự, trùng, hoặc mất — lớp này gom
//   theo updateId, trả văn bản đúng MỘT lần khi đủ mảnh, và biết từ bỏ bản dở dang
//   khi lần copy mới hơn tới (người dùng copy tiếp thì bản cũ hết giá trị).
//
// CHÍNH SÁCH
//   - Chỉ giữ MỘT update đang ghép: updateId khác với bản đang ghép là thay mới.
//     Không giữ nhiều bản song song như Reassembler — clipboard không có "thứ tự
//     phát lại", chỉ bản MỚI NHẤT có nghĩa.
//   - Mảnh của update ĐÃ áp dụng rồi (bên gửi phát trùng) bị bỏ im lặng.
//   - Tổng vượt kMaxClipboardBytes là gói khai điêu/dựng ác ý → huỷ cả update.
//
// MÔ HÌNH LUỒNG
//   Thuần C++20, không khoá — dùng trên MỘT thread (thread Recv, bên trong
//   HandlePacket của HostSession/ClientSession).
//
// LIÊN QUAN: deskhub/wire/Wire.h (định dạng mảnh + các hằng), docs/04-protocol.md §7
// =============================================================================
#include "deskhub/wire/Wire.h"

#include <optional>
#include <string>
#include <vector>

namespace deskhub {

class ClipboardAssembler {
public:
    // Nạp một mảnh. Trả văn bản hoàn chỉnh đúng MỘT lần khi mảnh cuối của update
    // về tới; mọi trường hợp khác (chưa đủ, trùng, hỏng) trả nullopt.
    std::optional<std::string> Push(const ClipboardChunkView& c);

private:
    bool active_ = false;   // đang ghép dở một update
    uint32_t id_ = 0;       // updateId đang ghép
    uint16_t count_ = 0;    // tổng số mảnh của update đó
    uint32_t received_ = 0; // số mảnh đã có
    size_t bytes_ = 0;
    std::vector<std::string> parts_; // theo chunkIndex; rỗng = chưa nhận

    bool haveDone_ = false; // đã từng áp dụng một update
    uint32_t doneId_ = 0;   // updateId gần nhất đã áp dụng — chặn mảnh phát trùng
};

} // namespace deskhub
