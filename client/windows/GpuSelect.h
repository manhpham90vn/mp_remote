#pragma once
//
// Chọn GPU theo chuỗi ưu tiên: NVIDIA -> Intel -> ... -> CPU (WARP).
//
// Lý do: máy Agent có thể có GPU rời (NVENC), máy Client có thể chỉ có iGPU Intel (QSV),
// và trường hợp xấu nhất không có HW encoder thì phải chạy software. Cả capture lẫn
// encoder dùng CHUNG device trả về ở đây để tránh copy chéo GPU.
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
    std::wstring                                description;   // tên adapter
    GpuVendor                                   vendor = GpuVendor::Unknown;
    bool                                        hardware = false; // false = WARP/software
};

// Tạo D3D11 device trên GPU đầu tiên khớp `preference` (theo thứ tự). Nếu không adapter
// phần cứng nào dùng được, rớt về WARP (software). Trả về false nếu thất bại hoàn toàn.
bool CreateBestDevice(const std::vector<GpuVendor>& preference, GpuChoice& out);
