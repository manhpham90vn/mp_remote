// =============================================================================
// Wire.cpp — cài đặt các hàm dựng/giải mã datagram khai báo ở Wire.h.
//
// NHIỆM VỤ
//   Chuyển qua lại giữa struct trong bộ nhớ và chuỗi byte trên đường truyền.
//   Mỗi hàm Build* có đúng một hàm Parse* đối xứng; hai bên PHẢI khớp nhau về
//   thứ tự và độ rộng từng trường, nên chúng được đặt cạnh nhau theo cặp và mọi
//   thay đổi phải sửa cả hai cùng lúc (CoreTests.cpp có test khứ hồi cho từng cặp).
//
// BỐ CỤC FILE
//   1. namespace vô danh — tiện ích nội bộ: WriteCommon, BuildEmpty,
//      BuildPingPongImpl, Utf8TruncLen.
//   2. Các hàm Build*  — dựng gói, theo thứ tự: bắt tay → điều khiển → video/FEC → input.
//   3. Các hàm Parse*  — giải gói, cùng thứ tự.
//
// NGUYÊN TẮC AN TOÀN XUYÊN SUỐT
//   - Build*: WriteCommon kiểm tra sức chứa của `out` MỘT LẦN cho cả header lẫn
//     payload rồi trả về tổng kích thước. Sau khi nó trả về khác 0, mọi lệnh ghi
//     phía dưới đã được bảo đảm nằm trong biên — nên chúng không kiểm tra lại nữa.
//   - Parse*: kiểm tra `payload.size()` trước MỌI lần đọc. Dữ liệu đến từ mạng là
//     dữ liệu không tin được: một datagram cụt hoặc bị dựng ác ý không được phép
//     làm đọc ngoài biên. Đây là ranh giới tin cậy của toàn bộ chương trình.
//   - Không hàm nào ở đây giữ trạng thái giữa các lần gọi.
//
// LIÊN QUAN: deskhub/wire/Wire.h (khai báo + giải thích từng thông điệp),
//            deskhub/wire/ByteOrder.h (PutU16/GetU32/...), docs/04-protocol.md
// =============================================================================
#include "deskhub/wire/Wire.h"
#include "deskhub/wire/ByteOrder.h"

#include <cstring>

