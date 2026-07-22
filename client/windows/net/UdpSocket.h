#pragma once
// =============================================================================
// UdpSocket.h — bọc winsock2. Lớp mỏng platform-specific của GĐ3 (docs/06 §1.3).
//
// NHIỆM VỤ
//   Che khác biệt giữa API socket của các hệ điều hành sau MỘT API duy nhất, để
//   phần logic phía trên (AgentLoop, ClientLoop) đọc y hệt nhau ở mọi nền tảng.
//   API ở đây trùng từng chữ với client/android/.../net/UdpSocket.h — port code
//   qua lại chỉ là chép, không phải viết lại.
//
// VỊ TRÍ TRONG KIẾN TRÚC
//   core/ (rgc) tuyệt đối không biết đến lớp này: nó chỉ nhận/giao byte qua callback
//   `send` và hàm HandlePacket. Toàn bộ hiểu biết về socket của chương trình nằm ở
//   đây và ở người gọi trực tiếp nó.
//
// VÌ SAO sock_ LÀ uint64_t CHỨ KHÔNG PHẢI SOCKET
//   Khai báo kiểu SOCKET trong header này sẽ kéo winsock2.h vào mọi file include nó,
//   và winsock2.h xung khắc với windows.h nếu sai thứ tự (lỗi kinh điển: phải include
//   winsock2.h TRƯỚC windows.h, không thì hàng trăm lỗi định nghĩa lại). Giấu nó
//   sau uint64_t giữ header này sạch; ~0ull đóng vai INVALID_SOCKET.
//
// QUY ƯỚC ĐỊA CHỈ
//   NetAddr giữ IP ở HOST byte order chứ không phải network byte order. Việc đổi
//   thứ tự byte dồn hết vào ranh giới gọi API hệ thống (htonl/ntohl trong .cpp),
//   nên mọi chỗ khác trong app so sánh và in địa chỉ một cách tự nhiên.
//   Pack()/Unpack() ép NetAddr vào một u64 để hai thread chia sẻ nó qua std::atomic
//   mà không cần khoá — AgentLoop dùng để cập nhật địa chỉ peer khi client roaming.
//
// SỞ HỮU TÀI NGUYÊN
//   Lớp này sở hữu socket VÀ vòng đời WSAStartup/WSACleanup: destructor tự dọn, và
//   copy bị CẤM (hai đối tượng cùng giữ một handle thì cái nào hủy trước sẽ đóng
//   socket của cái kia). Truyền đi thì dùng tham chiếu.
//
// LIÊN QUAN: client/android/app/src/main/cpp/net/UdpSocket.h (bản song song),
//            AgentLoop.cpp, ClientLoop.cpp (người dùng), docs/06 §1.3
// =============================================================================
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

// Tìm cổng UDP TRỐNG đầu tiên trong [start, start+count) bằng cách thử bind rồi đóng
// ngay. Trả 0 nếu cả dải đều bận. Dùng để host chọn cổng lúc khởi động (ưu tiên
// 47777, kẹt thì +1 dần) mà không đụng tới host cũ đang chạy. Im lặng — không in log.
uint16_t FindFreeUdpPort(uint16_t start, int count);

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

    // Sau khi Open() trả false: true nếu nguyên nhân là CỔNG ĐÃ BỊ CHIẾM
    // (WSAEADDRINUSE) — trường hợp duy nhất người dùng xử lý được (đóng host cũ hoặc
    // đổi cổng). Che sau bool để không lộ hằng winsock ra header.
    bool lastBindAddrInUse() const { return lastBindAddrInUse_; }

private:
    uint64_t sock_ = ~0ull; // SOCKET (INVALID_SOCKET) - tránh kéo winsock vào header
    bool wsaInit_ = false;
    bool lastBindAddrInUse_ = false;
};
