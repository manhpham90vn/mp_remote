#pragma once
//
// Chon GPU theo chuoi uu tien: NVIDIA -> Intel -> ... -> CPU (WARP).
//
// Ly do: may Agent co the co GPU roi (NVENC), may Client co the chi co iGPU Intel (QSV),
// va truong hop xau nhat khong co HW encoder thi phai chay software. Ca capture lan
// encoder dung CHUNG device tra ve o day de tranh copy cheo GPU.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>

enum class GpuVendor { Nvidia, Intel, Amd, Microsoft /*WARP - software*/, Unknown };

const wchar_t* GpuVendorName(GpuVendor v);

struct GpuChoice {
    Microsoft::WRL::ComPtr<ID3D11Device>        device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::wstring                                description;   // ten adapter
    GpuVendor                                   vendor = GpuVendor::Unknown;
    bool                                        hardware = false; // false = WARP/software
};

// Tao D3D11 device tren GPU dau tien khop `preference` (theo thu tu). Neu khong adapter
// phan cung nao dung duoc, rot ve WARP (software). Tra ve false neu that bai hoan toan.
bool CreateBestDevice(const std::vector<GpuVendor>& preference, GpuChoice& out);
