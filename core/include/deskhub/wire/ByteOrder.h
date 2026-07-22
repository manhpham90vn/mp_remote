#pragma once
// =============================================================================
// ByteOrder.h — đọc/ghi số nguyên big-endian trên bộ đệm byte thô.
//
// NHIỆM VỤ
//   Lớp thấp nhất của tầng wire: chuyển qua lại giữa số nguyên của máy và chuỗi
//   byte trên đường truyền. Giao thức v1 quy định MỌI trường số là big-endian
//   (network byte order), nên đây là chỗ duy nhất trong dự án được phép biết về
//   thứ tự byte — Wire.cpp gọi xuống đây chứ không tự dịch bit.
//
// VÌ SAO KHÔNG DÙNG htons/htonl
//   Chúng nằm trong <winsock2.h> / <arpa/inet.h>, tức là kéo theo header hệ điều
//   hành vào tận lõi. core/ phải biên dịch được cho Windows, Android NDK, iOS,
//   Ubuntu bằng cùng một mã nguồn, nên lõi tuyệt đối không include header nền
//   tảng nào. Tự dịch bằng phép dịch bit vừa di động vừa không phụ thuộc gì.
//
// VÌ SAO CÁCH VIẾT NÀY AN TOÀN
//   Chỉ dùng phép dịch bit và gán từng byte, không ép kiểu con trỏ (kiểu
//   `*(uint32_t*)p`). Ép kiểu con trỏ sẽ hỏng ở hai chỗ: đọc sai trên máy
//   little-endian, và unaligned access — payload trên wire không bảo đảm căn
//   biên 4/8 byte, trên ARM điều đó là hành vi không xác định.
//
// SỬ DỤNG
//   Người gọi tự bảo đảm `p` còn đủ 2/4/8 byte. Các hàm ở đây KHÔNG kiểm tra
//   biên (chúng nằm trên đường nóng, mỗi gói video gọi hàng chục lần); việc kiểm
//   tra độ dài nằm ở Wire.cpp — mọi hàm Parse* đối chiếu kích thước payload
//   trước khi gọi xuống đây.
//
// LIÊN QUAN: deskhub/wire/Wire.h (người dùng duy nhất), docs/04-protocol.md
// =============================================================================
#include <cstdint>

namespace deskhub {

// Ghi 2 byte: byte nặng trước. Ép về uint8_t tự cắt lấy 8 bit thấp nên byte sau
// không cần mặt nạ 0xFF.
inline void PutU16(uint8_t* p, uint16_t v) noexcept {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

// Ghi 4 byte theo cùng quy tắc: dịch phải dần từ 24 bit về 0.
inline void PutU32(uint8_t* p, uint32_t v) noexcept {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// 64 bit = hai nửa 32 bit, nửa cao đứng trước (vẫn là big-endian). Dùng cho mọi
// mốc thời gian trên wire (timestampUs, sendTimeUs) vì micro-giây tràn 32 bit sau
// khoảng 71 phút.
inline void PutU64(uint8_t* p, uint64_t v) noexcept {
    PutU32(p, uint32_t(v >> 32));
    PutU32(p + 4, uint32_t(v));
}

// Đọc ngược lại. Ép p[0] lên uint16_t TRƯỚC khi dịch: uint8_t bị thăng cấp thành
// int, dịch trái rồi mới thu hẹp sẽ sinh cảnh báo thu hẹp ở một số trình dịch.
inline uint16_t GetU16(const uint8_t* p) noexcept {
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

// Mỗi byte được ép lên uint32_t trước khi dịch, vì dịch trái 24 bit trên int
// (16 bit ở lý thuyết chuẩn C++) là hành vi không xác định.
inline uint32_t GetU32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

// Ghép hai nửa 32 bit. Nửa cao phải ép lên uint64_t trước khi dịch 32 bit.
inline uint64_t GetU64(const uint8_t* p) noexcept {
    return (uint64_t(GetU32(p)) << 32) | GetU32(p + 4);
}

} // namespace deskhub
