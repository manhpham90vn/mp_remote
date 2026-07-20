// client.exe - MOT exe kieu AnyDesk chua ca hai vai tro (host + client).
//
// GD5: toan bo dieu khien qua GUI Win32 (MainMenuWindow.h) - khong con CLI.
// Build: CMake + Ninja (CMakePresets.json, preset x64-debug/x64-release).
// Chay:  client.exe (khong tham so) -> mo man hinh chinh: nut "Chia se ung
// dung" (mo WindowPickerDialog.h roi RunAgent) hoac o nhap IP + nut "Ket noi"
// (RunClient). Bitrate/FPS/port chinh truc tiep trong cua so do.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <clocale>
#include <cstdio>

#include "MainMenuWindow.h"
#include "WindowCapture.h"

int main(int, char**) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // UTF-8 cho console de wprintf in dung tieu de cua so co dau (tieng Viet...).
    std::setlocale(LC_ALL, ".UTF8");
    SetConsoleOutputCP(CP_UTF8);
    // Log ra ngay ca khi stdout bi redirect (CRT full-buffer khi khong phai console).
    setvbuf(stdout, nullptr, _IONBF, 0);
    capture::InitRuntime();

    return RunMainMenuWindow();
}
