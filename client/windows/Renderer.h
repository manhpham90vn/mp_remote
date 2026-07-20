#pragma once
//
// Renderer - cửa sổ preview + DXGI swapchain, vẽ frame NV12 từ decoder.
//
// Thiết kế:
//   - Chuyển màu NV12 -> BGRA + scale bằng D3D11 Video Processor (phần cứng,
//     không cần viết shader). Input view trỏ thẳng vào texture pool của decoder
//     (kèm array slice) -> zero-copy từ decode đến swapchain.
//   - Swapchain flip-model (FLIP_DISCARD) + Present(0) cho độ trễ thấp.
//   - Cửa sổ tạo và bơm message trên luồng GỌI Init/Pump (main). RenderNV12 được
//     phép gọi từ luồng khác (chuỗi callback decode); device đã bật
//     multithread-protected nên immediate context an toàn.
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

    // Tạo cửa sổ (client ~ srcW x srcH, thu nhỏ nếu vượt màn hình) + swapchain
    // trên `device` dùng chung. Gọi trên luồng sẽ Pump().
    bool Init(ID3D11Device* device, uint32_t srcW, uint32_t srcH, const wchar_t* title);

    // Vẽ một frame NV12 (texture + array slice từ decoder) lên backbuffer và Present.
    // `width`/`height` là kích thước hiển thị thực (có thể nhỏ hơn texture do align).
    bool RenderNV12(ID3D11Texture2D* tex, UINT subresource, uint32_t width, uint32_t height);

    // DEBUG: frame kế tiếp sẽ lưu backbuffer ra BMP (trước khi Present) để kiểm chứng.
    void RequestDumpBmp(const std::string& path);

    // GD4: chuyển tiếp message cửa sổ cho bên ngoài (InputCapture) TRƯỚC khi
    // Renderer xử lý. Trả true = đã tiêu thụ, Renderer bỏ qua message đó.
    // Renderer không biết gì về ngữ nghĩa input - chỉ làm đường ống.
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM)>;
    void SetMessageHook(MessageHook hook);

    // GD5: overlay trên cửa sổ preview - 2 nút góc trên-phải. Renderer không
    // biết gì về ngữ nghĩa khóa chuột/tạm dừng, chỉ báo id nút vừa bấm ra ngoài
    // (giống SetMessageHook), giống hệt đường phím tắt F9/F10.
    static constexpr int kBtnLock  = 1001; // == F9 (khóa/thả chuột tương đối)
    static constexpr int kBtnPause = 1002; // == F10 (tạm dừng/tiếp tục gửi input)
    using CommandHook = std::function<void(int id)>;
    void SetCommandHook(CommandHook hook);

    // Dòng chữ số liệu (fps/kbps/mất gói/RTT/e2e) hiện góc trên-trái cửa sổ.
    // Chỉ gọi từ luồng đã Init/Pump.
    void SetStatusText(const wchar_t* text);

    // Đồng bộ trạng thái 2 nút với InputCapture khi người dùng đổi bằng phím tắt
    // thay vì click. Chỉ gọi từ luồng đã Init/Pump.
    void SetToggleState(bool locked, bool paused);

    // HWND cửa sổ preview (nullptr nếu chưa Init) - để đăng ký Raw Input.
    HWND Hwnd() const;

    // Kích thước vùng client (dùng để chuẩn hóa tọa độ chuột).
    void ClientSize(uint32_t& w, uint32_t& h) const;

    // Bơm message của cửa sổ - gọi lặp lại trên luồng đã Init.
    void Pump();

    // True khi người dùng đóng cửa sổ preview.
    bool Closed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
