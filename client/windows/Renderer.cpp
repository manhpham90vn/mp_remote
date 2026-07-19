#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Renderer.h"
#include "BmpWriter.h"

#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

#define RND_CHECK(expr, msg)                                                      \
    do {                                                                          \
        HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                        \
            std::printf("[Renderer] %s that bai: 0x%08lX\n", (msg),               \
                        (unsigned long)_hr);                                      \
            return false;                                                         \
        }                                                                         \
    } while (0)

namespace {
constexpr wchar_t kWndClass[] = L"LoopbackPreviewWnd";
}

struct Renderer::Impl {
    HWND                                  hwnd = nullptr;
    ComPtr<ID3D11Device>                  device;
    ComPtr<ID3D11DeviceContext>           context;
    ComPtr<IDXGISwapChain1>               swapchain;
    ComPtr<ID3D11Texture2D>               backbuffer;
    ComPtr<ID3D11VideoDevice>             videoDevice;
    ComPtr<ID3D11VideoContext>            videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor>          vp;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    // Cache input view theo (texture, slice): pool decoder dung lai vai texture.
    std::map<std::pair<ID3D11Texture2D*, UINT>,
             ComPtr<ID3D11VideoProcessorInputView>> inViews;
    std::mutex        renderMutex;   // RenderNV12 tu luong decode vs huy tu main
    std::atomic<bool> closed{ false };
    uint32_t vpSrcW = 0, vpSrcH = 0;    // kich thuoc nguon ma VP hien tai phuc vu
    uint32_t clientW = 0, clientH = 0;
    std::string dumpBmpPath;            // rong = khong dump

    ~Impl() {
        if (hwnd) DestroyWindow(hwnd);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (msg) {
        case WM_CLOSE:
            if (self) self->closed.store(true);
            return 0;  // main tu dong dep; khong DestroyWindow o day
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && self) self->closed.store(true);
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    bool Init(ID3D11Device* dev, uint32_t srcW, uint32_t srcH, const wchar_t* title) {
        device = dev;
        device->GetImmediateContext(&context);

        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }

        // Client = kich thuoc nguon, thu nho giu ty le neu vuot 90% vung lam viec.
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const uint32_t maxW = (uint32_t)((wa.right - wa.left) * 9 / 10);
        const uint32_t maxH = (uint32_t)((wa.bottom - wa.top) * 9 / 10);
        clientW = srcW; clientH = srcH;
        if (clientW > maxW) { clientH = clientH * maxW / clientW; clientW = maxW; }
        if (clientH > maxH) { clientW = clientW * maxH / clientH; clientH = maxH; }
        if (!clientW || !clientH) { std::printf("[Renderer] Kich thuoc nguon = 0.\n"); return false; }

        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWndClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);  // lan 2 tra ALREADY_EXISTS - khong sao

        const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        RECT wr{ 0, 0, (LONG)clientW, (LONG)clientH };
        AdjustWindowRect(&wr, style, FALSE);
        hwnd = CreateWindowExW(0, kWndClass, title, style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               wr.right - wr.left, wr.bottom - wr.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) { std::printf("[Renderer] CreateWindow that bai.\n"); return false; }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        ShowWindow(hwnd, SW_SHOW);

        // Swapchain flip-model tren chinh device dung chung.
        ComPtr<IDXGIDevice> dxgiDev;
        RND_CHECK(device.As(&dxgiDev), "IDXGIDevice");
        ComPtr<IDXGIAdapter> adapter;
        RND_CHECK(dxgiDev->GetAdapter(&adapter), "GetAdapter");
        ComPtr<IDXGIFactory2> factory;
        RND_CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent(Factory2)");

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = clientW;
        sd.Height = clientH;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        RND_CHECK(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr,
                                                  &swapchain),
                  "CreateSwapChainForHwnd");
        RND_CHECK(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "GetBuffer");

        RND_CHECK(device.As(&videoDevice), "ID3D11VideoDevice");
        RND_CHECK(context.As(&videoContext), "ID3D11VideoContext");

        std::printf("[Renderer] Cua so preview %ux%u (nguon %ux%u).\n",
                    clientW, clientH, srcW, srcH);
        return true;
    }

    // Tao (lai) video processor cho kich thuoc nguon `w x h`.
    bool EnsureVideoProcessor(uint32_t w, uint32_t h) {
        if (vp && w == vpSrcW && h == vpSrcH) return true;
        inViews.clear();
        outView.Reset();
        vp.Reset();
        vpEnum.Reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = w;
        cd.InputHeight = h;
        cd.OutputWidth = clientW;
        cd.OutputHeight = clientH;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        RND_CHECK(videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum),
                  "CreateVideoProcessorEnumerator");
        RND_CHECK(videoDevice->CreateVideoProcessor(vpEnum.Get(), 0, &vp),
                  "CreateVideoProcessor");

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        od.Texture2D.MipSlice = 0;
        RND_CHECK(videoDevice->CreateVideoProcessorOutputView(backbuffer.Get(), vpEnum.Get(),
                                                              &od, &outView),
                  "CreateVideoProcessorOutputView");

        // Chi lay dung vung hinh that (texture decoder co the align lon hon).
        RECT src{ 0, 0, (LONG)w, (LONG)h };
        videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        RECT dst{ 0, 0, (LONG)clientW, (LONG)clientH };
        videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);

        vpSrcW = w;
        vpSrcH = h;
        return true;
    }

    bool RenderNV12(ID3D11Texture2D* tex, UINT subresource, uint32_t w, uint32_t h) {
        std::lock_guard<std::mutex> lk(renderMutex);
        if (closed.load() || !swapchain) return false;
        if (!EnsureVideoProcessor(w, h)) return false;

        auto key = std::make_pair(tex, subresource);
        auto it = inViews.find(key);
        if (it == inViews.end()) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC vd{};
            vd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipSlice = 0;
            vd.Texture2D.ArraySlice = subresource;
            ComPtr<ID3D11VideoProcessorInputView> view;
            RND_CHECK(videoDevice->CreateVideoProcessorInputView(tex, vpEnum.Get(), &vd, &view),
                      "CreateVideoProcessorInputView");
            it = inViews.emplace(key, std::move(view)).first;
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = it->second.Get();
        RND_CHECK(videoContext->VideoProcessorBlt(vp.Get(), outView.Get(), 0, 1, &stream),
                  "VideoProcessorBlt");

        if (!dumpBmpPath.empty()) {
            if (SaveTextureToBmp(device.Get(), context.Get(), backbuffer.Get(), dumpBmpPath))
                std::printf("[Renderer] Da luu backbuffer: %s\n", dumpBmpPath.c_str());
            dumpBmpPath.clear();
        }

        RND_CHECK(swapchain->Present(0, 0), "Present");
        return true;
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Init(ID3D11Device* device, uint32_t srcW, uint32_t srcH, const wchar_t* title) {
    impl_ = std::make_unique<Impl>();
    if (!impl_->Init(device, srcW, srcH, title)) { impl_.reset(); return false; }
    return true;
}

bool Renderer::RenderNV12(ID3D11Texture2D* tex, UINT subresource,
                          uint32_t width, uint32_t height) {
    return impl_ && impl_->RenderNV12(tex, subresource, width, height);
}

void Renderer::RequestDumpBmp(const std::string& path) {
    if (impl_) {
        std::lock_guard<std::mutex> lk(impl_->renderMutex);
        impl_->dumpBmpPath = path;
    }
}

void Renderer::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Renderer::Closed() const { return impl_ && impl_->closed.load(); }
