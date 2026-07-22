#pragma once
// =============================================================================
// Pacer.h — rải gói của một frame ra theo thời gian thay vì bắn hết một lúc.
//
// NHIỆM VỤ
//   Chặn ngay trước mỗi sendto() đủ lâu để tốc độ phát trung bình không vượt
//   `rateBps_`. Một dòng chức năng, nhưng nó giải quyết vấn đề mất gói lớn nhất mà
//   dự án từng gặp.
//
// VẤN ĐỀ NÓ GIẢI QUYẾT
//   Packetizer::SendFrame gọi `send` trong một vòng for không nghỉ, nên một IDR
//   450 KB rời card mạng 1 Gbps thành một chùm liên tục. Đường đi tới client bước
//   xuống Wi-Fi ~300 Mbps; chỗ thắt đó phải đệm trọn cả chùm và nhả chậm hơn ba
//   lần. Hàng đợi tràn thì nó tail-drop nguyên một DẢI gói liền nhau.
//
// SỐ ĐO THẬT (Pixel 4, docs/08 §6) — khẳng định đúng hình dạng đó
//   Cả phiên chỉ có MỘT chùm mất dài 1 gói; còn lại gần như toàn bộ ≥32 gói, dài
//   nhất là 384 gói (~450 KB) mất cùng lúc.
//   Đó cũng là lý do FEC luôn báo `fec+0`: parity XOR trên kFecGroupSize gói liên
//   tiếp cứu được đúng một gói mỗi nhóm, chùm 384 thì không cách nào gánh nổi
//   (muốn gỡ phải có 384 gói parity, tức 100% overhead).
//   Kết luận: chữa ở GỐC — đừng bắn thành chùm — chứ không phải nới FEC.
//
// VÌ SAO NẰM Ở client/windows CHỨ KHÔNG PHẢI core/
//   Chỉ host mới cần lớp này (client chỉ nhận video), và nó cần đồng hồ và cần
//   NGỦ — hai thứ core cố ý không có.
//
// ⚠ PHẢI GỌI TỪ THREAD GỬI RIÊNG, TUYỆT ĐỐI KHÔNG TỪ onPacket
//   Encoder gọi onPacket đồng bộ ngay trong Encode(), mà Encode() lại nằm trong
//   callback FrameArrived của WGC — ngủ ở đó là giữ slot frame pool và làm capture
//   đứng hình. Bản đầu mắc đúng lỗi này và trả giá ngay khi đo: stream tụt về
//   ~0 kbps, e2e vọt lên 13,8 GIÂY.
//   Đó cũng là lý do lớp này không còn "ngân sách chống treo" nào — chặn thread gửi
//   bao lâu cũng vô hại; cái phải giới hạn là HÀNG ĐỢI (số gói xếp hàng), không
//   phải đồng hồ.
//
// LIÊN QUAN: AgentLoop.cpp (thread gửi — người dùng duy nhất),
//            deskhub/transport/Packetizer.h (nguồn sinh ra chùm gói), docs/08 §6
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>

class Pacer {
public:
    Pacer() = default;
    ~Pacer();
    Pacer(const Pacer&) = delete;
    Pacer& operator=(const Pacer&) = delete;

    // 0 = tắt pacing (Gate trả về ngay). Đổi giữa chừng có hiệu lực từ gói kế tiếp.
    void SetRateBps(uint64_t rateBps) { rateBps_ = rateBps; }

    // CHẶN tới khi được phép gửi thêm `bytes` byte. Gọi ngay trước sendto().
    void Gate(size_t bytes);

private:
    void SleepUs(uint64_t us);

    uint64_t rateBps_ = 0;
    uint64_t nextUs_  = 0;       // thời điểm sớm nhất được phép gửi gói kế tiếp
    HANDLE   timer_   = nullptr; // waitable timer độ phân giải cao, tạo lười
};
