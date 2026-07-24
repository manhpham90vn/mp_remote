// =============================================================================
// Packetizer.cpp — cài đặt SendFrame: cắt mảnh, phát gói, tích luỹ parity FEC xen kẽ.
//
// File chỉ có một hàm, gánh hai việc:
//
//   1. CẮT VÀ PHÁT. Frame chia thành các mảnh kMaxVideoPayload byte (mảnh cuối lấy
//      phần dư), mỗi mảnh thành một datagram VIDEO_PACKET gửi ngay trong vòng lặp.
//
//   2. TÍCH LUỸ PARITY XEN KẼ (nếu bật FEC). numGroups = ceil(count/kFecGroupSize)
//      nhóm; mảnh thứ i góp vào parity của nhóm (i % numGroups). Vì các nhóm đan xen
//      nhau, chúng chỉ đóng lại ở CUỐI frame — nên vòng lặp giữ numGroups bộ tích luỹ
//      song song, rồi phát một loạt gói parity sau khi mọi mảnh dữ liệu đã đi. Mất
//      đúng một mảnh trong một nhóm thì bên nhận dựng lại được bằng XOR ngược; chùm
//      mất liên tiếp trải ra nhiều nhóm nên vẫn cứu được (xem Wire.h).
//
// Vẫn CHỈ MỘT LƯỢT đọc dữ liệu: XOR góp vào bộ tích luỹ của nhóm ngay khi cắt mảnh,
// không đọc lại frame lần hai (frame có thể vài chục KB, chạy 60 lần/giây).
//
// VỀ CẤP PHÁT
//   buf_ là mảng thành viên cố định. parity_ là vector thành viên, assign() mỗi frame
//   tái dùng sức chứa nên chỉ cấp phát khi gặp frame lớn hơn mọi frame trước — steady
//   state không cấp phát trên đường nóng.
//
// LIÊN QUAN: deskhub/transport/Packetizer.h (thiết kế + lý do), Reassembler.cpp (đầu kia)
// =============================================================================
#include "deskhub/transport/Packetizer.h"

namespace deskhub {

size_t Packetizer::SendFrame(std::span<const uint8_t> nal, uint32_t frameId,
    uint64_t timestampUs, bool idr, const SendFn& send) {
    if (nal.empty() || !send) return 0;
    // Phép chia làm tròn LÊN: mảnh cuối thường không đầy nhưng vẫn tính là một mảnh.
    const size_t count = (nal.size() + kMaxVideoPayload - 1) / kMaxVideoPayload;
    if (count > 0xFFFF) return 0; // pktIndex/pktCount là u16 — không đánh số nổi
    // Nhóm xen kẽ: numGroups = ceil(count/kFecGroupSize), gói i thuộc nhóm (i % numGroups).
    // groupIndex là u8: quá kMaxFecGroups nhóm thì không đánh số được → gửi trần, không FEC.
    const size_t numGroups = (count + kFecGroupSize - 1) / kFecGroupSize;
    const bool fec = fec_ && numGroups <= kMaxFecGroups;

    VideoHeader vh;
    vh.frameId = frameId;
    vh.timestampUs = timestampUs;
    vh.pktCount = uint16_t(count);

    // numGroups bộ tích luỹ liền nhau trong parity_; bộ của nhóm g bắt đầu ở g*stride.
    // assign() vừa cấp đủ chỗ (tái dùng sức chứa) vừa đặt lại 0 — phần tử trung hoà của
    // XOR là 0 nên mọi nhóm bắt đầu từ nền sạch.
    if (fec) parity_.assign(numGroups * kParityStride, 0);

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
        // Cộng mảnh vào parity của NHÓM XEN KẼ của nó. XOR cả độ dài lẫn dữ liệu: chỉ
        // gói cuối frame ngắn hơn kMaxVideoPayload, mà bên nhận không đoán được độ dài
        // gói THIẾU nếu không có lenXor.
        uint8_t* par = parity_.data() + (i % numGroups) * kParityStride;
        par[0] ^= uint8_t(len >> 8);
        par[1] ^= uint8_t(len & 0xFF);
        for (size_t b = 0; b < len; ++b)
            par[kFecLenPrefix + b] ^= nal[off + b];
    }

    if (!fec) return count;

    // Parity đi SAU toàn bộ dữ liệu: nhóm xen kẽ chỉ đóng lại khi mảnh cuối cùng của
    // frame đã đi, và gửi sau thì lúc parity tới bên nhận đã biết chính xác còn thiếu
    // gói nào. Một gói parity cho mỗi nhóm.
    FecHeader fh;
    fh.frameId = frameId;
    fh.timestampUs = timestampUs;
    fh.pktCount = uint16_t(count);
    for (size_t g = 0; g < numGroups; ++g) {
        fh.groupIndex = uint8_t(g);
        const size_t n = BuildFecPacket(buf_, sessionId_, fh, idr,
            std::span<const uint8_t>(parity_.data() + g * kParityStride, kParityStride));
        if (!n) return 0;
        send(std::span<const uint8_t>(buf_, n));
    }
    return count;
}

} // namespace deskhub
