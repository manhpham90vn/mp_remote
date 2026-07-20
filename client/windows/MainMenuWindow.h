#pragma once
//
// MainMenuWindow (GD5) - màn hình chính kiểu AnyDesk, GUI thuần (không còn CLI).
// Hiện địa chỉ IP máy này, ô chỉnh Port/FPS/Bitrate (trước đây chỉ sửa được
// bằng --port/--fps/--bitrate), nút Chia sẻ (mở WindowPickerDialog rồi
// RunAgent) và ô nhập IP + nút Kết nối (RunClient).
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int RunMainMenuWindow();
