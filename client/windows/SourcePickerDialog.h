#pragma once
//
// SourcePickerDialog (GD6, phía CLIENT) - hộp thoại chọn nguồn nào của host muốn
// xem, sau khi QueryHostSources() đã lấy được danh sách. Đối xứng với
// WindowPickerDialog phía host: cùng kiểu listbox nhiều lựa chọn.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

#include "rgc/Wire.h"

// Hiện hộp thoại MODAL. `sources` là danh sách host trả về; `outSelected` nhận
// những nguồn người dùng tick. Trả false nếu hủy hoặc không chọn gì.
bool ShowSourcePickerDialog(HWND owner, const std::vector<rgc::SourceInfo>& sources,
                            std::vector<rgc::SourceInfo>& outSelected);
