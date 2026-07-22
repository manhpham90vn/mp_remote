#pragma once
// =============================================================================
// ClientLoop.h — vai trò CLIENT: giao diện gọi vào phía xem.
//
// NHIỆM VỤ
//   Khai báo hai đường vào: hỏi host có nguồn gì (QueryHostSources) và chạy phiên
//   xem (RunClient). Phần ghép nối nằm ở ClientLoop.cpp — đọc header khối ở đó để
//   hiểu kiến trúc luồng.
//
// VỊ TRÍ TRONG LUỒNG NGƯỜI DÙNG
//   MainMenuWindow (gõ IP) → QueryHostSources() → SourcePickerDialog → RunClient()
//   RunClient CHẶN tới khi người dùng ngắt kết nối / Ctrl+C. Trong lúc chạy nó mở
//   một cửa sổ quản lý phiên xem (ui/ViewerWindow.h) hiện danh sách nguồn host
//   đang chia sẻ: `sources` chỉ là danh sách BAN ĐẦU — giữa phiên bấm Refresh để
//   hỏi lại host rồi View / Stop từng nguồn ngay trên đó.
//
// GĐ6: XEM NHIỀU NGUỒN CÙNG LÚC
//   Mỗi nguồn là một phiên HOÀN TOÀN độc lập: socket riêng, ClientSession riêng,
//   cửa sổ preview riêng, cặp thread Recv+Decode riêng. Không dùng streamId chung
//   một phiên — lý do đầy đủ ở chú thích của deskhub::SourceInfo trong Wire.h.
//   `sources` rỗng = xem nguồn 0, giữ đường cũ chạy được với host chỉ chia sẻ một thứ.
//
// VÌ SAO QueryHostSources TÁCH RIÊNG KHỎI RunClient
//   Nó chạy TRƯỚC khi có phiên và phải trả kết quả cho hộp thoại chọn nguồn. Cùng
//   lý do như SourceQuery.h bên Android tách khỏi ClientLoop bên đó.
//
// LIÊN QUAN: ClientLoop.cpp (kiến trúc luồng + ước lượng trễ e2e), AgentLoop.h
//            (phía đối diện), ui/SourcePickerDialog.h, docs/06 §5
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <vector>

#include "net/UdpSocket.h"
#include "deskhub/wire/Wire.h"

struct ClientOptions {
    NetAddr server; // địa chỉ agent
    bool saveBmp = false;
    bool sendInput = true; // GD4: đẩy phím/chuột tới host
    // Nguồn muốn xem (từ QueryHostSources). Rỗng = xem nguồn 0 — giữ đường cũ chạy
    // được với host chỉ chia sẻ một thứ.
    std::vector<deskhub::SourceInfo> sources;
};

// Hỏi host đang chia sẻ những gì. Trả false nếu host không trả lời trong ~3s
// (sai IP, firewall chặn, hoặc host bản cũ không biết LIST_SOURCES).
bool QueryHostSources(const NetAddr& server, std::vector<deskhub::SourceInfo>& out);

// Kết nối tới agent, hiện video tới khi người dùng ngắt kết nối (nút Disconnect
// trên cửa sổ quản lý, hoặc đóng hết preview khi không có cửa sổ đó) / Ctrl+C.
// Trả về exit code cho main.
int RunClient(const ClientOptions& opt);
