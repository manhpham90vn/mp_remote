#pragma once
//
// UdpSocket - lớp mỏng platform-specific duy nhất của GD3 (docs/06 §1.3).
// Windows: winsock2. Sau này các OS khác #ifdef sang BSD sockets - API giữ nguyên.
// Core (rgc) KHÔNG biết đến lớp này: byte vào/ra core qua callback/span.
//
#include <cstdint>
#include <string>

// Địa chỉ IPv4 dạng host byte order - POD để so sánh/copy rẻ (roaming: peer đổi addr).
struct NetAddr {
    uint32_t ip = 0;   // host byte order (127.0.0.1 = 0x7F000001)
    uint16_t port = 0;

    bool operator==(const NetAddr&) const = default;
    // Gói gọn vào u64 để chia sẻ giữa 2 thread bằng std::atomic (AgentLoop).
    uint64_t Pack() const { return (uint64_t(ip) << 16) | port; }
    static NetAddr Unpack(uint64_t v) { return NetAddr{uint32_t(v >> 16), uint16_t(v)}; }
    std::string ToString() const;
};

// "ip[:port]" -> NetAddr (port mặc định nếu không ghi). false nếu sai cú pháp.
bool ParseNetAddr(const std::string& s, uint16_t defaultPort, NetAddr& out);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Mở + bind. `localPort` = 0 -> hệ thống cấp port ngẫu nhiên (phía client).
    // Tự tắt WSAECONNRESET (ICMP port unreachable làm recvfrom lỗi vĩnh viễn).
    bool Open(uint16_t localPort);

    // Timeout cho RecvFrom (ms). 0 = blocking vô hạn.
    bool SetRecvTimeout(uint32_t ms);

    bool SendTo(const NetAddr& to, const uint8_t* data, size_t len);

    // >0: số byte nhận; 0: timeout; <0: lỗi thật sự.
    int RecvFrom(uint8_t* buf, size_t cap, NetAddr& from);

    void Close();
    bool IsOpen() const { return sock_ != ~0ull; }

private:
    uint64_t sock_ = ~0ull; // SOCKET (INVALID_SOCKET) - tránh kéo winsock vào header
    bool wsaInit_ = false;
};
