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
            std::printf("[Renderer] %s failed: 0x%08lX\n", (msg),                 \
                        (unsigned long)_hr);                                      \
            return false;                                                         \
        }                                                                         \
    } while (0)

namespace {
constexpr wchar_t kWndClass[] = L"LoopbackPreviewWnd";
}

struct Renderer::Impl {
    HWND                                  hwnd = nullptr;
    HWND                                  statusLabel = nullptr;
    HWND                                  btnLock = nullptr;
    HWND                                  btnPause = nullptr;
    HBRUSH                                labelBrush = nullptr; // nền dòng số liệu
    Renderer::CommandHook                 cmdHook;    // GD5: nút overlay -> bên ngoài
    ComPtr<ID3D11Device>                  device;
    ComPtr<ID3D11DeviceContext>           context;
    ComPtr<IDXGISwapChain1>               swapchain;
    ComPtr<ID3D11Texture2D>               backbuffer;
    ComPtr<ID3D11VideoDevice>             videoDevice;
    ComPtr<ID3D11VideoContext>            videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor>          vp;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    // Cache input view theo (texture, slice): pool decoder dùng lại vài texture.
    std::map<std::pair<ID3D11Texture2D*, UINT>,
             ComPtr<ID3D11VideoProcessorInputView>> inViews;
    std::mutex        renderMutex;   // RenderNV12 từ luồng decode vs hủy từ main
    std::atomic<bool> closed{ false };
    uint32_t vpSrcW = 0, vpSrcH = 0;    // kích thước nguồn mà VP hiện tại phục vụ
    uint32_t clientW = 0, clientH = 0;
    std::string dumpBmpPath;            // rỗng = không dump
    Renderer::MessageHook msgHook;      // chỉ đọc/ghi trên luồng message (main)

