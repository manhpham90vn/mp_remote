#pragma once
//
// WindowPickerDialog (GD5) - hộp thoại chọn cửa sổ nguồn để chia sẻ, thay cho
// menu console cũ (PickWindowFromConsole). Danh sách lấy từ
// ListCapturableWindows() (WindowFinder.h), kèm checkbox "cho điều khiển" gộp
// luôn câu hỏi allow-input vào cùng một bước thay vì hỏi riêng sau đó.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Hiện hộp thoại MODAL (vô hiệu hóa `owner` trong lúc mở). Trả false nếu người
// dùng bấm Hủy/đóng cửa sổ hoặc không còn cửa sổ nào để chọn. `outAllowInput`
// chỉ có ý nghĩa khi trả về true.
bool ShowWindowPickerDialog(HWND owner, HWND& outTarget, bool& outAllowInput);
