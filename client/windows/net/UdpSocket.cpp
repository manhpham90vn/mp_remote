// =============================================================================
// UdpSocket.cpp — cài đặt bằng winsock2 (Windows).
//
// BỐ CỤC
//   NetAddr::ToString / ParseNetAddr — chuyển đổi địa chỉ ↔ chuỗi, không đụng socket.
//   Open / SetRecvTimeout / SendTo / RecvFrom / Close — vòng đời và I/O.
//
// QUY ƯỚC XỬ LÝ LỖI XUYÊN SUỐT
//   Hàm trả bool: true = thành công. Riêng RecvFrom trả int với BA vùng ý nghĩa —
//   >0 là số byte nhận được, 0 là HẾT TIMEOUT (chuyện bình thường, không phải lỗi),
//   <0 là lỗi thật khiến người gọi phải dừng vòng lặp. Phân biệt được 0 với lỗi là
//   điều kiện để vòng lặp mạng vừa nghe gói vừa chạy Tick theo nhịp đều đặn.
//
// CẠM BẪY RIÊNG CỦA WINDOWS — SIO_UDP_CONNRESET
//   Đây là khác biệt lớn nhất so với bản Android, và là thứ dễ mất hàng giờ để lần
//   ra nếu chưa biết. Chi tiết ở ngay chỗ gọi WSAIoctl trong Open().
//
// VỀ THỨ TỰ BYTE
//   Đây là ranh giới duy nhất trong chương trình có htonl/ntohl. NetAddr luôn giữ
//   host byte order; mọi lần chạm vào sockaddr_in đều kèm một phép đổi. Nhầm chỗ
//   này cho ra lỗi rất khó thấy: địa chỉ vẫn "hợp lệ" nhưng trỏ sang máy khác hẳn.
//
// LIÊN QUAN: net/UdpSocket.h (API + lý do thiết kế),
//            client/android/.../net/UdpSocket.cpp (bản song song trên BSD socket)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "net/UdpSocket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>

#pragma comment(lib, "ws2_32.lib")

// Một số SDK cũ không khai báo hằng này. Tự định nghĩa theo đúng công thức của
// Microsoft để build được trên mọi phiên bản SDK.
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

std::string NetAddr::ToString() const {
    char b[32];
    std::snprintf(b, sizeof(b), "%u.%u.%u.%u:%u",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
    return b;
}

// "192.168.1.5" hoặc "192.168.1.5:47777" -> NetAddr. Người dùng gõ chuỗi này vào ô
// địa chỉ trên UI nên nó là dữ liệu không tin được: mọi đường sai đều trả false để
// tầng trên báo lỗi tử tế, không có đường nào cho ra địa chỉ rác trông như hợp lệ.
// Chỉ IPv4, không phân giải tên miền — chương trình này dùng trong mạng LAN.
bool ParseNetAddr(const std::string& s, uint16_t defaultPort, NetAddr& out) {
    std::string ipPart = s;
    uint16_t port = defaultPort;
    // Có dấu ':' thì phần sau là cổng; không có thì dùng cổng mặc định của caller.
    if (const size_t colon = s.find(':'); colon != std::string::npos) {
        ipPart = s.substr(0, colon);
        const int p = std::atoi(s.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = uint16_t(p);
    }
    // InetPtonA chứ không phải inet_addr: inet_addr trả về INADDR_NONE (0xFFFFFFFF)
    // khi lỗi, mà đó cũng là giá trị hợp lệ của 255.255.255.255 — không phân biệt
    // được. InetPtonA trả về mã lỗi riêng nên chặt chẽ hơn.
    IN_ADDR a{};
    if (InetPtonA(AF_INET, ipPart.c_str(), &a) != 1) return false;
    out.ip = ntohl(a.S_un.S_addr);
    out.port = port;
    return true;
}

// Thử bind từng cổng trong dải, đóng ngay khi thấy trống. Không tái dùng Open() vì
// Open() in log mỗi lần bind hỏng — dò cổng phải im lặng. WSAStartup đếm tham chiếu
// nên gọi tạm ở đây không ảnh hưởng các UdpSocket khác.
uint16_t FindFreeUdpPort(uint16_t start, int count) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
    uint16_t found = 0;
    for (int i = 0; i < count && !found; ++i) {
        const int p = int(start) + i;
        if (p <= 0 || p > 65535) break; // hết dải cổng hợp lệ
        const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(uint16_t(p));
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0) found = uint16_t(p);
        closesocket(s);
    }
    WSACleanup();
    return found;
}

UdpSocket::~UdpSocket() { Close(); }