namespace deskhub {

namespace {

// Ghi header chung; trả về tổng kích thước datagram, 0 nếu out thiếu chỗ.
//
// Đây là CỬA KIỂM TRA BIÊN DUY NHẤT của mọi hàm Build*: người gọi truyền vào kích
// thước payload nó sắp ghi, hàm này đối chiếu với sức chứa thật của `out`. Nhờ vậy
// các hàm Build* phía dưới ghi thẳng bằng con trỏ mà không phải kiểm tra từng lần —
// đổi lại, người gọi BẮT BUỘC phải truyền payloadSize đúng bằng số byte nó sẽ ghi.
size_t WriteCommon(std::span<uint8_t> out, MsgType type, uint8_t flags, Chan chan,
                   uint32_t sessionId, size_t payloadSize) {
    const size_t total = kCommonHeaderSize + payloadSize;
    if (out.size() < total) return 0;
    out[0] = kProtocolVersion;
    out[1] = uint8_t(type);
    out[2] = flags;
    out[3] = uint8_t(chan);
    PutU32(out.data() + 4, sessionId);
    return total;
}

// Thông điệp không có payload — chỉ cần header chung là đủ mang hết ý nghĩa
// (START, BYE, LIST_SOURCES, REQUEST_KEYFRAME). Loại thông điệp nằm ở byte type.
size_t BuildEmpty(std::span<uint8_t> out, MsgType type, uint32_t sessionId) {
    return WriteCommon(out, type, 0, Chan::Control, sessionId, 0);
}

// Cắt tên nguồn về ≤ limit byte NHƯNG lùi tới ranh giới ký tự UTF-8 — cắt giữa một
// ký tự nhiều byte sẽ hiện ra ô vuông ở danh sách nguồn phía client.
size_t Utf8TruncLen(const std::string& s, size_t limit) {
    if (s.size() <= limit) return s.size();
    size_t n = limit;
    while (n > 0 && (uint8_t(s[n]) & 0xC0) == 0x80) --n; // lùi qua byte nối 10xxxxxx
    return n;
}

// PING và PONG có payload y hệt nhau, chỉ khác byte type — PONG là bản dội lại
// nguyên văn payload của PING. Nhờ giữ nguyên sendTimeUs (đồng hồ CLIENT) mà client
// tính được RTT chỉ bằng một phép trừ, không cần bảng tra pingId → thời điểm gửi,
// và hai đồng hồ không cần đồng bộ với nhau.
size_t BuildPingPongImpl(std::span<uint8_t> out, MsgType type, uint32_t sessionId,
                         const PingPong& m) {
    constexpr size_t kPayload = 12; // pingId(4) + sendTimeUs(8)
    const size_t total = WriteCommon(out, type, 0, Chan::Control, sessionId, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, m.pingId);
    PutU64(p + 4, m.sendTimeUs);
    return total;
}

} // namespace

// HELLO: client tự giới thiệu và nêu khả năng của mình. sessionId = 0 vì phiên
// chưa tồn tại — chính HELLO_ACK mới cấp số phiên.
size_t BuildHello(std::span<uint8_t> out, const Hello& m) {
    // clientId(4) codecMask(2) maxW(2) maxH(2) fps(1) features(2) sourceId(1)
    constexpr size_t kPayload = 14;
    const size_t total = WriteCommon(out, MsgType::Hello, 0, Chan::Control, 0, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, m.clientId);
    PutU16(p + 4, m.codecMask);
    PutU16(p + 6, m.maxWidth);
    PutU16(p + 8, m.maxHeight);
    p[10] = m.desiredFps;
    PutU16(p + 11, m.features);
    p[13] = m.sourceId;
    return total;
}

size_t BuildListSources(std::span<uint8_t> out) {
    return BuildEmpty(out, MsgType::ListSources, 0);
}

// SOURCE_LIST: host liệt kê các cửa sổ đang chia sẻ. Đây là thông điệp DUY NHẤT có
// payload dài thay đổi (tên cửa sổ dài ngắn khác nhau), nên nó phải đếm hai lượt:
// lượt một tính tổng kích thước để WriteCommon kiểm tra biên, lượt hai mới ghi thật.
//
// Định dạng: count(1) rồi count bản ghi
//            [ sourceId(1) width(2) height(2) nameLen(1) name(nameLen) ].
// sessionId = 0: client hỏi danh sách TRƯỚC khi có phiên (nó cần danh sách để chọn
// nguồn rồi mới gửi HELLO kèm sourceId).
size_t BuildSourceList(std::span<uint8_t> out, std::span<const SourceInfo> sources) {
    const size_t n = sources.size() < kMaxSources ? sources.size() : kMaxSources;
    // Đếm trước để biết tổng kích thước: WriteCommon cần payloadSize ngay từ đầu.
    size_t payload = 1;
    for (size_t i = 0; i < n; ++i) {
        payload += 6 + Utf8TruncLen(sources[i].name, kMaxSourceNameBytes);
    }
    const size_t total = WriteCommon(out, MsgType::SourceList, 0, Chan::Control, 0, payload);
    if (!total) return 0;

    // Lượt hai: ghi thật. `p` chạy tiến dần vì bản ghi có độ dài thay đổi, không
    // tính được offset cố định như các thông điệp khác.
    uint8_t* p = out.data() + kCommonHeaderSize;
    *p++ = uint8_t(n);
    for (size_t i = 0; i < n; ++i) {
        const SourceInfo& s = sources[i];
        const size_t nameLen = Utf8TruncLen(s.name, kMaxSourceNameBytes);
        *p++ = s.sourceId;
        PutU16(p, s.width);   p += 2;
        PutU16(p, s.height);  p += 2;
        *p++ = uint8_t(nameLen);
        if (nameLen) std::memcpy(p, s.name.data(), nameLen);
        p += nameLen;
    }
    return total;
}

// HELLO_ACK: host chốt tham số phiên (hoặc từ chối bằng codec = Rejected).
// sessionId nằm trong PAYLOAD chứ không phải header, vì lúc gửi gói này client
// chưa biết số phiên nên không thể đối chiếu trường header.
size_t BuildHelloAck(std::span<uint8_t> out, const HelloAck& m) {
    // sessionId(4) codec(1) w(2) h(2) fps(1) bitrate(4) timebaseUs(8)
    constexpr size_t kPayload = 22;
    const size_t total = WriteCommon(out, MsgType::HelloAck, 0, Chan::Control, 0, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, m.sessionId);
    p[4] = uint8_t(m.codec);
    PutU16(p + 5, m.width);
    PutU16(p + 7, m.height);
    p[9] = m.fps;
    PutU32(p + 10, m.bitrateBps);
    PutU64(p + 14, m.timebaseUs);
    return total;
}

size_t BuildStart(std::span<uint8_t> out, uint32_t sessionId) {
    return BuildEmpty(out, MsgType::Start, sessionId);
}

size_t BuildBye(std::span<uint8_t> out, uint32_t sessionId) {
    return BuildEmpty(out, MsgType::Bye, sessionId);
}

size_t BuildPing(std::span<uint8_t> out, uint32_t sessionId, const PingPong& m) {
    return BuildPingPongImpl(out, MsgType::Ping, sessionId, m);
}

size_t BuildPong(std::span<uint8_t> out, uint32_t sessionId, const PingPong& m) {
    return BuildPingPongImpl(out, MsgType::Pong, sessionId, m);
}

// FEEDBACK: báo cáo chất lượng đường truyền của cửa sổ 1 giây vừa qua, client gửi
// ngược cho host. Đầu vào của BitrateController.
size_t BuildFeedback(std::span<uint8_t> out, uint32_t sessionId, const Feedback& m) {
    // lostFrames(2) lossPct(1) rttMs(2) recvBitrateKbps(4)
    constexpr size_t kPayload = 9;
    const size_t total = WriteCommon(out, MsgType::Feedback, 0, Chan::Control, sessionId, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU16(p, m.lostFrames);
    p[2] = m.lossPct;
    PutU16(p + 3, m.rttMs);
    PutU32(p + 5, m.recvBitrateKbps);
    return total;
}

size_t BuildRequestKeyframe(std::span<uint8_t> out, uint32_t sessionId) {
    return BuildEmpty(out, MsgType::RequestKeyframe, sessionId);
}

size_t BuildSetFocus(std::span<uint8_t> out, uint32_t sessionId, bool focused) {
    const size_t total = WriteCommon(out, MsgType::SetFocus, 0, Chan::Control, sessionId, 1);
    if (!total) return 0;
    out[kCommonHeaderSize] = focused ? 1 : 0;
    return total;
}

// RECONFIG: host báo đổi kích thước nguồn hoặc bitrate GIỮA phiên, client không
// phải bắt tay lại. Host gửi kèm một IDR ngay sau đó để decoder bám được.
size_t BuildReconfig(std::span<uint8_t> out, uint32_t sessionId, const Reconfig& m) {
    constexpr size_t kPayload = 8; // w(2) h(2) bitrate(4)
    const size_t total = WriteCommon(out, MsgType::Reconfig, 0, Chan::Control, sessionId, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU16(p, m.width);
    PutU16(p + 2, m.height);
    PutU32(p + 4, m.bitrateBps);
    return total;
}

// VIDEO_PACKET: một mảnh của frame đã mã hoá. Đi trên kênh Video.
// Hai cờ nằm ở byte `flags` của header chung chứ không tốn byte payload: `idr` cho
// Reassembler biết có thể bắt đầu giải mã từ frame này, `frameEnd` đánh dấu mảnh cuối.
size_t BuildVideoPacket(std::span<uint8_t> out, uint32_t sessionId, const VideoHeader& vh,
                        bool idr, bool frameEnd, std::span<const uint8_t> payload) {
    // Chặn ở đây thay vì để WriteCommon phát hiện: vượt ngưỡng này nghĩa là
    // Packetizer cắt sai, và gói vượt MTU sẽ bị phân mảnh IP rồi mất cả cụm.
    if (payload.size() > kMaxVideoPayload) return 0;
    const uint8_t flags = (idr ? kVideoFlagIdr : 0) | (frameEnd ? kVideoFlagFrameEnd : 0);
    const size_t total = WriteCommon(out, MsgType::VideoPacket, flags, Chan::Video, sessionId,
                                     kVideoHeaderSize + payload.size());
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, vh.frameId);
    PutU64(p + 4, vh.timestampUs);
    PutU16(p + 12, vh.pktIndex);
    PutU16(p + 14, vh.pktCount);
    if (!payload.empty())
        std::memcpy(p + kVideoHeaderSize, payload.data(), payload.size());
    return total;
}

size_t BuildFecPacket(std::span<uint8_t> out, uint32_t sessionId, const FecHeader& fh,
                      bool idr, std::span<const uint8_t> parity) {
    if (parity.size() < kFecLenPrefix ||
        parity.size() > kFecLenPrefix + kMaxVideoPayload) return 0;
    const uint8_t flags = idr ? kVideoFlagIdr : 0;
    const size_t total = WriteCommon(out, MsgType::FecPacket, flags, Chan::Video, sessionId,
                                     kFecHeaderSize + parity.size());
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, fh.frameId);
    PutU64(p + 4, fh.timestampUs);
    PutU16(p + 12, fh.pktCount);
    p[14] = fh.groupIndex;
    p[15] = 0; // dự trữ
    std::memcpy(p + kFecHeaderSize, parity.data(), parity.size());
    return total;
}

// INPUT_EVENT: một LÔ event bàn phím/chuột. Gộp nhiều event vào một datagram thay
// vì mỗi event một gói, vì header 8 byte cho một event 19 byte là quá phí, và chuột
// di chuyển sinh event dày đặc (hàng trăm mỗi giây).
//
// Chỉ seq của event ĐẦU TIÊN được ghi; event thứ i mang seq = firstSeq + i, suy ra
// được nên khỏi tốn 4 byte mỗi event. InputReceiver dựa vào đúng quy ước này để
// khử trùng khi InputSender gửi lặp.
size_t BuildInputEvents(std::span<uint8_t> out, uint32_t sessionId, uint32_t firstSeq,
                        std::span<const InputEvent> events) {
    if (events.empty() || events.size() > kMaxInputEvents) return 0;
    const size_t payloadSize = kInputHeaderSize + events.size() * kInputEventSize;
    const size_t total = WriteCommon(out, MsgType::InputEvent, 0, Chan::Input, sessionId,
                                     payloadSize);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, firstSeq);
    p[4] = uint8_t(events.size());
    uint8_t* e = p + kInputHeaderSize;
    for (const auto& ev : events) {
        e[0] = uint8_t(ev.type);
        PutU64(e + 1, ev.timestampUs);
        PutU32(e + 9, uint32_t(ev.a));  // i32 gửi dưới dạng bit-pattern u32
        PutU32(e + 13, uint32_t(ev.b));
        e[17] = ev.state;
        e[18] = ev.absolute;
        e += kInputEventSize;
    }
    return total;
}

