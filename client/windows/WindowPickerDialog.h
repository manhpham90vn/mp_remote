#pragma once
//
// WindowPickerDialog (GD5) - hộp thoại chọn nguồn để chia sẻ, thay cho menu console
// cũ (PickWindowFromConsole). Kèm checkbox "cho điều khiển" gộp luôn câu hỏi
// allow-input vào cùng một bước thay vì hỏi riêng sau đó.
//
// GD6: chọn được NHIỀU nguồn cùng lúc, và nguồn có thể là CẢ MÀN HÌNH chứ không
// chỉ cửa sổ. Danh sách = màn hình (EnumDisplayMonitors) rồi tới cửa sổ
// (ListCapturableWindows, WindowFinder.h).
//
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
