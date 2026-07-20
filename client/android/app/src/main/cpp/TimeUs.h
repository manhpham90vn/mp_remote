#pragma once
// Đồng hồ đơn điệu, micro giây — tương đương QpcUs() của client Windows.
// CLOCK_MONOTONIC không nhảy khi người dùng chỉnh giờ hệ thống; nó ĐỨNG YÊN khi
// máy ngủ sâu, nhưng phiên stream cũng chết lúc đó nên không ảnh hưởng.
#include <cstdint>
#include <ctime>

inline uint64_t NowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000ull + uint64_t(ts.tv_nsec) / 1000ull;
}