// Mở socket UDP và bind. Ghi vào sock_ CHỈ KHI mọi bước đã thành công — thất bại
// giữa chừng thì đóng handle cục bộ và để đối tượng nguyên trạng "chưa mở".
//
// WSAStartup đếm tham chiếu ở cấp tiến trình, nên gọi nhiều lần từ nhiều UdpSocket
// là hợp lệ; mỗi lần phải có đúng một WSACleanup đối ứng, và cờ wsaInit_ bảo đảm
// điều đó ngay cả khi Open() thất bại giữa chừng.
bool UdpSocket::Open(uint16_t localPort) {
    lastBindAddrInUse_ = false; // reset: chỉ nói về lần Open này
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

    // CẠM BẪY RIÊNG CỦA WINDOWS. Nếu một lần sendto trước đó gây ra ICMP "port
    // unreachable" (host chưa chạy, chuyện thường trong lúc client đang thử kết
    // nối), Windows sẽ nhớ điều đó và làm recvfrom trả WSAECONNRESET MÃI MÃI — kể
    // cả khi host đã lên và đang gửi dữ liệu bình thường. Socket coi như chết.
    //
    // Đây là hành vi Windows tự thêm, trái với tinh thần UDP: một giao thức không
    // kết nối thì không nên có khái niệm "phía kia từ chối". Linux không làm vậy,
    // nên bản Android không cần đoạn này.
    //
    // Tắt bằng WSAIoctl. Không kiểm tra trị trả về vì trên các bản Windows rất cũ
    // ioctl này có thể không tồn tại — lúc đó RecvFrom vẫn nuốt WSAECONNRESET như
    // lớp phòng thủ thứ hai.
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);

    // Nới buffer nhận của kernel lên 4 MB. Ở bitrate cao, một khoảng ngừng ngắn của
    // thread Net (bị hệ điều hành cho ra rìa, hoặc kẹt ở một vòng xử lý dài) là đủ
    // để buffer mặc định tràn và mất gói THẬT — thứ mất mát mà FEC lẫn việc xin IDR
    // đều không cứu nổi vì nó xảy ra trước khi gói đến tay chương trình.
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

    // INADDR_ANY: nghe trên mọi giao diện mạng. Máy thường có nhiều đường ra cùng
    // lúc (Ethernet, Wi-Fi, vEthernet của Hyper-V/WSL) và ta không biết trước phía
    // kia sẽ tới từ nhánh nào — xem NetInfo.h về việc liệt kê chúng cho người dùng.
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(localPort);
    if (bind(s, (sockaddr*)&local, sizeof(local)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        // WSAEADDRINUSE là ca người dùng gặp thật: một host RemoteGame cũ còn chạy
        // nền (cửa sổ menu bị ẩn suốt phiên share) vẫn giữ cổng. Tách riêng để tầng
        // trên báo cách xử lý thay vì phơi số lỗi 10048.
        lastBindAddrInUse_ = (err == WSAEADDRINUSE);
        if (lastBindAddrInUse_)
            std::printf("[UDP] Port %u is already in use — another RemoteGame host (or "
                        "another program) is still listening on it.\n", localPort);
        else
            std::printf("[UDP] bind(:%u) failed: %d\n", localPort, err);
        closesocket(s);
        return false;
    }

    sock_ = uint64_t(s);
    return true;
}

// Đặt hạn chờ cho RecvFrom. Đây là thứ biến vòng lặp mạng từ "chặn vô hạn" thành
// "chặn tối đa N ms rồi trả 0" — nhờ vậy vòng lặp vẫn chạy Tick đều đặn (ping,
// timeout, phát lại) ngay cả khi phía kia im lặng hoàn toàn.
//
// Khác bản POSIX: winsock nhận DWORD mili-giây, không phải struct timeval.
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
    // Đòi gửi TRỌN VẸN: với SOCK_DGRAM thì sendto hoặc gửi cả datagram hoặc không
    // gửi gì, nên gửi thiếu byte nghĩa là có chuyện bất thường. Không thử gửi lại —
    // ở tầng này mất gói là bình thường, các tầng trên đã có cơ chế phát lại riêng.
    return sendto(SOCKET(sock_), (const char*)data, int(len), 0,
                  (sockaddr*)&sa, sizeof(sa)) == int(len);
}

// Nhận một datagram. Xem quy ước ba vùng giá trị trả về ở đầu file.
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
    // Ba mã lỗi này KHÔNG phải lỗi thật — quy về 0 để vòng lặp cứ chạy tiếp:
    //   WSAETIMEDOUT  — hết hạn SO_RCVTIMEO, đúng như thiết kế.
    //   WSAECONNRESET — vọng lại từ ICMP port-unreachable. Về lý thuyết đã bị tắt
    //                   bằng SIO_UDP_CONNRESET trong Open(), nhưng giữ ở đây làm
    //                   lớp phòng thủ thứ hai cho máy mà ioctl đó không có tác dụng.
    //   WSAEMSGSIZE   — datagram dài hơn `cap` nên bị cắt cụt. Người gọi luôn truyền
    //                   bộ đệm kMaxDatagram nên chỉ xảy ra khi ai đó gửi gói không
    //                   đúng giao thức; phần đã nhận được vẫn đưa lên và Wire.cpp
    //                   sẽ loại nó.
    const int err = WSAGetLastError();
    if (err == WSAETIMEDOUT || err == WSAECONNRESET || err == WSAEMSGSIZE) return 0;
    return -1;
}

// Đặt lại sock_ = ~0ull sau khi đóng, nên gọi Close() nhiều lần là vô hại
// (destructor cũng gọi nó). Đóng hai lần một handle là lỗi nặng: handle được cấp
// lại rất nhanh, lần đóng thứ hai có thể đóng nhầm socket mà phần khác vừa mở.
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
