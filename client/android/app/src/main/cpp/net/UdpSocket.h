#pragma once
// =============================================================================
// UdpSocket.h — bọc BSD socket, bản Android. Lớp mỏng platform-specific (docs/06 §1.3).
//
// NHIỆM VỤ
//   Che khác biệt giữa API socket của các hệ điều hành sau MỘT API duy nhất, để
//   phần logic phía trên (ClientLoop, SourceQuery) đọc y hệt nhau ở mọi nền tảng.
//   API ở đây GIỮ NGUYÊN từng chữ so với client/windows/net/UdpSocket.h — port
//   code qua lại chỉ là chép, không phải viết lại.
//
// VỊ TRÍ TRONG KIẾN TRÚC
//   core/ (deskhub) tuyệt đối không biết đến lớp này: nó chỉ nhận/giao byte qua callback
//   `send` và hàm HandlePacket. Toàn bộ hiểu biết về socket của app nằm ở đây và ở
//   người gọi trực tiếp nó.
//
// KHÁC BIỆT SO VỚI BẢN WINDOWS
//   - Dùng file descriptor (int, -1 = đóng) thay cho SOCKET của winsock.
//   - Không cần WSAStartup/WSACleanup.
//   - Không cần tắt SIO_UDP_CONNRESET (xem ghi chú trong Open()).
//   Ngoài ba điểm đó, hai bản gần như trùng khít.
//
// QUY ƯỚC ĐỊA CHỈ
//   NetAddr giữ IP ở HOST byte order chứ không phải network byte order. Việc đổi
//   thứ tự byte dồn hết vào ranh giới gọi API hệ thống (htonl/ntohl trong .cpp),
//   nên mọi chỗ khác trong app so sánh và in địa chỉ một cách tự nhiên.
//
// SỞ HỮU TÀI NGUYÊN
//   Lớp này sở hữu file descriptor: destructor tự đóng, và copy bị CẤM (nếu cho
//   copy thì hai đối tượng cùng giữ một fd và cái nào hủy trước sẽ đóng fd của cái
//   kia). Truyền đi thì dùng tham chiếu, đừng truyền theo giá trị.
//
// LIÊN QUAN: client/windows/net/UdpSocket.h (bản song song, cùng API),
//            net/SourceQuery.h, ClientLoop.h (người dùng), docs/06 §1.3
// =============================================================================
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
