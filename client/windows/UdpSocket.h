#pragma once
//
// UdpSocket - lop mong platform-specific duy nhat cua GD3 (docs/06 §1.3).
// Windows: winsock2. Sau nay cac OS khac #ifdef sang BSD sockets - API giu nguyen.
// Core (rgc) KHONG biet den lop nay: byte vao/ra core qua callback/span.
//
#include <cstdint>
#include <string>

// Dia chi IPv4 d.o host byte order - POD de so sanh/copy re (roaming: peer doi addr).
struct NetAddr {
    uint32_t ip = 0;   // host byte order (127.0.0.1 = 0x7F000001)
    uint16_t port = 0;

    bool operator==(const NetAddr&) const = default;
    // Goi gon vao u64 de chia se giua 2 thread bang std::atomic (AgentLoop).
    uint64_t Pack() const { return (uint64_t(ip) << 16) | port; }
    static NetAddr Unpack(uint64_t v) { return NetAddr{uint32_t(v >> 16), uint16_t(v)}; }
    std::string ToString() const;
};

// "ip[:port]" -> NetAddr (port mac dinh neu khong ghi). false neu sai cu phap.
bool ParseNetAddr(const std::string& s, uint16_t defaultPort, NetAddr& out);

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Mo + bind. `localPort` = 0 -> he thong cap port ngau nhien (phia client).
    // Tu tat WSAECONNRESET (ICMP port unreachable lam recvfrom loi vinh vien).
    bool Open(uint16_t localPort);

    // Timeout cho RecvFrom (ms). 0 = blocking vo han.
    bool SetRecvTimeout(uint32_t ms);

    bool SendTo(const NetAddr& to, const uint8_t* data, size_t len);

    // >0: so byte nhan; 0: timeout; <0: loi that su.
    int RecvFrom(uint8_t* buf, size_t cap, NetAddr& from);

    void Close();
    bool IsOpen() const { return sock_ != ~0ull; }

private:
    uint64_t sock_ = ~0ull; // SOCKET (INVALID_SOCKET) - tranh keo winsock vao header
    bool wsaInit_ = false;
};
