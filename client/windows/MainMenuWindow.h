#pragma once
//
// MainMenuWindow (GD5) - man hinh chinh kieu AnyDesk, GUI thuan (khong con CLI).
// Hien dia chi IP may nay, o chinh Port/FPS/Bitrate (truoc day chi sua duoc
// bang --port/--fps/--bitrate), nut Chia se (mo WindowPickerDialog roi
// RunAgent) va o nhap IP + nut Ket noi (RunClient).
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

int RunMainMenuWindow();