// ---------------------------------------------------------------------------
// PHẦN GIẢI MÃ. Từ đây trở xuống, dữ liệu vào ĐẾN TỪ MẠNG và không được tin.
// Mọi hàm kiểm tra độ dài trước khi đọc; gói không hợp lệ trả nullopt/0 chứ không
// bao giờ ném ngoại lệ hay đọc ngoài biên.
// ---------------------------------------------------------------------------

// Bước đầu tiên cho MỌI datagram nhận được. Lọc luôn gói sai phiên bản giao thức
// ở đây, vì diễn giải payload của một phiên bản khác sẽ cho kết quả rác.
// Không kiểm tra `type`/`chan` có nằm trong enum không — người gọi dùng switch và
// tự bỏ qua nhánh default, cách đó chịu được việc phiên bản sau thêm loại mới.
std::optional<CommonHeader> ParseCommonHeader(std::span<const uint8_t> datagram) {
    if (datagram.size() < kCommonHeaderSize) return std::nullopt;
    if (datagram[0] != kProtocolVersion) return std::nullopt;
    CommonHeader h;
    h.ver       = datagram[0];
    h.type      = MsgType(datagram[1]);
    h.flags     = datagram[2];
    h.chan      = Chan(datagram[3]);
    h.sessionId = GetU32(datagram.data() + 4);
    return h;
}

