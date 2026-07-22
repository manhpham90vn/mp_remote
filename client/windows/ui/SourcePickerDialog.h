#pragma once
// =============================================================================
// SourcePickerDialog.h — hộp thoại chọn nguồn muốn xem, phía CLIENT (GĐ6).
//
// NHIỆM VỤ
//   Host có thể chia sẻ nhiều cửa sổ cùng lúc. Sau khi QueryHostSources() lấy được
//   danh sách (LIST_SOURCES → SOURCE_LIST), hộp thoại này cho người dùng tick
//   những nguồn muốn mở. Mỗi nguồn được chọn sẽ thành một cửa sổ preview riêng.
//
// VỊ TRÍ TRONG LUỒNG NGƯỜI DÙNG
//   MainMenuWindow (gõ IP) → QueryHostSources → **SourcePickerDialog** → ClientLoop
//
// ĐỐI XỨNG VỚI WindowPickerDialog PHÍA HOST
//   Cùng kiểu listbox nhiều lựa chọn, cùng cách trả kết quả. Bên host chọn "chia
//   sẻ cái gì", bên client chọn "xem cái gì" — hai nửa của cùng một câu hỏi.
//
// KHÁC BẢN ANDROID
//   Android chỉ cho chọn MỘT nguồn (một Activity, một Surface). Bản Windows mở
//   được nhiều cửa sổ preview cùng lúc nên cho tick nhiều — xem MainActivity.kt.
//
// LIÊN QUAN: ui/WindowPickerDialog.h (đối xứng phía host), ClientLoop.h,
//            deskhub/wire/Wire.h (SourceInfo, kMaxSources)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>

#include "deskhub/wire/Wire.h"

// Hiện hộp thoại MODAL. `sources` là danh sách host trả về; `outSelected` nhận
// những nguồn người dùng tick. Trả false nếu hủy hoặc không chọn gì.
bool ShowSourcePickerDialog(HWND owner, const std::vector<deskhub::SourceInfo>& sources,
                            std::vector<deskhub::SourceInfo>& outSelected);
