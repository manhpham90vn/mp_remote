#pragma once
// =============================================================================
// WindowPickerDialog.h — hộp thoại chọn nguồn để chia sẻ, phía HOST (GĐ5/GĐ6).
//
// NHIỆM VỤ
//   Bước giữa "bấm nút Chia sẻ" và "bắt đầu stream": cho người dùng chọn chia sẻ
//   cái gì, và có cho điều khiển hay không. Thay cho menu console cũ
//   (PickWindowFromConsole).
//
// DANH SÁCH GỒM HAI LOẠI NGUỒN, theo thứ tự
//   1. Màn hình (EnumDisplayMonitors) — chia sẻ trọn một màn hình.
//   2. Cửa sổ  (ListCapturableWindows, xem WindowFinder.h) — chia sẻ một ứng dụng.
//   Màn hình xếp trước vì đó là lựa chọn dễ hiểu nhất với người mới dùng.
//
// VÌ SAO GỘP CHECKBOX "CHO ĐIỀU KHIỂN" VÀO ĐÂY
//   Đây là quyết định về QUYỀN, và nó phải nằm ngay cạnh quyết định "chia sẻ cái
//   gì" để người dùng thấy cả hai cùng lúc. Hỏi riêng ở một bước sau thì người ta
//   bấm qua theo quán tính mà không đọc.
//
// GĐ6: CHỌN ĐƯỢC NHIỀU NGUỒN CÙNG LÚC
//   Mỗi nguồn thành một phiên độc lập với encoder và bitrate riêng — xem ghi chú
//   về kMaxSources trong deskhub/wire/Wire.h.
//
// LIÊN QUAN: ui/MainMenuWindow.h (nơi mở hộp thoại này), capture/WindowFinder.h
//            (nguồn danh sách cửa sổ), AgentLoop.h (AgentSource),
//            ui/SourcePickerDialog.h (hộp thoại đối xứng phía client)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

#include "AgentLoop.h" // AgentSource

// Hiện hộp thoại MODAL (vô hiệu hóa `owner` trong lúc mở). Trả false nếu người
// dùng bấm Hủy/đóng cửa sổ hoặc không chọn nguồn nào. `outAllowInput` chỉ có ý
// nghĩa khi trả về true.
bool ShowWindowPickerDialog(HWND owner, std::vector<AgentSource>& outSources,
    bool& outAllowInput);

// Biến thể "thêm nguồn GIỮA PHIÊN" (nút Add của cửa sổ phiên — SessionWindow):
// cùng danh sách, nhưng KHÔNG có checkbox điều khiển — quyền điều khiển là
// quyết định một lần cho cả phiên, đã chốt lúc bấm Share (đổi giữa chừng còn
// kéo theo cả chuyện nâng quyền UAC). Nút xác nhận đề "Add" thay vì "Share".
bool ShowWindowPickerAddDialog(HWND owner, std::vector<AgentSource>& outSources);
