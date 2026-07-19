#pragma once
// Đọc/ghi số nguyên big-endian (network byte order) độc lập nền tảng.
// Cố ý không dùng htons/htonl để không kéo theo header hệ điều hành nào.
#include <cstdint>

namespace rgc {

inline void PutU16(uint8_t* p, uint16_t v) noexcept {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

inline void PutU32(uint8_t* p, uint32_t v) noexcept {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

inline void PutU64(uint8_t* p, uint64_t v) noexcept {
    PutU32(p, uint32_t(v >> 32));
    PutU32(p + 4, uint32_t(v));
}

inline uint16_t GetU16(const uint8_t* p) noexcept {
    return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}

inline uint32_t GetU32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

inline uint64_t GetU64(const uint8_t* p) noexcept {
    return (uint64_t(GetU32(p)) << 32) | GetU32(p + 4);
}

} // namespace rgc
