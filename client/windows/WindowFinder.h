#pragma once
//
// Tim cua so chinh cua mot tien trinh theo ten file exe.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Tra ve HWND cua so hien thi lon nhat thuoc process co ten exe khop `exeName`
// (so sanh khong phan biet hoa thuong, chi phan ten file). nullptr neu khong thay.
HWND FindWindowByProcessName(const std::wstring& exeName);

// Mot cua so co the chon lam nguon stream.
struct WindowInfo {
    HWND         hwnd = nullptr;
    std::wstring exeName;   // ten file exe, chu thuong
    std::wstring title;
    uint32_t     width = 0, height = 0;   // kich thuoc client
    bool         minimized = false;       // dang thu nho -> can restore truoc khi capture
};

// Liet ke cac cua so top-level capture duoc: hien thi, khong owner, co tieu de,
// khong bi DWM cloak (UWP an), khong thuoc chinh process nay.
// Sap xep theo dien tich giam dan (cua so game thuong lon nhat).
std::vector<WindowInfo> ListCapturableWindows();
