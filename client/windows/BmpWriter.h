#pragma once
//
// Cong cu DEBUG - KHONG nam trong duong nong streaming.
// Copy mot texture VRAM ve CPU va luu ra file .bmp de kiem chung bang mat thuong.
// Duong nay CHAM (VRAM -> CPU); streaming that dua texture thang sang encoder.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <string>

// Copy `src` (BGRA, VRAM) ve CPU roi ghi ra file BMP 32-bit tai `path`.
// Tra ve false neu copy hoac ghi that bai.
bool SaveTextureToBmp(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* src, const std::string& path);
