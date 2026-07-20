#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "UdpSocket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

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
    IN_ADDR a{};
    if (InetPtonA(AF_INET, ipPart.c_str(), &a) != 1) return false;
    out.ip = ntohl(a.S_un.S_addr);
    out.port = port;
    return true;
}

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(uint16_t localPort) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("[UDP] WSAStartup failed.\n");
        return false;
    }
    wsaInit_ = true;

    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        std::printf("[UDP] socket() failed: %d\n", WSAGetLastError());
        return false;
    }

    // Windows: khi sendto trước đó gây ICMP "port unreachable", recvfrom sẽ trả
    // WSAECONNRESET mãi mãi. Tắt hành vi này - UDP đúng nghĩa là fire-and-forget.
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);

    // Đệm video bitrate cao: nới buffer nhận để không rớt gói khi thread bận decode.
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(localPort);
    if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        std::printf("[UDP] bind(:%u) failed: %d\n", localPort, WSAGetLastError());
        closesocket(s);
        return false;
    }

    sock_ = uint64_t(s);
    return true;
}

bool UdpSocket::SetRecvTimeout(uint32_t ms) {
    if (!IsOpen()) return false;
    DWORD t = ms;
    return setsockopt(SOCKET(sock_), SOL_SOCKET, SO_RCVTIMEO,
                      (const char*)&t, sizeof(t)) == 0;
}

bool UdpSocket::SendTo(const NetAddr& to, const uint8_t* data, size_t len) {
    if (!IsOpen()) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(to.ip);
    sa.sin_port = htons(to.port);
    return sendto(SOCKET(sock_), (const char*)data, int(len), 0,
                  (sockaddr*)&sa, sizeof(sa)) == int(len);
}

int UdpSocket::RecvFrom(uint8_t* buf, size_t cap, NetAddr& from) {
    if (!IsOpen()) return -1;
    sockaddr_in sa{};
    int salen = sizeof(sa);
    const int n = recvfrom(SOCKET(sock_), (char*)buf, int(cap), 0, (sockaddr*)&sa, &salen);
    if (n >= 0) {
        from.ip = ntohl(sa.sin_addr.s_addr);
        from.port = ntohs(sa.sin_port);
        return n;
    }
    const int err = WSAGetLastError();
    if (err == WSAETIMEDOUT || err == WSAECONNRESET || err == WSAEMSGSIZE) return 0;
    return -1;
}

void UdpSocket::Close() {
    if (IsOpen()) {
        closesocket(SOCKET(sock_));
        sock_ = ~0ull;
    }
    if (wsaInit_) {
        WSACleanup();
        wsaInit_ = false;
    }
}