std::span<const uint8_t> PayloadOf(std::span<const uint8_t> datagram) {
    if (datagram.size() < kCommonHeaderSize) return {};
    return datagram.subspan(kCommonHeaderSize);
}

// Chấp nhận 13 byte (bản trước GĐ6) lẫn 14 byte — xem ghi chú về sourceId bên dưới.
// Đây là lý do dùng `<` chứ không phải `!=`: gói DÀI hơn dự kiến cũng nhận, để
// phiên bản sau thêm trường vào cuối mà không phá client cũ.
std::optional<Hello> ParseHello(std::span<const uint8_t> payload) {
    if (payload.size() < 13) return std::nullopt;
    const uint8_t* p = payload.data();
    Hello m;
    m.clientId   = GetU32(p);
    m.codecMask  = GetU16(p + 4);
    m.maxWidth   = GetU16(p + 6);
    m.maxHeight  = GetU16(p + 8);
    m.desiredFps = p[10];
    m.features   = GetU16(p + 11);
    // sourceId thêm ở GĐ6; gói 13 byte của bản cũ vẫn đọc được, hiểu là nguồn 0.
    m.sourceId   = payload.size() >= 14 ? p[13] : 0;
    return m;
}

// Đối xứng với BuildSourceList. Trường `count` ở đầu gói do BÊN KIA khai báo, nên
// nó là con số không tin được: kẹp về sức chứa của `out` trước, rồi vẫn kiểm tra
// biên ở từng bản ghi — một gói khai count=200 với payload 10 byte không được phép
// làm gì hơn là trả về danh sách rỗng.
size_t ParseSourceList(std::span<const uint8_t> payload, std::span<SourceInfo> out) {
    if (payload.empty()) return 0;
    size_t count = payload[0];
    if (count > out.size()) count = out.size();

    size_t off = 1;
    size_t written = 0;
    for (size_t i = 0; i < count; ++i) {
        if (off + 6 > payload.size()) break; // gói cụt — trả về những gì đọc được
        const uint8_t* p = payload.data() + off;
        const size_t nameLen = p[5];
        if (off + 6 + nameLen > payload.size()) break;
        SourceInfo s;
        s.sourceId = p[0];
        s.width    = GetU16(p + 1);
        s.height   = GetU16(p + 3);
        s.name.assign(reinterpret_cast<const char*>(p + 6), nameLen);
        out[written++] = std::move(s);
        off += 6 + nameLen;
    }
    return written;
}

