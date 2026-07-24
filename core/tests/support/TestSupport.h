#pragma once
// =============================================================================
// TestSupport.h — tiện ích dùng chung cho toàn bộ test của core.
//
// Test được tách theo tầng (gương với cây src/: wire/, transport/, session/,
// input/, control/), mỗi module một file .cpp. Những thứ NHIỀU module cùng dùng —
// bộ đếm lỗi, PRNG xác định, khung dựng/giải frame giả — gom hết vào đây để không
// lặp lại và để mọi file test nói cùng một "ngôn ngữ".
//
// Tiện ích riêng của một tầng (MakeKey của input, Fb của control, WirePair của
// session, RunFecCase của FEC) nằm ngay trong file test của tầng đó.
// =============================================================================
#include "deskhub/transport/Packetizer.h"
#include "deskhub/transport/Reassembler.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// Bộ đếm lỗi toàn cục: mọi Check thất bại tăng nó lên, main() đọc để quyết exit code.
extern int g_failures;
void Check(bool ok, const char* what);

// PRNG xác định (xorshift32) để mỗi lần chạy cho cùng kết quả — test dựng frame
// bằng dữ liệu "ngẫu nhiên" nhưng phải tái lập được khi một check thất bại.
uint32_t Rnd();

struct TestFrame {
    uint32_t id;
    bool idr;
    std::vector<uint8_t> nal;
};

using Datagram = std::vector<uint8_t>;

// Chuỗi frame giả: IDR mỗi `gop` frame, kích thước trộn các ca biên.
std::vector<TestFrame> MakeFrames(size_t count, size_t gop);

// Frame IDR có ĐÚNG `pkts` mảnh (mảnh cuối ngắn hơn để ép nhánh lenXor chạy).
TestFrame MakeIdrFrame(uint32_t id, size_t pkts);

// Cắt frame thành datagram qua Packetizer (gom vào vector thay vì gửi ra mạng).
std::vector<Datagram> Packetize(deskhub::Packetizer& pk, const TestFrame& f, uint64_t tsUs);

// Phân loại một datagram (video hay FEC) rồi đẩy vào Reassembler đúng cửa.
void Feed(deskhub::Reassembler& ra, const Datagram& d, uint64_t nowUs);

bool IsFec(const Datagram& d);

// Chỉ số (trong danh sách datagram) của gói DỮ LIỆU thứ n, bỏ qua gói parity.
size_t NthDataPacket(const std::vector<Datagram>& pkts, size_t n);

bool SameFrame(const deskhub::Reassembler::Frame& got, const TestFrame& want);
