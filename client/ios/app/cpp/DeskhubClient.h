// =============================================================================
// DeskhubClient.h — mặt tiền C duy nhất cho Swift gọi xuống tầng C++.
//                   Đối ứng JniBridge.cpp + NativeClient.kt bên Android.
//
// VÌ SAO LÀ HÀM C THẦ CHỨ KHÔNG PHẢI LỚP OBJ-C
//   Bridging header của Swift nhập hàm C nguyên bản. Lớp Obj-C vẫn dùng được nhưng
//   thêm một lớp dispatch không cần thiết khi facade chỉ là bọc mỏng cho một đối
//   tượng toàn cục (g_client). Hàm C giữ API phẳng, dễ đọc, dễ test.
//
// MÔ HÌNH "MỘT PHIÊN TOÀN CỤC"
//   App chỉ xem MỘT nguồn tại một thời điểm (view-only v1) nên toàn bộ trạng thái
//   nằm trong biến static duy nhất bên .mm. Đây là đối ứng chính xác của biến toàn
//   cục g_client trong JniBridge.cpp bên Android.
//
// THREAD SAFETY
//   Mọi hàm an toàn gọi từ main thread. listSources CHẶN ~3s nên PHẢI gọi ngoài
//   main (Swift dùng Task.detached). start/stop/setLayer từ main thread. phase/
//   statusLine/videoWidth/videoHeight thread-safe nhờ atomic/mutex bên trong.
//
// LIÊN QUAN: DeskhubClient.mm (cài đặt), app/DeskhubClient.swift (bọc Swift),
//            client/android/.../JniBridge.cpp (bản song song)
// =============================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Trạng thái phiên mà UI quan tâm.
typedef enum {
    DHPhaseIdle = 0,
    DHPhaseConnecting = 1,
    DHPhaseStreaming = 2,
    DHPhaseEnded = 3,
} DHPhase;

// Thông tin một nguồn (cửa sổ) host đang chia sẻ.
typedef struct {
    uint8_t sourceId;
    uint16_t width;
    uint16_t height;
    char name[256];
} DHSourceInfo;

// Hỏi host đang chia sẻ gì. CHẶN ~3s, gọi ngoài main thread.
// Trả về số nguồn tìm thấy (0 = host im lặng / bản cũ).
int dh_list_sources(const char* address, DHSourceInfo* out, int capacity);

// Bắt đầu phiên xem. Trả true nếu khởi động thành công.
bool dh_start(const char* address, uint8_t sourceId);

// Dừng phiên hiện tại.
void dh_stop(void);

// Giao/thu hồi layer. `layer` là AVSampleBufferDisplayLayer* (__bridge void*),
// hoặc NULL khi app xuống nền / view biến mất. CHẶN main thread cho tới khi
// thread Decode xác nhận đã buông layer cũ.
void dh_set_layer(void* layer);

// Trạng thái phiên hiện tại.
DHPhase dh_phase(void);

// Dòng số liệu cho overlay (fps/kbps/RTT/e2e). Trả chuỗi tĩnh, hợp lệ tới
// lần gọi kế (không cần free). Rỗng khi chưa có số liệu.
const char* dh_status_line(void);

// Lý do phiên kết thúc. Rỗng nếu chưa kết thúc.
const char* dh_end_reason(void);

// Kích thước video đàm phán được. 0 nếu chưa đàm phán xong.
uint32_t dh_video_width(void);
uint32_t dh_video_height(void);

#ifdef __cplusplus
}
#endif