std::optional<HelloAck> ParseHelloAck(std::span<const uint8_t> payload) {
    if (payload.size() < 22) return std::nullopt;
    const uint8_t* p = payload.data();
    HelloAck m;
    m.sessionId  = GetU32(p);
    m.codec      = Codec(p[4]);
    m.width      = GetU16(p + 5);
    m.height     = GetU16(p + 7);
    m.fps        = p[9];
    m.bitrateBps = GetU32(p + 10);
    m.timebaseUs = GetU64(p + 14);
    return m;
}

std::optional<PingPong> ParsePingPong(std::span<const uint8_t> payload) {
    if (payload.size() < 12) return std::nullopt;
    const uint8_t* p = payload.data();
    return PingPong{GetU32(p), GetU64(p + 4)};
}

std::optional<Feedback> ParseFeedback(std::span<const uint8_t> payload) {
    if (payload.size() < 9) return std::nullopt;
    const uint8_t* p = payload.data();
    Feedback m;
    m.lostFrames      = GetU16(p);
    m.lossPct         = p[2];
    m.rttMs           = GetU16(p + 3);
    m.recvBitrateKbps = GetU32(p + 5);
    return m;
}

std::optional<Reconfig> ParseReconfig(std::span<const uint8_t> payload) {
    if (payload.size() < 8) return std::nullopt;
    const uint8_t* p = payload.data();
    return Reconfig{GetU16(p), GetU16(p + 2), GetU32(p + 4)};
}

