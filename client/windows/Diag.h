#pragma once
// =============================================================================
// Diag.h — tiện ích cho log chẩn đoán điểm nghẽn (xem docs/09-diagnostics.md).
//
// NHIỆM VỤ
//   Log [DIAG] LUÔN BẬT, không có cờ tắt: diag chỉ thêm in log (bộ đếm thì luôn
//   chạy, chi phí không đáng kể), và khi sự cố xảy ra thì log đã-có-sẵn đáng giá
//   hơn nhiều so với việc phải tái hiện lại. File này chỉ còn giữ helper dùng
//   chung cho các bộ đếm cửa sổ.
//
// ĐỊNH DẠNG THỐNG NHẤT: `[DIAG][<nguồn>] evt=<tên> k1=v1 k2=v2` — một sự kiện
// một dòng, grep được, và là thứ người dùng gửi kèm khi cần chẩn đoán từ xa.
//
// LIÊN QUAN: AgentLoop.cpp / ClientLoop.cpp (nơi phát log), docs/09-diagnostics.md
// =============================================================================
#include <atomic>
#include <cstdint>

// Cập nhật "giá trị lớn nhất" trên một atomic khi NHIỀU thread cùng ghi (bộ đếm
// max của cửa sổ chẩn đoán). Vòng CAS chuẩn: thất bại thì `cur` được nạp lại và
// điều kiện xét lại, không bao giờ ghi đè max bằng số nhỏ hơn.
inline void DiagAtomicMax(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
