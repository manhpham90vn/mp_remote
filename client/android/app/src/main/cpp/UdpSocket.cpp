#include "UdpSocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include "Log.h"

std::string NetAddr::ToString() const {
    char b[32];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u:%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
    return b;
}

bool ParseNetAddr(const std::string& s, uint16_t defaultPort, NetAddr& out) {
    std::string ipPart = s;
    uint16_t port = defaultPort;
    if (const size_t colon = s.find(':'); colon != std::string::npos) {
        ipPart = s.substr(0, colon);
        const int p = std::atoi(s.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = uint16_t(p);
    }
    in_addr a{};
    if (inet_pton(AF_INET, ipPart.c_str(), &a) != 1) return false;
    out.ip = ntohl(a.s_addr);
    out.port = port;
    return true;
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(uint16_t localPort) {
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        LOGE("[UDP] socket() failed: %d", errno);
        return false;
    }

    // Không cần tắt SIO_UDP_CONNRESET như Windows: socket UDP chưa connect() trên
    // Linux không nhận lỗi ICMP port-unreachable. RecvFrom vẫn nuốt ECONNREFUSED
    // cho chắc, phòng khi tầng dưới đẩy lên.

    // Đệm video bitrate cao: nới buffer nhận để không rớt gói khi thread bận decode.
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(localPort);
    if (bind(s, (sockaddr*)&local, sizeof(local)) != 0) {
        LOGE("[UDP] bind(:%u) failed: %d", localPort, errno);
        close(s);
        return false;
    }

    fd_ = s;
    return true;
}

bool UdpSocket::SetRecvTimeout(uint32_t ms) {
    if (!IsOpen()) return false;
    timeval tv{};
    tv.tv_sec = long(ms / 1000);
    tv.tv_usec = long((ms % 1000) * 1000);
    return setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool UdpSocket::SendTo(const NetAddr& to, const uint8_t* data, size_t len) {
    if (!IsOpen()) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(to.ip);
    sa.sin_port = htons(to.port);
    const ssize_t n = sendto(fd_, data, len, 0, (sockaddr*)&sa, sizeof(sa));
    return n == ssize_t(len);
}

int UdpSocket::RecvFrom(uint8_t* buf, size_t cap, NetAddr& from) {
    if (!IsOpen()) return -1;
    sockaddr_in sa{};
    socklen_t salen = sizeof(sa);
    const ssize_t n = recvfrom(fd_, buf, cap, 0, (sockaddr*)&sa, &salen);
    if (n >= 0) {
        from.ip = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        return int(n);
    }
    // EAGAIN/EWOULDBLOCK = hết timeout (không phải lỗi). EINTR = bị tín hiệu cắt.
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED)
        return 0;
    return -1;
}

void UdpSocket::Close() {
    if (IsOpen()) {
        close(fd_);
        fd_ = -1;
    }
}