std::optional<bool> ParseSetFocus(std::span<const uint8_t> payload) {
    if (payload.empty()) return std::nullopt;
    return payload[0] != 0;
}

std::optional<VideoPacketView> ParseVideoPacket(const CommonHeader& h,
                                                std::span<const uint8_t> payload) {
    if (payload.size() < kVideoHeaderSize) return std::nullopt;
    const uint8_t* p = payload.data();
    VideoPacketView v;
    v.hdr.frameId     = GetU32(p);
    v.hdr.timestampUs = GetU64(p + 4);
    v.hdr.pktIndex    = GetU16(p + 12);
    v.hdr.pktCount    = GetU16(p + 14);
    v.idr      = (h.flags & kVideoFlagIdr) != 0;
    v.frameEnd = (h.flags & kVideoFlagFrameEnd) != 0;
    v.payload  = payload.subspan(kVideoHeaderSize);
    if (v.hdr.pktCount == 0 || v.hdr.pktIndex >= v.hdr.pktCount) return std::nullopt;
    return v;
}

std::optional<FecPacketView> ParseFecPacket(const CommonHeader& h,
                                            std::span<const uint8_t> payload) {
    if (payload.size() < kFecHeaderSize + kFecLenPrefix) return std::nullopt;
    const uint8_t* p = payload.data();
    FecPacketView v;
    v.hdr.frameId     = GetU32(p);
    v.hdr.timestampUs = GetU64(p + 4);
    v.hdr.pktCount    = GetU16(p + 12);
    v.hdr.groupIndex  = p[14];
    v.idr    = (h.flags & kVideoFlagIdr) != 0;
    v.parity = payload.subspan(kFecHeaderSize);
    if (v.hdr.pktCount == 0) return std::nullopt;
    // Nhóm phải nằm trong frame, không thì parity không phủ gói nào.
    if (size_t(v.hdr.groupIndex) * kFecGroupSize >= v.hdr.pktCount) return std::nullopt;
    return v;
}

size_t ParseInputEvents(std::span<const uint8_t> payload, uint32_t& firstSeq,
                        std::span<InputEvent> out) {
    if (payload.size() < kInputHeaderSize) return 0;
    const uint8_t* p = payload.data();
    const size_t count = p[4];
    if (count == 0 || count > out.size()) return 0;
    // Bắt buộc: `count` do bên gửi khai, phải đối chiếu với độ dài THẬT của payload
    // trước khi lặp. Thiếu dòng này, một gói khai 62 event nhưng chỉ mang 1 sẽ đọc
    // tràn hơn 1KB ngoài bộ đệm nhận.
    if (payload.size() < kInputHeaderSize + count * kInputEventSize) return 0;
    firstSeq = GetU32(p);
    const uint8_t* e = p + kInputHeaderSize;
    for (size_t i = 0; i < count; ++i) {
        InputEvent ev;
        ev.type        = InputType(e[0]);
        ev.timestampUs = GetU64(e + 1);
        ev.a           = int32_t(GetU32(e + 9));
        ev.b           = int32_t(GetU32(e + 13));
        ev.state       = e[17];
        ev.absolute    = e[18];
        out[i] = ev;
        e += kInputEventSize;
    }
    return count;
}

} // namespace deskhub
