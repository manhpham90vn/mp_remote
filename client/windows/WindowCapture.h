#pragma once
//
// WindowCapture - bắt hình một cửa sổ bằng Windows Graphics Capture.
//
// Thiết kế:
//   - Hoạt động theo sự kiện FrameArrived (không polling) để giảm độ trễ và tải CPU.
//   - Giữ winrt hoàn toàn trong file .cpp (PIMPL). Header này chỉ lộ D3D11/COM thuần,
//     nên EncoderModule tiêu thụ được mà không kéo theo winrt.
//   - D3D11 device được chia sẻ ra ngoài qua Device()/Context() để encoder dùng chung
//     texture, tránh copy chéo device.
//
#include "CaptureTypes.h"
#include <functional>
#include <memory>

namespace capture {

// Khởi tạo runtime WinRT (MTA) cho luồng hiện tại. Gọi một lần lúc khởi động,
// trước khi tạo WindowCapture.
void InitRuntime();

}  // namespace capture

class WindowCapture {
public:
    // Callback được gọi trên luồng của thread-pool WGC mỗi khi có frame mới.
    // Phải xử lý nhanh (encode/copy) và KHÔNG giữ FrameInfo::texture sau khi trả về.
    using FrameHandler = std::function<void(const FrameInfo&)>;

    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    // Bắt đầu bắt hình cửa sổ `hwnd`, gọi `onFrame` cho mỗi frame.
    // `device`: D3D11 device dùng chung (từ GpuSelect). Nếu nullptr, tự tạo device mặc định.
    // Dùng chung device với encoder để texture không phải copy chéo GPU.
    bool Start(HWND hwnd, ID3D11Device* device, FrameHandler onFrame);
    void Stop();

    // True khi cửa sổ mục tiêu đã đóng.
    bool Closed() const;

    // D3D11 device/context dùng cho capture - chia sẻ cho encoder (COM thuần).
    ID3D11Device*        Device() const;
    ID3D11DeviceContext* Context() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
