#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "GpuSelect.h"

#include <dxgi1_2.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

const wchar_t* GpuVendorName(GpuVendor v) {
    switch (v) {
        case GpuVendor::Nvidia:    return L"NVIDIA";
        case GpuVendor::Intel:     return L"Intel";
        case GpuVendor::Amd:       return L"AMD";
        case GpuVendor::Microsoft: return L"Microsoft (WARP/software)";
        default:                   return L"Unknown";
    }
}

static GpuVendor VendorFromId(UINT vendorId) {
    switch (vendorId) {
        case 0x10DE: return GpuVendor::Nvidia;
        case 0x8086: return GpuVendor::Intel;
        case 0x1002: return GpuVendor::Amd;
        case 0x1414: return GpuVendor::Microsoft;
        default:     return GpuVendor::Unknown;
    }
}

// Cố gắng tạo device trên một adapter cụ thể (nullptr = WARP).
static bool TryCreateOnAdapter(IDXGIAdapter1* adapter, D3D_DRIVER_TYPE driverType,
                               GpuChoice& out) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT   // WGC + video
                     | D3D11_CREATE_DEVICE_VIDEO_SUPPORT; // Media Foundation HW
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };

    HRESULT hr = D3D11CreateDevice(
        adapter, driverType, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        out.device.ReleaseAndGetAddressOf(), nullptr,
        out.context.ReleaseAndGetAddressOf());
    return SUCCEEDED(hr);
}

bool CreateBestDevice(const std::vector<GpuVendor>& preference, GpuChoice& out) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        std::printf("CreateDXGIFactory1 failed.\n");
        return false;
    }

    // Thử từng vendor theo thứ tự ưu tiên; với mỗi vendor, quét các adapter.
    for (GpuVendor want : preference) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // bỏ WARP ở vòng này
            if (VendorFromId(desc.VendorId) != want) continue;

            // Với adapter tường minh phải dùng DRIVER_TYPE_UNKNOWN.
            if (TryCreateOnAdapter(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, out)) {
                out.description = desc.Description;
                out.vendor = want;
                out.hardware = true;
                return true;
            }
        }
    }

    // Rớt về WARP (software) - "CPU".
    std::printf("No preferred hardware GPU found; falling back to WARP (software).\n");
    if (TryCreateOnAdapter(nullptr, D3D_DRIVER_TYPE_WARP, out)) {
        out.description = L"WARP (software rasterizer)";
        out.vendor = GpuVendor::Microsoft;
        out.hardware = false;
        return true;
    }
    return false;
}
