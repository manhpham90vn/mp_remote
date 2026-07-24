#pragma once
// =============================================================================
// Renderer.h — cửa sổ preview + DXGI swapchain, vẽ frame NV12 từ decoder.
//
// NHIỆM VỤ
//   Chặng cuối của luồng video phía client. Ngoài việc vẽ, nó còn sở hữu cửa sổ
//   nên gánh thêm hai vai trò phụ: làm đường ống chuyển message cho InputCapture,
//   và hiển thị overlay (dòng số liệu + hai nút điều khiển).
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   UDP ~~~> Reassembler → IVideoDecoder → **Renderer** → màn hình
//
// BỐN QUYẾT ĐỊNH THIẾT KẾ
//   1. CHUYỂN MÀU BẰNG VIDEO PROCESSOR. NV12 → BGRA và co giãn đều do D3D11 Video
//      Processor làm, tức là phần cứng, không phải viết shader. Input view trỏ
//      THẲNG vào texture pool của decoder (kèm array slice) nên khung hình không
//      rời VRAM trên cả chặng decode → màn hình.
//   2. SWAPCHAIN FLIP-MODEL + Present(0). FLIP_DISCARD bỏ được một lần copy so với
//      mô hình bitblt cũ; Present(0) là không chờ VSync — thà xé hình còn hơn thêm
//      tới 16 ms độ trễ vào một ứng dụng tương tác.
//   3. OVERLAY BẰNG CHILD WINDOW THƯỜNG, không vẽ vào swapchain. DWM tự ghép chúng
//      lên trên. Cách này rẻ và không đụng gì tới đường video.
//   4. RENDERER KHÔNG BIẾT NGỮ NGHĨA INPUT. Nó chỉ chuyển tiếp message thô ra
//      ngoài qua MessageHook, và báo id nút vừa bấm qua CommandHook. Mọi hiểu biết
//      về khoá chuột / tạm dừng nằm ở InputCapture.
//
// ⚠ MÔ HÌNH LUỒNG — hai nhóm hàm, hai luồng khác nhau
//   Luồng đã gọi Init/Pump (main): Init, Pump, SetStatusText, SetToggleState,
//                                   RequestDumpBmp, Hwnd, ClientSize.
//   Luồng khác (chuỗi callback decode): RenderNV12.
//   RenderNV12 gọi được từ luồng khác vì device đã bật multithread-protected và
//   lớp này tự khoá renderMutex. Các hàm còn lại đụng tới cửa sổ Win32 nên PHẢI ở
//   đúng luồng tạo ra nó.
//
// LIÊN QUAN: decode/IVideoDecoder.h (nguồn frame), input/InputCapture.h (bên gắn
//            MessageHook), ClientLoop.cpp (người dùng), capture/BmpWriter.h
// =============================================================================
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
    static constexpr int kBtnLock = 1001;  // == F9 (khóa/thả chuột tương đối)
    static constexpr int kBtnPause = 1002; // == F10 (tạm dừng/tiếp tục gửi input)
    using CommandHook = std::function<void(int id)>;
    void SetCommandHook(CommandHook hook);

    // Dòng số liệu (fps/kbps/mất gói/RTT/e2e) — ghép vào THANH TIÊU ĐỀ cửa sổ,
    // sau tiêu đề gốc. Không che video, không đụng swapchain. Chuỗi rỗng = trả
    // về tiêu đề gốc. Chỉ gọi từ luồng đã Init/Pump.
    void SetStatusText(const wchar_t* text);

    // Đồng bộ trạng thái 2 nút với InputCapture khi người dùng đổi bằng phím tắt
    // thay vì click. Chỉ gọi từ luồng đã Init/Pump.
    void SetToggleState(bool locked, bool paused);

    // HWND cửa sổ preview (nullptr nếu chưa Init) - để đăng ký Raw Input.
    HWND Hwnd() const;

    // Kích thước vùng client (dùng để chuẩn hóa tọa độ chuột).
    void ClientSize(uint32_t& w, uint32_t& h) const;

    // Bơm message của LUỒNG - gọi lặp lại trên luồng đã Init. Static vì PeekMessage
    // không lọc theo HWND: một lần gọi phục vụ mọi cửa sổ preview trên luồng này
    // (GD6 mở nhiều cửa sổ cùng lúc).
    static void Pump();

    // True khi người dùng đóng cửa sổ preview.
    bool Closed() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
