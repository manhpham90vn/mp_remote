// WindowCapture - hiện thực dùng Windows Graphics Capture (winrt nằm trọn trong đây).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "WindowCapture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Hai header interop là cầu nối giữa thế giới WinRT và COM/D3D11 cũ.
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <cstdio>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace wg = winrt::Windows::Graphics;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3 = winrt::Windows::Graphics::DirectX::Direct3D11;
using winrt::Windows::Foundation::Metadata::ApiInformation;

namespace capture {
void InitRuntime() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}
}  // namespace capture

// Lấy interface D3D11 gốc ra khỏi một đối tượng WinRT (surface -> ID3D11Texture2D).
template <typename T>
static winrt::com_ptr<T> GetDXGIInterface(winrt::Windows::Foundation::IInspectable const& obj) {
    auto access = obj.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
    return result;
}

// ---------------------------------------------------------------------------
struct WindowCapture::Impl {
    winrt::com_ptr<ID3D11Device>        d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;

    wgd3::IDirect3DDevice               winrtDevice{ nullptr };
    wgc::GraphicsCaptureItem            item{ nullptr };
    wgc::Direct3D11CaptureFramePool     framePool{ nullptr };
    wgc::GraphicsCaptureSession         session{ nullptr };
    winrt::event_token                  closedToken{};
    winrt::event_token                  frameArrivedToken{};
    wg::SizeInt32                       lastSize{};

    FrameHandler                        onFrame;
    std::atomic<bool>                   closed{ false };
    std::atomic<uint64_t>               frameId{ 0 };

    bool CreateDevice() {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
        };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            d3dDevice.put(), nullptr, d3dContext.put());
        if (FAILED(hr)) {
            std::printf("D3D11CreateDevice failed: 0x%08lX\n", (unsigned long)hr);
            return false;
        }
        return true;
    }

    // Chạy trên luồng thread-pool của WGC. Rút hết frame đang chờ, xử lý từng cái.
    void OnFrameArrived() {
        if (!framePool) return;

        while (auto frame = framePool.TryGetNextFrame()) {
            auto size = frame.ContentSize();

            // Cửa sổ đổi kích thước -> tạo lại pool, bỏ frame này (frame sau sẽ dùng).
            if (size.Width != lastSize.Width || size.Height != lastSize.Height) {
                std::printf("Window size changed: %dx%d\n", size.Width, size.Height);
                lastSize = size;
                framePool.Recreate(
                    winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                continue;
            }

            auto tex = GetDXGIInterface<ID3D11Texture2D>(frame.Surface());
            if (!tex) continue;

            if (onFrame) {
                FrameInfo info{};
                info.texture = tex.get();
                info.width = static_cast<uint32_t>(size.Width);
                info.height = static_cast<uint32_t>(size.Height);
                // SystemRelativeTime: đơn vị 100ns -> micro giây.
                info.timestampUs =
                    static_cast<uint64_t>(frame.SystemRelativeTime().count()) / 10ull;
                info.frameId = frameId.fetch_add(1);
                onFrame(info);
            }
            // `frame` và `tex` giải phóng ở cuối vòng lặp -> texture hết hiệu lực.
        }
    }
};

// ---------------------------------------------------------------------------
WindowCapture::WindowCapture() : impl_(std::make_unique<Impl>()) {}

WindowCapture::~WindowCapture() { Stop(); }

bool WindowCapture::Start(HWND hwnd, ID3D11Device* device, FrameHandler onFrame) {
    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        std::printf("Windows Graphics Capture is not supported on this machine.\n");
        return false;
    }

    impl_->onFrame = std::move(onFrame);

    // 1. D3D11 device. Dùng device chia sẻ (từ GpuSelect) nếu có, không thì tự tạo.
    if (device) {
        impl_->d3dDevice.copy_from(device);
        impl_->d3dDevice->GetImmediateContext(impl_->d3dContext.put());
    }
    else if (!impl_->CreateDevice()) {
        return false;
    }

    // 2. Bọc D3D11 device thành IDirect3DDevice của WinRT.
    auto dxgiDevice = impl_->d3dDevice.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    impl_->winrtDevice = inspectable.as<wgd3::IDirect3DDevice>();

    // 3. Tạo GraphicsCaptureItem từ HWND (chỉ làm được qua interop).
    auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    HRESULT hr = interop->CreateForWindow(
        hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(impl_->item));
    if (FAILED(hr)) {
        std::printf("CreateForWindow failed: 0x%08lX\n", (unsigned long)hr);
        return false;
    }

    impl_->lastSize = impl_->item.Size();
    std::printf("Window size: %dx%d\n", impl_->lastSize.Width, impl_->lastSize.Height);

    // 4. Frame pool (free-threaded -> FrameArrived chạy trên luồng thread-pool).
    impl_->framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, impl_->lastSize);

    impl_->session = impl_->framePool.CreateCaptureSession(impl_->item);

    // 5. Tắt con trỏ chuột (1903+) và viền vàng (2004+) nếu API có.
    if (ApiInformation::IsPropertyPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
        impl_->session.IsCursorCaptureEnabled(false);
    }
    if (ApiInformation::IsPropertyPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        try {
            impl_->session.IsBorderRequired(false);
        }
        catch (winrt::hresult_error const&) {
            // Một số cấu hình đòi quyền riêng - không sao, chỉ là còn viền vàng.
        }
    }

    // 6. Đăng ký sự kiện: cửa sổ đóng, và có frame mới.
    impl_->closedToken = impl_->item.Closed(
        [impl = impl_.get()](auto&&, auto&&) { impl->closed = true; });
    impl_->frameArrivedToken = impl_->framePool.FrameArrived(
        [impl = impl_.get()](auto&&, auto&&) { impl->OnFrameArrived(); });

    impl_->session.StartCapture();
    std::printf("Started capturing (via FrameArrived event).\n");
    return true;
}

void WindowCapture::Stop() {
    if (!impl_) return;
    if (impl_->framePool && impl_->frameArrivedToken.value) {
        impl_->framePool.FrameArrived(impl_->frameArrivedToken);
        impl_->frameArrivedToken = {};
    }
    if (impl_->item && impl_->closedToken.value) {
        impl_->item.Closed(impl_->closedToken);
        impl_->closedToken = {};
    }
    impl_->session = nullptr;
    impl_->framePool = nullptr;
    impl_->item = nullptr;
    impl_->winrtDevice = nullptr;
    impl_->d3dContext = nullptr;
    impl_->d3dDevice = nullptr;
    impl_->onFrame = nullptr;
}

bool WindowCapture::Closed() const { return impl_->closed; }

ID3D11Device*        WindowCapture::Device() const { return impl_->d3dDevice.get(); }
ID3D11DeviceContext* WindowCapture::Context() const { return impl_->d3dContext.get(); }
