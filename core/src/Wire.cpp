#include "rgc/Wire.h"
#include "rgc/ByteOrder.h"

#include <cstring>

namespace rgc {

namespace {

// Ghi header chung; trả về tổng kích thước datagram, 0 nếu out thiếu chỗ.
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

size_t BuildPingPongImpl(std::span<uint8_t> out, MsgType type, uint32_t sessionId,
                         const PingPong& m) {
    constexpr size_t kPayload = 12;
    const size_t total = WriteCommon(out, type, 0, Chan::Control, sessionId, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU32(p, m.pingId);
    PutU64(p + 4, m.sendTimeUs);
    return total;
}

} // namespace

size_t BuildHello(std::span<uint8_t> out, const Hello& m) {
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

size_t BuildSourceList(std::span<uint8_t> out, std::span<const SourceInfo> sources) {
    const size_t n = sources.size() < kMaxSources ? sources.size() : kMaxSources;
    // Đếm trước để biết tổng kích thước: WriteCommon cần payloadSize ngay từ đầu.
    size_t payload = 1;
    for (size_t i = 0; i < n; ++i) {
        payload += 6 + Utf8TruncLen(sources[i].name, kMaxSourceNameBytes);
    }
    const size_t total = WriteCommon(out, MsgType::SourceList, 0, Chan::Control, 0, payload);
    if (!total) return 0;

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

size_t BuildHelloAck(std::span<uint8_t> out, const HelloAck& m) {
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

size_t BuildFeedback(std::span<uint8_t> out, uint32_t sessionId, const Feedback& m) {
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

size_t BuildReconfig(std::span<uint8_t> out, uint32_t sessionId, const Reconfig& m) {
    constexpr size_t kPayload = 8;
    const size_t total = WriteCommon(out, MsgType::Reconfig, 0, Chan::Control, sessionId, kPayload);
    if (!total) return 0;
    uint8_t* p = out.data() + kCommonHeaderSize;
    PutU16(p, m.width);
    PutU16(p + 2, m.height);
    PutU32(p + 4, m.bitrateBps);
    return total;
}

size_t BuildVideoPacket(std::span<uint8_t> out, uint32_t sessionId, const VideoHeader& vh,
                        bool idr, bool frameEnd, std::span<const uint8_t> payload) {
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

} // namespace rgc
