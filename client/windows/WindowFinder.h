#pragma once
//
// Tìm cửa sổ chính của một tiến trình theo tên file exe.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Trả về HWND cửa sổ hiển thị lớn nhất thuộc process có tên exe khớp `exeName`
// (so sánh không phân biệt hoa thường, chỉ phần tên file). nullptr nếu không thấy.
HWND FindWindowByProcessName(const std::wstring& exeName);

// Một cửa sổ có thể chọn làm nguồn stream.
struct WindowInfo {
    HWND         hwnd = nullptr;
    std::wstring exeName;   // tên file exe, chữ thường
    std::wstring title;
    uint32_t     width = 0, height = 0;   // kích thước client
    bool         minimized = false;       // đang thu nhỏ -> cần restore trước khi capture
};

// Liệt kê các cửa sổ top-level capture được: hiển thị, không owner, có tiêu đề,
// không bị DWM cloak (UWP ẩn), không thuộc chính process này.
// Sắp xếp theo diện tích giảm dần (cửa sổ game thường lớn nhất).
std::vector<WindowInfo> ListCapturableWindows();
