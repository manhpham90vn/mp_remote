#pragma once
// HostSession — máy trạng thái phía Agent (host): IDLE → READY → STREAMING.
//
// Thuần C++20: không socket, không thread, không đồng hồ. Byte vào qua
// HandlePacket, byte ra qua callback `send`, thời gian bơm từ ngoài (`nowUs`).
// Caller (AgentLoop) sở hữu socket và địa chỉ peer; HandlePacket trả true khi
// gói hợp lệ thuộc phiên → caller cập nhật peer theo địa chỉ nguồn (roaming §1.5).
//
// Thread-model: HandlePacket/Tick chạy trên MỘT thread (Recv). state()/sessionId()
// đọc được từ thread khác (thread encode hỏi "đã STREAMING chưa?") — atomic.
#include "rgc/Wire.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>

namespace rgc {

inline constexpr uint64_t kSessionTimeoutUs = 5'000'000; // 5s không gói → mất peer

struct StreamParams {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t  fps = 60;
    uint32_t bitrateBps = 20'000'000;
};

struct HostCallbacks {
    std::function<void(std::span<const uint8_t>)> send; // giao datagram cho tầng socket
    std::function<void()> onStart;           // nhận START → force IDR, bắt đầu đẩy video
    std::function<void()> onKeyframeRequest; // REQUEST_KEYFRAME từ client
    std::function<void()> onDisconnect;      // BYE hoặc timeout → đã quay về IDLE
};

class HostSession {
public:
    enum class State : uint8_t { Idle, Ready, Streaming };

    HostSession(HostCallbacks cb, StreamParams offer)
        : cb_(std::move(cb)), offer_(offer) {}

    // Trả true nếu gói hợp lệ và thuộc phiên hiện tại (caller cập nhật peer addr).
    bool HandlePacket(std::span<const uint8_t> pkt, uint64_t nowUs);
    void Tick(uint64_t nowUs);

    State    state() const { return state_.load(std::memory_order_acquire); }
    uint32_t sessionId() const { return sessionId_.load(std::memory_order_relaxed); }

private:
    void SendHelloAck(uint64_t nowUs);
    void SendReject();
    void Disconnect();

    HostCallbacks cb_;
    StreamParams  offer_;
    std::atomic<State>    state_{State::Idle};
    std::atomic<uint32_t> sessionId_{0};
    uint32_t clientId_ = 0;
    uint64_t lastRecvUs_ = 0;
    uint8_t  buf_[kMaxDatagram] = {}; // chỉ dùng trên thread Recv
};

} // namespace rgc
