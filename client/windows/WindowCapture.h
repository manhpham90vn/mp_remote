#pragma once
//
// WindowCapture - bat hinh mot cua so bang Windows Graphics Capture.
//
// Thiet ke:
//   - Hoat dong theo su kien FrameArrived (khong polling) de giam do tre va tai CPU.
//   - Giu winrt hoan toan trong file .cpp (PIMPL). Header nay chi lo D3D11/COM thuan,
//     nen EncoderModule tieu thu duoc ma khong keo theo winrt.
//   - D3D11 device duoc chia se ra ngoai qua Device()/Context() de encoder dung chung
//     texture, tranh copy cheo device.
//
#include "CaptureTypes.h"
#include <functional>
#include <memory>

namespace capture {

// Khoi tao runtime WinRT (MTA) cho luong hien tai. Goi mot lan luc khoi dong,
// truoc khi tao WindowCapture.
void InitRuntime();

}  // namespace capture

class WindowCapture {
public:
    // Callback duoc goi tren luong cua thread-pool WGC moi khi co frame moi.
    // Phai xu ly nhanh (encode/copy) va KHONG giu FrameInfo::texture sau khi tra ve.
    using FrameHandler = std::function<void(const FrameInfo&)>;

    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture&) = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    // Bat dau bat hinh cua so `hwnd`, goi `onFrame` cho moi frame.
    // `device`: D3D11 device dung chung (tu GpuSelect). Neu nullptr, tu tao device mac dinh.
    // Dung chung device voi encoder de texture khong phai copy cheo GPU.
    bool Start(HWND hwnd, ID3D11Device* device, FrameHandler onFrame);
    void Stop();

    // True khi cua so muc tieu da dong.
    bool Closed() const;

    // D3D11 device/context dung cho capture - chia se cho encoder (COM thuan).
    ID3D11Device*        Device() const;
    ID3D11DeviceContext* Context() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
