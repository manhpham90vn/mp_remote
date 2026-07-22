// =============================================================================
// Packetizer.cpp — cài đặt SendFrame: cắt mảnh, phát gói, tích luỹ parity FEC.
//
// File chỉ có một hàm, nhưng nó gánh hai việc đan xen nhau trong cùng một vòng lặp:
//
//   1. CẮT VÀ PHÁT. Frame được chia thành các mảnh kMaxVideoPayload byte, mảnh
//      cuối lấy phần dư. Mỗi mảnh thành một datagram VIDEO_PACKET gửi ngay.
//
//   2. TÍCH LUỸ PARITY (nếu bật FEC). Cứ kFecGroupSize mảnh liên tiếp thì XOR lại
//      với nhau thành một gói parity, phát ngay sau mảnh cuối của nhóm. Mất đúng
//      một mảnh trong nhóm thì bên nhận dựng lại được bằng phép XOR ngược.
//
// Hai việc này nằm chung một vòng lặp thay vì tách hai lượt vì frame có thể nặng
// vài chục KB: lặp hai lượt nghĩa là đọc lại toàn bộ dữ liệu lần nữa, trên đường
// nóng chạy 60 lần mỗi giây.
//
// VÌ SAO KHÔNG CẤP PHÁT GÌ Ở ĐÂY
//   Hàm này chạy mỗi frame, tức 60 lần/giây với mỗi lần vài chục datagram. Bộ đệm
//   buf_ và parity_ là thành viên của lớp, dùng lại qua từng lần gọi; không có
//   std::vector nào được tạo trên đường này.
//
// LIÊN QUAN: deskhub/transport/Packetizer.h (thiết kế + lý do), Reassembler.cpp (đầu kia)
// =============================================================================
#include "deskhub/transport/Packetizer.h"

#include <cstring>

namespace deskhub {

size_t Packetizer::SendFrame(std::span<const uint8_t> nal, uint32_t frameId,
                             uint64_t timestampUs, bool idr, const SendFn& send) {
    if (nal.empty() || !send) return 0;
    // Phép chia làm tròn LÊN: mảnh cuối thường không đầy nhưng vẫn tính là một mảnh.
    const size_t count = (nal.size() + kMaxVideoPayload - 1) / kMaxVideoPayload;
    if (count > 0xFFFF) return 0; // pktIndex/pktCount là u16 — không đánh số nổi
    // groupIndex là u8: quá 256 nhóm thì không đánh số được → gửi trần, không FEC.
    const bool fec = fec_ && (count + kFecGroupSize - 1) / kFecGroupSize <= 256;

    VideoHeader vh;
    vh.frameId     = frameId;
    vh.timestampUs = timestampUs;
    vh.pktCount    = uint16_t(count);

    FecHeader fh;
    fh.frameId     = frameId;
    fh.timestampUs = timestampUs;
    fh.pktCount    = uint16_t(count);

    // Parity đi SAU cả nhóm: gửi trước thì nó tới trước gói dữ liệu và bên nhận phải
    // giữ chỗ chờ; gửi sau thì lúc nó tới ta đã biết chính xác còn thiếu gói nào.
    auto flushParity = [&](size_t groupIdx) -> bool {
        fh.groupIndex = uint8_t(groupIdx);
        const size_t n = BuildFecPacket(buf_, sessionId_, fh, idr,
                                        std::span<const uint8_t>(parity_, sizeof(parity_)));
        if (!n) return false;
        send(std::span<const uint8_t>(buf_, n));
        return true;
    };

    // Phần tử trung hoà của XOR là 0, nên nhóm mới luôn bắt đầu từ bộ đệm sạch.
    if (fec) std::memset(parity_, 0, sizeof(parity_));

    for (size_t i = 0; i < count; ++i) {
        // Vị trí mảnh suy từ chỉ số vì mọi mảnh trừ mảnh cuối đều dài bằng nhau —
        // đây chính là quy ước cho phép bỏ trường offset khỏi header video.
        const size_t off = i * kMaxVideoPayload;
        const size_t len = (nal.size() - off < kMaxVideoPayload) ? nal.size() - off
                                                                 : kMaxVideoPayload;
        vh.pktIndex = uint16_t(i);
        const bool frameEnd = (i + 1 == count);
        const size_t n = BuildVideoPacket(buf_, sessionId_, vh, idr, frameEnd,
                                          nal.subspan(off, len));
        if (!n) return 0;
        send(std::span<const uint8_t>(buf_, n));

        if (!fec) continue;
        // XOR cả độ dài lẫn dữ liệu. Chỉ gói cuối frame ngắn hơn kMaxVideoPayload,
        // nhưng bên nhận không đoán được độ dài gói THIẾU nếu không có lenXor.
        parity_[0] ^= uint8_t(len >> 8);
        parity_[1] ^= uint8_t(len & 0xFF);
        for (size_t b = 0; b < len; ++b)
            parity_[kFecLenPrefix + b] ^= nal[off + b];

        // Nhóm đóng lại khi đủ kFecGroupSize mảnh, HOẶC khi frame hết — nhóm cuối
        // của frame thường không đầy, và parity của nhóm lẻ đó vẫn có ích (nhóm một
        // phần tử thì parity đúng bằng bản sao của mảnh đó).
        const bool groupEnd = ((i + 1) % kFecGroupSize == 0) || frameEnd;
        if (groupEnd) {
            if (!flushParity(i / kFecGroupSize)) return 0;
            std::memset(parity_, 0, sizeof(parity_));
        }
    }
    return count;
}

} // namespace deskhub
