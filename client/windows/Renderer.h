#pragma once
//
// Renderer - cua so preview + DXGI swapchain, ve frame NV12 tu decoder.
//
// Thiet ke:
//   - Chuyen mau NV12 -> BGRA + scale bang D3D11 Video Processor (phan cung,
//     khong can viet shader). Input view tro thang vao texture pool cua decoder
//     (kem array slice) -> zero-copy tu decode den swapchain.
//   - Swapchain flip-model (FLIP_DISCARD) + Present(0) cho do tre thap.
//   - Cua so tao va bom message tren luong GOI Init/Pump (main). RenderNV12 duoc
//     phep goi tu luong khac (chuoi callback decode); device da bat
//     multithread-protected nen immediate context an toan.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Tao cua so (client ~ srcW x srcH, thu nho neu vuot man hinh) + swapchain
    // tren `device` dung chung. Goi tren luong se Pump().
    bool Init(ID3D11Device* device, uint32_t srcW, uint32_t srcH, const wchar_t* title);

    // Ve mot frame NV12 (texture + array slice tu decoder) len backbuffer va Present.
    // `width`/`height` la kich thuoc hien thi thuc (co the nho hon texture do align).
    bool RenderNV12(ID3D11Texture2D* tex, UINT subresource, uint32_t width, uint32_t height);

    // DEBUG: frame ke tiep se luu backbuffer ra BMP (truoc khi Present) de kiem chung.
    void RequestDumpBmp(const std::string& path);

    // GD4: chuyen tiep message cua so cho ben ngoai (InputCapture) TRUOC khi
    // Renderer xu ly. Tra true = da tieu thu, Renderer bo qua message do.
    // Renderer khong biet gi ve ngu nghia input - chi lam duong ong.
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
    void SetMessageHook(MessageHook hook);

    // GD5: overlay tren cua so preview - 2 nut goc tren-phai. Renderer khong
    // biet gi ve ngu nghia khoa chuot/tam dung, chi bao id nut vua bam ra ngoai
    // (giong SetMessageHook), giong het duong phim tat F9/F10.
    static constexpr int kBtnLock  = 1001; // == F9 (khoa/tha chuot tuong doi)
    static constexpr int kBtnPause = 1002; // == F10 (tam dung/tiep tuc gui input)
    using CommandHook = std::function<void(int id)>;
    void SetCommandHook(CommandHook hook);

    // Dong chu so lieu (fps/kbps/mat goi/RTT/e2e) hien goc tren-trai cua so.
    // Chi goi tu luong da Init/Pump.
    void SetStatusText(const wchar_t* text);

    // Dong bo trang thai 2 nut voi InputCapture khi nguoi dung doi bang phim tat
    // thay vi click. Chi goi tu luong da Init/Pump.
    void SetToggleState(bool locked, bool paused);

    // HWND cua so preview (nullptr neu chua Init) - de dang ky Raw Input.
    HWND Hwnd() const;

    // Kich thuoc vung client (dung de chuan hoa toa do chuot).
    void ClientSize(uint32_t& w, uint32_t& h) const;

    // Bom message cua cua so - goi lap lai tren luong da Init.
    void Pump();

    // True khi nguoi dung dong cua so preview.
    bool Closed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
