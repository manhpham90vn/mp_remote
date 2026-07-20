#pragma once
//
// UdpSocket — bản Android (BSD sockets) của lớp mỏng platform-specific ở docs/06 §1.3.
// API GIỮ NGUYÊN như client/windows/UdpSocket.h để phần logic port sang đọc y hệt;
// core (rgc) vẫn không biết đến lớp này.
//
#include <cstdint>
#include <string>

// Địa chỉ IPv4 dạng host byte order — POD, copy rẻ.
struct NetAddr {
    uint32_t ip = 0;   // host byte order (127.0.0.1 = 0x7F000001)
    uint16_t port = 0;

    bool operator==(const NetAddr&) const = default;
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
    bool Open(uint16_t localPort);

    // Timeout cho RecvFrom (ms). 0 = blocking vô hạn.
    bool SetRecvTimeout(uint32_t ms);

    bool SendTo(const NetAddr& to, const uint8_t* data, size_t len);

    // >0: số byte nhận; 0: timeout; <0: lỗi thật sự.
    int RecvFrom(uint8_t* buf, size_t cap, NetAddr& from);

    void Close();
    bool IsOpen() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};
