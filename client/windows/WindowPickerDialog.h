#pragma once
//
// WindowPickerDialog (GD5) - hop thoai chon cua so nguon de chia se, thay cho
// menu console cu (PickWindowFromConsole). Danh sach lay tu
// ListCapturableWindows() (WindowFinder.h), kem checkbox "cho dieu khien" gop
// luon cau hoi allow-input vao cung mot buoc thay vi hoi rieng sau do.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// Hien hop thoai MODAL (vo hieu hoa `owner` trong luc mo). Tra false neu nguoi
// dung bam Huy/dong cua so hoac khong con cua so nao de chon. `outAllowInput`
// chi co y nghia khi tra ve true.
bool ShowWindowPickerDialog(HWND owner, HWND& outTarget, bool& outAllowInput);
