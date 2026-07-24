#pragma once
// =============================================================================
// Wire.h — đặc tả giao thức v1: kiểu thông điệp, hằng số, và API dựng/giải mã gói.
//
// NHIỆM VỤ
//   Định nghĩa DUY NHẤT về "một byte ở vị trí nào có ý nghĩa gì" trong dự án.
//   Mọi thứ chạy trên UDP đều đi qua đây: host dựng gói bằng các hàm Build*,
//   client giải bằng các hàm Parse* (và ngược lại). Không module nào khác được
//   phép tự đọc/ghi byte của datagram.
//   Đặc tả bằng văn bản nằm ở docs/04-protocol.md và là NGUỒN CHÂN LÝ — sửa mã ở
//   đây mà không sửa tài liệu là làm hai bản đặc tả mâu thuẫn nhau.
//
// CẤU TRÚC MỘT DATAGRAM
//   [ header chung 8 byte ][ payload riêng theo từng loại thông điệp ]
//   Header chung (CommonHeader): ver(1) type(1) flags(1) chan(1) sessionId(4).
//   Gói video/FEC có thêm một header con (16 byte) đứng đầu payload.
//   Mọi trường số là big-endian (xem ByteOrder.h).
//
// VÌ SAO THUẦN C++20, KHÔNG HEADER HỆ ĐIỀU HÀNH
//   File này được biên dịch bởi cả Agent Windows (MSVC), client Windows, và
//   client Android (NDK/clang), sau này thêm macOS/iOS/Ubuntu. Giữ nó không phụ
//   thuộc nền tảng là điều kiện để cả hai đầu của đường truyền dùng CHUNG một mã
//   nguồn — hai bản cài đặt song song của cùng một giao thức chắc chắn sẽ lệch
//   nhau, và lỗi lệch giao thức rất khó chẩn đoán qua UDP.
//
// QUY ƯỚC CHUNG CỦA API
//   Build*(out, ...) → ghi TRỌN một datagram vào `out`, trả số byte đã ghi, hoặc
//                      0 nếu `out` không đủ chỗ. Không bao giờ ghi tràn.
//   Parse*(payload)  → trả std::optional, nullopt nếu gói ngắn/sai định dạng.
//   Không hàm nào cấp phát bộ nhớ động (trừ std::string tên nguồn) và không hàm
//   nào giữ trạng thái — an toàn khi gọi từ nhiều thread trên các bộ đệm khác nhau.
//
// LIÊN QUAN
//   deskhub/wire/ByteOrder.h — lớp dịch byte bên dưới
//   deskhub/transport/Packetizer.h, Reassembler.h — người dùng chính của kênh video
//   deskhub/session/HostSession.h, ClientSession.h — người dùng chính của kênh control
//   docs/04-protocol.md — đặc tả gốc
// =============================================================================
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace deskhub {

inline constexpr uint8_t kProtocolVersion = 1;
inline constexpr size_t kMaxDatagram = 1200; // an toàn MTU Internet
inline constexpr size_t kCommonHeaderSize = 8;
inline constexpr size_t kVideoHeaderSize = 16;
inline constexpr size_t kFecHeaderSize = 16;

// FEC (GĐ5): mỗi frame video kèm các gói parity XOR để dựng lại gói mất mà không phải
// bỏ cả frame và xin IDR (IDR to gấp nhiều lần P-frame; mất gói lúc đang nghẽn mà đáp
// bằng IDR là đổ thêm dầu vào lửa).
//
// CHIA NHÓM XEN KẼ (interleaved). Frame N gói chia thành numGroups = ceil(N/kFecGroupSize)
// nhóm; gói thứ i thuộc nhóm (i % numGroups) — KHÔNG phải các gói liên tiếp. Mỗi nhóm một
// gói parity, dựng lại được ĐÚNG 1 gói mất trong nhóm. Vì hai gói cùng nhóm cách nhau
// numGroups vị trí, một CHÙM mất tới numGroups gói LIÊN TIẾP chỉ đụng mỗi nhóm một gói →
// vẫn cứu được trọn. Đây là điểm hơn hẳn cách gom liên tiếp trước đây (chùm ≥2 là chịu),
// mà chi phí băng thông vẫn y hệt = 1/kFecGroupSize. Mất ≥2 gói CÙNG một nhóm thì vẫn chịu.
// kFecGroupSize vì thế là cỡ nhóm mục tiêu (điều tiết overhead), numGroups là sức chịu chùm.
inline constexpr uint16_t kFecGroupSize = 8;

// groupIndex trên wire là u8 → tối đa 256 nhóm. Frame cần nhiều nhóm hơn (rất to) thì
// Packetizer gửi trần, không FEC — xem Packetizer::SendFrame.
inline constexpr size_t kMaxFecGroups = 256;

// Parity phải XOR được cả ĐỘ DÀI (gói cuối frame ngắn hơn các gói khác), nên payload
// của nó là 2 byte lenXor + dữ liệu XOR. Gói FEC vì thế chật hơn gói video thường →
// lấy nó làm ràng buộc để cả hai loại vừa một datagram.
inline constexpr size_t kFecLenPrefix = 2;
inline constexpr size_t kMaxVideoPayload =
    kMaxDatagram - kCommonHeaderSize - kFecHeaderSize - kFecLenPrefix; // 1174

inline constexpr size_t kInputHeaderSize = 5; // seq(u32) + count(u8)
inline constexpr size_t kInputEventSize = 19; // evType+ts+a+b+state+absolute
// Số event tối đa nhét vừa một datagram (thực tế gửi ít hơn nhiều).
inline constexpr size_t kMaxInputEvents =
    (kMaxDatagram - kCommonHeaderSize - kInputHeaderSize) / kInputEventSize; // 62

// NACK (GĐ7): frameId(u32) + count(u8) + count×pktIndex(u16). Số chỉ số tối đa vừa một
// datagram — thực tế client chỉ xin vài mảnh thiếu của frame đầu hàng.
inline constexpr size_t kNackHeaderSize = 5; // frameId(u32) + count(u8)
inline constexpr size_t kMaxNackIndices =
    (kMaxDatagram - kCommonHeaderSize - kNackHeaderSize) / 2; // 593

// CLIPBOARD (GĐ8): đồng bộ clipboard văn bản HAI CHIỀU. Văn bản UTF-8 có thể vượt
// một datagram nên chia mảnh: updateId đổi theo mỗi lần copy, bên nhận ghép đủ
// chunkCount mảnh (lạc thứ tự được) rồi mới áp dụng — xem ClipboardAssembler.
// Best-effort như phần còn lại của kênh control: mất mảnh thì bản copy đó bỏ,
// lần copy sau tự thay thế.
inline constexpr size_t kClipboardHeaderSize = 8; // updateId(4) chunkIndex(2) chunkCount(2)
inline constexpr size_t kMaxClipboardChunk =
    kMaxDatagram - kCommonHeaderSize - kClipboardHeaderSize; // 1184
// Trần MỘT lần copy. 64 KB văn bản là rất nhiều; to hơn thường là dữ liệu nhị phân
// dán nhầm, mà mỗi lần copy phát ~56 datagram liền nhau đã là cả một cụm burst.
inline constexpr size_t kMaxClipboardBytes = 64 * 1024;

enum class Chan : uint8_t { Control = 0,
    Video = 1,
    Input = 2,
    Audio = 3 };

enum class MsgType : uint8_t {
    Hello = 0x01,
    HelloAck = 0x02,
    Start = 0x03,
    Bye = 0x04,
    ListSources = 0x05, // GĐ6: client hỏi host đang chia sẻ những cửa sổ nào
    SourceList = 0x06,  // GĐ6: host trả danh sách
    VideoPacket = 0x10,
    FecPacket = 0x11,  // GĐ5: parity XOR cho một nhóm gói video
    InputEvent = 0x20, // GĐ4
    Ping = 0x30,
    Pong = 0x31,
    Feedback = 0x32,
    RequestKeyframe = 0x33,
    Reconfig = 0x34,
    SetFocus = 0x35,      // GĐ6: client báo cửa sổ preview của nguồn này vừa được focus
    Nack = 0x36,          // GĐ7: client xin host GỬI LẠI các mảnh video còn thiếu của 1 frame
    InvalidateRef = 0x37, // GĐ7: client báo đã bỏ hẳn 1 frame → host đừng tham chiếu nó nữa
    Clipboard = 0x38,     // GĐ8: một mảnh văn bản clipboard (hai chiều)
};

// Flags của VIDEO_PACKET
inline constexpr uint8_t kVideoFlagIdr = 1u << 0;
inline constexpr uint8_t kVideoFlagFrameEnd = 1u << 1;

// codecMask trong HELLO / codec trong HELLO_ACK
inline constexpr uint16_t kCodecMaskH264 = 1u << 0;
inline constexpr uint16_t kCodecMaskHevc = 1u << 1;
inline constexpr uint16_t kCodecMaskAv1 = 1u << 2;
enum class Codec : uint8_t { H264 = 0,
    Hevc = 1,
    Av1 = 2,
    Rejected = 0xFF };

struct CommonHeader {
    uint8_t ver;
    MsgType type;
    uint8_t flags;
    Chan chan;
    uint32_t sessionId; // 0 trong HELLO/HELLO_ACK; Agent cấp trong HELLO_ACK
};

// Một host chia sẻ nhiều cửa sổ cùng lúc. Mỗi cửa sổ là một "nguồn" có sourceId
// riêng, và mỗi cặp (client, nguồn) là một PHIÊN ĐỘC LẬP với sessionId riêng —
// không nhét streamId vào header video/input. Nhờ vậy kênh video, input, FEC,
// congestion control giữ nguyên y hệt trường hợp một nguồn, và mỗi nguồn tự điều
// chỉnh bitrate / xin IDR theo tình trạng của riêng nó (mỗi nguồn một encoder).
inline constexpr size_t kMaxSources = 8;
inline constexpr size_t kMaxSourceNameBytes = 64; // tiêu đề cửa sổ, UTF-8, cắt bớt

struct SourceInfo {
    uint8_t sourceId = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    std::string name; // tiêu đề cửa sổ (UTF-8), chỉ để hiển thị
};

struct Hello {
    uint32_t clientId;
    uint16_t codecMask;
    uint16_t maxWidth;
    uint16_t maxHeight;
    uint8_t desiredFps;
    uint16_t features;
    uint8_t sourceId; // nguồn muốn xem (lấy từ SOURCE_LIST; 0 = nguồn đầu tiên)
};

struct HelloAck {
    uint32_t sessionId;
    Codec codec; // Rejected (0xFF) = từ chối
    uint16_t width;
    uint16_t height;
    uint8_t fps;
    uint32_t bitrateBps;
    uint64_t timebaseUs;
};

struct PingPong {
    uint32_t pingId;
    uint64_t sendTimeUs;
};

struct Feedback {
    uint16_t lostFrames;
    uint8_t lossPct;
    uint16_t rttMs;
    uint32_t recvBitrateKbps;
};

struct Reconfig {
    uint16_t width;
    uint16_t height;
    uint32_t bitrateBps;
};

// ---- Kênh input (GĐ4) ----
enum class InputType : uint8_t {
    Key = 1,         // a = mã phím ảo (VK), b = scancode (bit8 = phím mở rộng E0)
    MouseMove = 2,   // absolute=1: a/b = toạ độ chuẩn hoá 0..65535 trong client rect
                     // absolute=0: a/b = delta thô (dx/dy) từ Raw Input
    MouseButton = 3, // a = MouseButton, b = 0
    MouseWheel = 4,  // a = 0, b = delta (bội của 120), state bỏ qua
};

enum class MouseButton : uint8_t { Left = 1,
    Right = 2,
    Middle = 3,
    X1 = 4,
    X2 = 5 };

// b của Key: scancode ở 8 bit thấp, bit8 = cờ E0 (phím mở rộng: mũi tên, Ctrl phải...).
inline constexpr int32_t kScanExtended = 0x100;

struct InputEvent {
    InputType type = InputType::Key;
    uint64_t timestampUs = 0;
    int32_t a = 0;
    int32_t b = 0;
    uint8_t state = 0;    // 1 = nhấn/giữ, 0 = nhả. MouseMove/Wheel bỏ qua.
    uint8_t absolute = 0; // 1 = a/b là toạ độ tuyệt đối chuẩn hoá
};

// Event đổi TRẠNG THÁI: mất gói chứa nó gây kẹt phím → được gửi lặp (InputSender).
inline constexpr bool IsStateEvent(InputType t) {
    return t == InputType::Key || t == InputType::MouseButton;
}

struct VideoHeader {
    uint32_t frameId;
    uint64_t timestampUs;
    uint16_t pktIndex;
    uint16_t pktCount;
};

struct VideoPacketView {
    VideoHeader hdr;
    bool idr;
    bool frameEnd;
    std::span<const uint8_t> payload;
};

// Gói parity của một nhóm XEN KẼ. Với numGroups = ceil(pktCount/kFecGroupSize), nhóm
// `groupIndex` phủ các gói { i : i % numGroups == groupIndex } = groupIndex, groupIndex +
// numGroups, groupIndex + 2*numGroups, … — suy được từ pktCount, khỏi tốn byte trên wire.
// timestampUs/pktCount/idr đi kèm để dựng được cả frame chỉ có 1 gói (nhóm 1 phần tử:
// parity chính là bản sao gói đó).
struct FecHeader {
    uint32_t frameId;
    uint64_t timestampUs;
    uint16_t pktCount;
    uint8_t groupIndex;
};

struct FecPacketView {
    FecHeader hdr;
    bool idr;
    // kFecLenPrefix byte lenXor (big-endian) rồi tới dữ liệu XOR đã đệm 0.
    std::span<const uint8_t> parity;
};

// Một mảnh của một lần copy clipboard (GĐ8). `data` trỏ vào datagram gốc.
struct ClipboardChunkView {
    uint32_t updateId = 0;   // đổi theo mỗi lần copy — khoá ghép mảnh
    uint16_t chunkIndex = 0; // 0..chunkCount-1
    uint16_t chunkCount = 0;
    std::span<const uint8_t> data; // UTF-8, 1..kMaxClipboardChunk byte
};

// ---- Build: ghi trọn một datagram (header chung + payload) vào out.
// Trả về số byte đã ghi, hoặc 0 nếu out không đủ chỗ. ----
size_t BuildHello(std::span<uint8_t> out, const Hello& m);
size_t BuildHelloAck(std::span<uint8_t> out, const HelloAck& m);
size_t BuildStart(std::span<uint8_t> out, uint32_t sessionId);
size_t BuildListSources(std::span<uint8_t> out);
// Cắt bớt ở kMaxSources nguồn / kMaxSourceNameBytes byte tên để chắc chắn vừa 1 datagram.
size_t BuildSourceList(std::span<uint8_t> out, std::span<const SourceInfo> sources);
size_t BuildBye(std::span<uint8_t> out, uint32_t sessionId);
size_t BuildPing(std::span<uint8_t> out, uint32_t sessionId, const PingPong& m);
size_t BuildPong(std::span<uint8_t> out, uint32_t sessionId, const PingPong& m);
size_t BuildFeedback(std::span<uint8_t> out, uint32_t sessionId, const Feedback& m);
size_t BuildRequestKeyframe(std::span<uint8_t> out, uint32_t sessionId);
size_t BuildReconfig(std::span<uint8_t> out, uint32_t sessionId, const Reconfig& m);
// `focused` = 1: người dùng vừa chuyển sang xem/điều khiển nguồn này → host đưa cửa
// sổ nguồn lên foreground. = 0: rời đi → host nhả hết phím đang giữ của phiên này.
size_t BuildSetFocus(std::span<uint8_t> out, uint32_t sessionId, bool focused);
// NACK: xin gửi lại các mảnh `indices` (pktIndex) của `frameId`. Trả 0 nếu indices rỗng,
// quá kMaxNackIndices, hoặc out thiếu chỗ.
size_t BuildNack(std::span<uint8_t> out, uint32_t sessionId, uint32_t frameId,
    std::span<const uint16_t> indices);
// INVALIDATE_REF: client đã bỏ hẳn `frameId` → host đừng dùng nó làm khung tham chiếu.
size_t BuildInvalidateRef(std::span<uint8_t> out, uint32_t sessionId, uint32_t frameId);
size_t BuildVideoPacket(std::span<uint8_t> out, uint32_t sessionId, const VideoHeader& vh,
    bool idr, bool frameEnd, std::span<const uint8_t> payload);
// `parity` gồm cả 2 byte lenXor đứng đầu (xem FecPacketView).
size_t BuildFecPacket(std::span<uint8_t> out, uint32_t sessionId, const FecHeader& fh,
    bool idr, std::span<const uint8_t> parity);
// `firstSeq` là seq của events[0]; event thứ i mang seq = firstSeq + i (§6).
// Trả 0 nếu events rỗng, quá kMaxInputEvents, hoặc out thiếu chỗ.
size_t BuildInputEvents(std::span<uint8_t> out, uint32_t sessionId, uint32_t firstSeq,
    std::span<const InputEvent> events);
// CLIPBOARD: một mảnh của lần copy `updateId`. Trả 0 nếu data rỗng/quá
// kMaxClipboardChunk, chunkIndex/chunkCount vô nghĩa, hoặc out thiếu chỗ.
size_t BuildClipboardChunk(std::span<uint8_t> out, uint32_t sessionId, uint32_t updateId,
    uint16_t chunkIndex, uint16_t chunkCount, std::span<const uint8_t> data);

// ---- Parse. Trả về nullopt nếu gói ngắn/sai phiên bản. ----
std::optional<CommonHeader> ParseCommonHeader(std::span<const uint8_t> datagram);
// Phần payload sau header chung (rỗng nếu datagram ngắn hơn header).
std::span<const uint8_t> PayloadOf(std::span<const uint8_t> datagram);

std::optional<Hello> ParseHello(std::span<const uint8_t> payload);
// Giải mã SOURCE_LIST vào `out` (đủ chỗ cho kMaxSources). Trả số nguồn đã ghi.
size_t ParseSourceList(std::span<const uint8_t> payload, std::span<SourceInfo> out);
std::optional<HelloAck> ParseHelloAck(std::span<const uint8_t> payload);
std::optional<PingPong> ParsePingPong(std::span<const uint8_t> payload);
std::optional<Feedback> ParseFeedback(std::span<const uint8_t> payload);
std::optional<Reconfig> ParseReconfig(std::span<const uint8_t> payload);
// true = xin focus, false = nhả. nullopt nếu payload rỗng.
std::optional<bool> ParseSetFocus(std::span<const uint8_t> payload);
// Giải mã NACK vào `out` (đủ chỗ cho kMaxNackIndices). Trả số chỉ số đã ghi và đặt
// `frameId`; 0 nếu gói hỏng/rỗng/không khớp count.
size_t ParseNack(std::span<const uint8_t> payload, uint32_t& frameId,
    std::span<uint16_t> out);
// Giải mã INVALIDATE_REF. nullopt nếu payload ngắn.
std::optional<uint32_t> ParseInvalidateRef(std::span<const uint8_t> payload);
std::optional<VideoPacketView> ParseVideoPacket(const CommonHeader& h,
    std::span<const uint8_t> payload);
std::optional<FecPacketView> ParseFecPacket(const CommonHeader& h,
    std::span<const uint8_t> payload);
// Giải mã batch input vào `out` (đủ chỗ cho kMaxInputEvents). Trả số event đã ghi
// và đặt `firstSeq`; 0 nếu gói hỏng/rỗng/không khớp count.
size_t ParseInputEvents(std::span<const uint8_t> payload, uint32_t& firstSeq,
    std::span<InputEvent> out);
// Giải mã một mảnh CLIPBOARD. nullopt nếu gói cụt/chỉ số vô nghĩa/mảnh quá khổ.
std::optional<ClipboardChunkView> ParseClipboardChunk(std::span<const uint8_t> payload);

} // namespace deskhub
