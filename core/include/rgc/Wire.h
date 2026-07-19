#pragma once
// Wire format protocol v1 — đặc tả gốc: docs/04-protocol.md (nguồn chân lý).
// Thuần C++20, không header hệ điều hành — dùng chung cho Agent và mọi client
// (Windows/macOS/Ubuntu/iOS/Android). Mọi trường số trên wire là big-endian.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace rgc {

inline constexpr uint8_t kProtocolVersion  = 1;
inline constexpr size_t  kMaxDatagram      = 1200; // an toàn MTU Internet
inline constexpr size_t  kCommonHeaderSize = 8;
inline constexpr size_t  kVideoHeaderSize  = 16;
inline constexpr size_t  kMaxVideoPayload  = kMaxDatagram - kCommonHeaderSize - kVideoHeaderSize; // 1176

enum class Chan : uint8_t { Control = 0, Video = 1, Input = 2, Audio = 3 };

enum class MsgType : uint8_t {
    Hello           = 0x01,
    HelloAck        = 0x02,
    Start           = 0x03,
    Bye             = 0x04,
    VideoPacket     = 0x10,
    InputEvent      = 0x20, // GĐ4
    Ping            = 0x30,
    Pong            = 0x31,
    Feedback        = 0x32,
    RequestKeyframe = 0x33,
    Reconfig        = 0x34,
};

// Flags của VIDEO_PACKET
inline constexpr uint8_t kVideoFlagIdr      = 1u << 0;
inline constexpr uint8_t kVideoFlagFrameEnd = 1u << 1;

// codecMask trong HELLO / codec trong HELLO_ACK
inline constexpr uint16_t kCodecMaskH264 = 1u << 0;
inline constexpr uint16_t kCodecMaskHevc = 1u << 1;
inline constexpr uint16_t kCodecMaskAv1  = 1u << 2;
enum class Codec : uint8_t { H264 = 0, Hevc = 1, Av1 = 2, Rejected = 0xFF };

struct CommonHeader {
    uint8_t  ver;
    MsgType  type;
    uint8_t  flags;
    Chan     chan;
    uint32_t sessionId; // 0 trong HELLO/HELLO_ACK; Agent cấp trong HELLO_ACK
};

struct Hello {
    uint32_t clientId;
    uint16_t codecMask;
    uint16_t maxWidth;
    uint16_t maxHeight;
    uint8_t  desiredFps;
    uint16_t features;
};

struct HelloAck {
    uint32_t sessionId;
    Codec    codec; // Rejected (0xFF) = từ chối
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    uint32_t bitrateBps;
    uint64_t timebaseUs;
};

struct PingPong {
    uint32_t pingId;
    uint64_t sendTimeUs;
};

struct Feedback {
    uint16_t lostFrames;
    uint8_t  lossPct;
    uint16_t rttMs;
    uint32_t recvBitrateKbps;
};

struct Reconfig {
    uint16_t width;
    uint16_t height;
    uint32_t bitrateBps;
};

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

// ---- Build: ghi trọn một datagram (header chung + payload) vào out.
// Trả về số byte đã ghi, hoặc 0 nếu out không đủ chỗ. ----
size_t BuildHello(std::span<uint8_t> out, const Hello& m);
size_t BuildHelloAck(std::span<uint8_t> out, const HelloAck& m);
size_t BuildStart(std::span<uint8_t> out, uint32_t sessionId);
size_t BuildBye(std::span<uint8_t> out, uint32_t sessionId);
size_t BuildPing(std::span<uint8_t> out, uint32_t sessionId, const PingPong& m);
size_t BuildPong(std::span<uint8_t> out, uint32_t sessionId, const PingPong& m);
size_t BuildFeedback(std::span<uint8_t> out, uint32_t sessionId, const Feedback& m);
size_t BuildRequestKeyframe(std::span<uint8_t> out, uint32_t sessionId);
size_t BuildReconfig(std::span<uint8_t> out, uint32_t sessionId, const Reconfig& m);
size_t BuildVideoPacket(std::span<uint8_t> out, uint32_t sessionId, const VideoHeader& vh,
                        bool idr, bool frameEnd, std::span<const uint8_t> payload);

// ---- Parse. Trả về nullopt nếu gói ngắn/sai phiên bản. ----
std::optional<CommonHeader> ParseCommonHeader(std::span<const uint8_t> datagram);
// Phần payload sau header chung (rỗng nếu datagram ngắn hơn header).
std::span<const uint8_t> PayloadOf(std::span<const uint8_t> datagram);

std::optional<Hello>    ParseHello(std::span<const uint8_t> payload);
std::optional<HelloAck> ParseHelloAck(std::span<const uint8_t> payload);
std::optional<PingPong> ParsePingPong(std::span<const uint8_t> payload);
std::optional<Feedback> ParseFeedback(std::span<const uint8_t> payload);
std::optional<Reconfig> ParseReconfig(std::span<const uint8_t> payload);
std::optional<VideoPacketView> ParseVideoPacket(const CommonHeader& h,
                                                std::span<const uint8_t> payload);

} // namespace rgc
