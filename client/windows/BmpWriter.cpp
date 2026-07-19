#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "BmpWriter.h"

#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

static bool CopyToCpu(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* src, std::vector<uint8_t>& outBgra,
                      UINT& outW, UINT& outH) {
    D3D11_TEXTURE2D_DESC desc{};
    src->GetDesc(&desc);
    outW = desc.Width;
    outH = desc.Height;

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) return false;

    context->CopyResource(staging.Get(), src);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    outBgra.resize(static_cast<size_t>(desc.Width) * desc.Height * 4);
    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dstRow = outBgra.data();
    const size_t   rowSize = static_cast<size_t>(desc.Width) * 4;

    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(dstRow, srcRow, rowSize);
        srcRow += mapped.RowPitch;
        dstRow += rowSize;
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

static bool WriteBmp(const std::string& path, const std::vector<uint8_t>& bgra,
                     UINT width, UINT height) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    const DWORD pixelBytes = static_cast<DWORD>(width) * height * 4;

    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + pixelBytes;

    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(width);
    ih.biHeight = -static_cast<LONG>(height);
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = pixelBytes;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(bgra.data(), 1, bgra.size(), f);
    std::fclose(f);
    return true;
}

bool SaveTextureToBmp(ID3D11Device* device, ID3D11DeviceContext* context,
                      ID3D11Texture2D* src, const std::string& path) {
    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    if (!CopyToCpu(device, context, src, pixels, w, h)) return false;
    return WriteBmp(path, pixels, w, h);
}