    ~Impl() {
        if (hwnd) DestroyWindow(hwnd); // hủy cả child (label + 2 nút)
        if (labelBrush) DeleteObject(labelBrush);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        // GD4: InputCapture xem message trước. Khi đang lái input đi xa, nó nuốt
        // cả phím/chuột (kể cả ESC) để người dùng gõ vào MÁY KIA, không phải đây.
        if (self && self->msgHook && self->msgHook(h, msg, wp, lp)) return 0;
        switch (msg) {
        case WM_CLOSE:
            if (self) self->closed.store(true);
            return 0;  // main tự đóng dẹp; không DestroyWindow ở đây
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && self) self->closed.store(true);
            return 0;
        case WM_CTLCOLORSTATIC:
            // Nền dòng số liệu: chữ sáng trên nền tối để đọc được trên video.
            if (self && (HWND)lp == self->statusLabel) {
                HDC hdc = (HDC)wp;
                SetTextColor(hdc, RGB(240, 240, 240));
                SetBkColor(hdc, RGB(20, 20, 20));
                return (LRESULT)self->labelBrush;
            }
            break;
        case WM_COMMAND:
            // GD5: 2 nút overlay (kBtnLock/kBtnPause) -> báo ra ngoài, giống hệt
            // đường phím tắt F9/F10. Renderer không tự đổi trạng thái nút - bên
            // ngoài gọi lại SetToggleState() sau khi xử lý xong.
            if (self && self->cmdHook && HIWORD(wp) == BN_CLICKED) {
                const int id = LOWORD(wp);
                if (id == Renderer::kBtnLock || id == Renderer::kBtnPause)
                    self->cmdHook(id);
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // GD5: dòng số liệu góc trên-trái + 2 nút khóa chuột/tạm dừng góc trên-phải,
    // đè lên trên video bằng child window thường - DWM tự ghép, không cần can
    // thiệp vào swapchain. Gọi sau khi `hwnd` + clientW/clientH đã có.
    void CreateOverlay() {
        labelBrush = CreateSolidBrush(RGB(20, 20, 20));
        const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        const int btnW = 130, btnH = 24, pad = 8;
        statusLabel = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            pad, pad, (int)clientW > 2 * (btnW + pad) + 400
                ? 400 : (int)clientW - 2 * (btnW + pad) - pad,
            btnH, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

        btnLock = CreateWindowExW(0, L"BUTTON", L"\U0001F512 Lock mouse (F9)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
            (int)clientW - pad - 2 * btnW - pad, pad, btnW, btnH,
            hwnd, (HMENU)(INT_PTR)Renderer::kBtnLock, GetModuleHandleW(nullptr), nullptr);

        btnPause = CreateWindowExW(0, L"BUTTON", L"⏸ Pause (F10)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_PUSHLIKE,
            (int)clientW - pad - btnW, pad, btnW, btnH,
            hwnd, (HMENU)(INT_PTR)Renderer::kBtnPause, GetModuleHandleW(nullptr), nullptr);

        for (HWND c : { statusLabel, btnLock, btnPause })
            if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    }

    bool Init(ID3D11Device* dev, uint32_t srcW, uint32_t srcH, const wchar_t* title) {
        device = dev;
        device->GetImmediateContext(&context);

        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt)))) {
            mt->SetMultithreadProtected(TRUE);
        }

        // Client = kích thước nguồn, thu nhỏ giữ tỷ lệ nếu vượt 90% vùng làm việc.
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const uint32_t maxW = (uint32_t)((wa.right - wa.left) * 9 / 10);
        const uint32_t maxH = (uint32_t)((wa.bottom - wa.top) * 9 / 10);
        clientW = srcW; clientH = srcH;
        if (clientW > maxW) { clientH = clientH * maxW / clientW; clientW = maxW; }
        if (clientH > maxH) { clientW = clientW * maxH / clientH; clientH = maxH; }
        if (!clientW || !clientH) { std::printf("[Renderer] Source size = 0.\n"); return false; }

        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWndClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);  // lần 2 trả ALREADY_EXISTS - không sao

        const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        RECT wr{ 0, 0, (LONG)clientW, (LONG)clientH };
        AdjustWindowRect(&wr, style, FALSE);
        hwnd = CreateWindowExW(0, kWndClass, title, style,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               wr.right - wr.left, wr.bottom - wr.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) { std::printf("[Renderer] CreateWindow failed.\n"); return false; }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        ShowWindow(hwnd, SW_SHOW);
        CreateOverlay();

        // Swapchain flip-model trên chính device dùng chung.
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

        std::printf("[Renderer] Preview window %ux%u (source %ux%u).\n",
                    clientW, clientH, srcW, srcH);
        return true;
    }

    // Tạo (lại) video processor cho kích thước nguồn `w x h`.
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

        // Chỉ lấy đúng vùng hình thật (texture decoder có thể align lớn hơn).
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
                std::printf("[Renderer] Saved backbuffer: %s\n", dumpBmpPath.c_str());
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

void Renderer::SetMessageHook(MessageHook hook) {
    if (impl_) impl_->msgHook = std::move(hook);
}

void Renderer::SetCommandHook(CommandHook hook) {
    if (impl_) impl_->cmdHook = std::move(hook);
}

void Renderer::SetStatusText(const wchar_t* text) {
    if (impl_ && impl_->statusLabel) SetWindowTextW(impl_->statusLabel, text);
}

void Renderer::SetToggleState(bool locked, bool paused) {
    if (!impl_) return;
    if (impl_->btnLock)
        SendMessageW(impl_->btnLock, BM_SETCHECK, locked ? BST_CHECKED : BST_UNCHECKED, 0);
    if (impl_->btnPause)
        SendMessageW(impl_->btnPause, BM_SETCHECK, paused ? BST_CHECKED : BST_UNCHECKED, 0);
}

HWND Renderer::Hwnd() const { return impl_ ? impl_->hwnd : nullptr; }

void Renderer::ClientSize(uint32_t& w, uint32_t& h) const {
    w = impl_ ? impl_->clientW : 0;
    h = impl_ ? impl_->clientH : 0;
}

void Renderer::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Renderer::Closed() const { return impl_ && impl_->closed.load(); }
