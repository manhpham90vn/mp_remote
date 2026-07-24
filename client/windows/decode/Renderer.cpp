// =============================================================================
// Renderer.cpp — cài đặt cửa sổ preview, swapchain và đường vẽ.
//
// BỐ CỤC
//   WndProc()      — thủ tục cửa sổ; chuyển message cho msgHook TRƯỚC mọi thứ khác.
//   CreateOverlay()— dựng dòng số liệu + hai nút bằng child window.
//   Init()         — đăng ký lớp cửa sổ, tạo cửa sổ, swapchain, video processor.
//   RenderNV12()   — đường nóng: chuyển màu + Present.
//
// VÌ SAO msgHook ĐƯỢC XEM MESSAGE TRƯỚC
//   Khi InputCapture đang lái input sang máy kia, nó phải NUỐT sạch phím và chuột —
//   kể cả ESC — để người dùng gõ vào máy từ xa chứ không phải vào cửa sổ này. Trả
//   true nghĩa là "đã tiêu thụ", và WndProc dừng ngay tại đó.
//
// CACHE INPUT VIEW THEO CẶP (texture, slice)
//   Khác capture (khoá theo mỗi con trỏ texture), ở đây khoá phải là CẶP: decoder
//   dùng một texture-array chung, nên cùng một con trỏ texture ứng với nhiều frame
//   khác nhau, phân biệt bằng array slice. Khoá thiếu slice sẽ vẽ nhầm frame —
//   xem cảnh báo ở IVideoDecoder.h.
//
// ⚠ renderMutex BẢO VỆ CÁI GÌ
//   RenderNV12 chạy trên luồng decode, còn việc huỷ cửa sổ/swapchain xảy ra trên
//   luồng main. Không có khoá thì main có thể thả swapchain ngay giữa lúc luồng
//   decode đang vẽ vào nó.
//
// LIÊN QUAN: decode/Renderer.h (bốn quyết định thiết kế + mô hình luồng)
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "decode/Renderer.h"
#include "capture/BmpWriter.h"

#include <d3d11_1.h>
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

#define RND_CHECK(expr, msg)                                      \
    do {                                                          \
        HRESULT _hr = (expr);                                     \
        if (FAILED(_hr)) {                                        \
            std::printf("[Renderer] %s failed: 0x%08lX\n", (msg), \
                (unsigned long)_hr);                              \
            return false;                                         \
        }                                                         \
    } while (0)

namespace {
constexpr wchar_t kWndClass[] = L"LoopbackPreviewWnd";
constexpr wchar_t kVideoClass[] = L"LoopbackVideoHost";
constexpr wchar_t kOverlayClass[] = L"LoopbackOverlayWnd";

// Kích thước dải nút overlay — dùng chung giữa CreateOverlay và SyncOverlayPos.
constexpr int kBtnW = 130, kBtnH = 24, kBtnPad = 8;
constexpr int kOverlayW = 3 * kBtnW + 2 * kBtnPad;

// Thủ tục của child window chứa video. HTTRANSPARENT để mọi cú chuột "xuyên" qua
// nó rơi về cửa sổ cha — InputCapture móc vào WndProc của cha nên không được để
// child này nuốt mất input.
LRESULT CALLBACK VideoHostProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, msg, wp, lp);
}
} // namespace

struct Renderer::Impl {
    HWND hwnd = nullptr;
    HWND videoHwnd = nullptr;   // child chứa swapchain — xem ghi chú ở Init
    HWND overlayWnd = nullptr;  // popup OWNED chứa 3 nút — xem CreateOverlay
    HWND btnLock = nullptr;
    HWND btnPause = nullptr;
    HWND btnHotkey = nullptr;      // GĐ8: gửi Ctrl+Shift+Esc sang host
    std::wstring baseTitle;        // tiêu đề gốc — SetStatusText ghép số liệu vào sau
    Renderer::CommandHook cmdHook; // GD5: nút overlay -> bên ngoài
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID3D11Texture2D> backbuffer;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11VideoProcessorOutputView> outView;
    // Cache input view theo (texture, slice): pool decoder dùng lại vài texture.
    std::map<std::pair<ID3D11Texture2D*, UINT>,
        ComPtr<ID3D11VideoProcessorInputView>>
        inViews;
    std::mutex renderMutex; // RenderNV12 từ luồng decode vs hủy từ main
    std::atomic<bool> closed{false};
    uint32_t vpSrcW = 0, vpSrcH = 0; // kích thước nguồn mà VP hiện tại phục vụ
    uint32_t clientW = 0, clientH = 0;
    std::string dumpBmpPath;       // rỗng = không dump
    Renderer::MessageHook msgHook; // chỉ đọc/ghi trên luồng message (main)

    ~Impl() {
        if (hwnd) DestroyWindow(hwnd); // hủy cả child + overlay (owned window)
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        // GD4: InputCapture xem message trước. Khi đang lái input đi xa, nó nuốt
        // cả phím/chuột (kể cả ESC) để người dùng gõ vào MÁY KIA, không phải đây.
        if (self && self->msgHook && self->msgHook(h, msg, wp, lp)) return 0;
        switch (msg) {
            case WM_CLOSE:
                if (self) self->closed.store(true);
                return 0; // main tự đóng dẹp; không DestroyWindow ở đây
            case WM_KEYDOWN:
                if (wp == VK_ESCAPE && self) self->closed.store(true);
                return 0;
            case WM_WINDOWPOSCHANGED:
                // Cửa sổ vừa di chuyển/ẩn/hiện — kéo dải nút overlay theo.
                if (self) self->SyncOverlayPos();
                break; // vẫn để DefWindowProc sinh WM_MOVE/WM_SIZE như thường
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Thủ tục của dải nút overlay. MA_NOACTIVATE: bấm nút không được cướp focus
    // của cửa sổ preview — InputCapture gắn theo cửa sổ đang foreground, đổi focus
    // là nó rời đi và input ngừng gửi.
    static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Impl*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (msg) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_COMMAND:
                // Nút -> báo ra ngoài, giống hệt đường phím tắt F9/F10. Renderer
                // không tự đổi trạng thái nút - bên ngoài gọi lại SetToggleState().
                if (self && self->cmdHook && HIWORD(wp) == BN_CLICKED) {
                    const int id = LOWORD(wp);
                    if (id == Renderer::kBtnLock || id == Renderer::kBtnPause ||
                        id == Renderer::kBtnHotkey)
                        self->cmdHook(id);
                }
                return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Các nút KHÔNG là child của cửa sổ preview mà nằm trong một POPUP RIÊNG do
    // preview làm owner. Lý do: child GDI vẽ vào chung redirection surface với
    // cửa sổ cha, mà DWM ghép visual của swapchain ĐÈ lên surface đó — mọi chiêu
    // trong cùng cửa sổ (z-order, WS_EX_LAYERED, WS_CLIPSIBLINGS) đều đã thử và
    // thua (24/07/2026). Owned window là cửa sổ TOP-LEVEL có surface riêng, luôn
    // nằm trên owner theo luật z-order của Windows — không còn gì để tranh chấp.
    // Dòng số liệu không cần chiêu này: nó nằm trên thanh tiêu đề (SetStatusText).
    void CreateOverlay() {
        WNDCLASSW oc{};
        oc.lpfnWndProc = OverlayProc;
        oc.hInstance = GetModuleHandleW(nullptr);
        oc.lpszClassName = kOverlayClass;
        oc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        oc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&oc); // lần hai trở đi ALREADY_EXISTS — không sao

        // WS_EX_NOACTIVATE đi cùng MA_NOACTIVATE ở trên; WS_EX_TOOLWINDOW để dải
        // nút không có mặt trên taskbar/Alt-Tab như một "cửa sổ" thật.
        overlayWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kOverlayClass,
            L"", WS_POPUP, 0, 0, kOverlayW, kBtnH,
            hwnd /*owner*/, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!overlayWnd) return;
        SetWindowLongPtrW(overlayWnd, GWLP_USERDATA, (LONG_PTR)this);

        const HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mkBtn = [&](const wchar_t* text, DWORD style, int x, int id) {
            HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | style,
                x, 0, kBtnW, kBtnH, overlayWnd, (HMENU)(INT_PTR)id,
                GetModuleHandleW(nullptr), nullptr);
            if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };
        // GĐ8: nút gửi Ctrl+Shift+Esc — bấm tổ hợp thật thì Windows máy client
        // nuốt mất (mở Task Manager của chính mình) nên phải đi đường nút bấm.
        btnHotkey = mkBtn(L"⌨ Ctrl+Shift+Esc", BS_PUSHBUTTON, 0, Renderer::kBtnHotkey);
        btnLock = mkBtn(L"\U0001F512 Lock mouse (F9)", BS_AUTOCHECKBOX | BS_PUSHLIKE,
            kBtnW + kBtnPad, Renderer::kBtnLock);
        btnPause = mkBtn(L"⏸ Pause (F10)", BS_AUTOCHECKBOX | BS_PUSHLIKE,
            2 * (kBtnW + kBtnPad), Renderer::kBtnPause);
        SyncOverlayPos();
    }

    // Đặt dải nút bám góc trên-phải vùng client của preview; preview ẩn/thu nhỏ
    // thì dải nút ẩn theo. Gọi từ CreateOverlay và mỗi WM_WINDOWPOSCHANGED.
    void SyncOverlayPos() {
        if (!overlayWnd) return;
        if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) {
            ShowWindow(overlayWnd, SW_HIDE);
            return;
        }
        POINT tr{(LONG)clientW - kBtnPad - kOverlayW, kBtnPad};
        ClientToScreen(hwnd, &tr);
        SetWindowPos(overlayWnd, HWND_TOP, tr.x, tr.y, kOverlayW, kBtnH,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
        clientW = srcW;
        clientH = srcH;
        if (clientW > maxW) {
            clientH = clientH * maxW / clientW;
            clientW = maxW;
        }
        if (clientH > maxH) {
            clientW = clientW * maxH / clientH;
            clientH = maxH;
        }
        if (!clientW || !clientH) {
            std::printf("[Renderer] Source size = 0.\n");
            return false;
        }

        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWndClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // GĐ6 mở nhiều cửa sổ preview cùng lúc nên hàm này chạy nhiều lần; lần thứ
        // hai trở đi RegisterClassW trả ALREADY_EXISTS. Không sao, lớp đã có sẵn.
        RegisterClassW(&wc);

        // Bỏ THICKFRAME và MAXIMIZEBOX: swapchain tạo theo kích thước cố định, cho
        // người dùng kéo giãn thì phải xử lý cả đường ResizeBuffers — chưa cần.
        const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        // AdjustWindowRect đổi kích thước VÙNG CLIENT mong muốn thành kích thước cả
        // cửa sổ (cộng viền và thanh tiêu đề). Không có bước này thì video bị viền
        // ăn mất một dải.
        RECT wr{0, 0, (LONG)clientW, (LONG)clientH};
        AdjustWindowRect(&wr, style, FALSE);
        hwnd = CreateWindowExW(0, kWndClass, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            std::printf("[Renderer] CreateWindow failed.\n");
            return false;
        }
        baseTitle = title ? title : L"";
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);

        // Swapchain flip-model KHÔNG được gắn thẳng vào cửa sổ cha: DWM ghép visual
        // của swapchain ĐÈ LÊN redirection surface chứa các GDI child (dòng số liệu,
        // hai nút) — chúng vẫn "visible" mà không bao giờ hiện ra. Tách video vào
        // một child riêng, đẩy xuống đáy z-order; label/nút là sibling nằm trên nên
        // DWM ghép đúng thứ tự.
        WNDCLASSW vc{};
        vc.lpfnWndProc = VideoHostProc;
        vc.hInstance = wc.hInstance;
        vc.lpszClassName = kVideoClass;
        RegisterClassW(&vc); // lần hai trở đi trả ALREADY_EXISTS — không sao
        videoHwnd = CreateWindowExW(0, kVideoClass, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, (int)clientW, (int)clientH, hwnd, nullptr, wc.hInstance, nullptr);
        if (!videoHwnd) {
            std::printf("[Renderer] CreateWindow (video host) failed.\n");
            return false;
        }
        SetWindowPos(videoHwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

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
        sd.BufferCount = 2; // tối thiểu của flip-model; nhiều hơn chỉ thêm độ trễ
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        RND_CHECK(factory->CreateSwapChainForHwnd(device.Get(), videoHwnd, &sd, nullptr,
                      nullptr, &swapchain),
            "CreateSwapChainForHwnd");
        RND_CHECK(swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "GetBuffer");

        RND_CHECK(device.As(&videoDevice), "ID3D11VideoDevice");
        RND_CHECK(context.As(&videoContext), "ID3D11VideoContext");

        std::printf("[Renderer] Preview window %ux%u (source %ux%u).\n",
            clientW, clientH, srcW, srcH);
        return true;
    }

    // Tạo (lại) video processor cho kích thước nguồn `w x h`.
    //
    // Video processor gắn chặt với kích thước nguồn, nên host đổi độ phân giải giữa
    // chừng là phải dựng lại. Thoát sớm khi kích thước không đổi — đây là đường
    // nóng, chạy mỗi frame.
    bool EnsureVideoProcessor(uint32_t w, uint32_t h) {
        if (vp && w == vpSrcW && h == vpSrcH) return true;
        // Các input view cũ gắn với enumerator cũ nên không dùng lại được.
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

        // Cắt đúng vùng hình THẬT. Decoder căn texture theo bội số macroblock nên nó
        // thường lớn hơn kích thước hiển thị; không đặt source rect thì phần đệm
        // (rác) cũng bị co giãn lên màn hình.
        RECT src{0, 0, (LONG)w, (LONG)h};
        videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &src);
        RECT dst{0, 0, (LONG)clientW, (LONG)clientH};
        videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dst);

        // Khai TƯỜNG MINH color space cho phép chuyển NV12→BGRA: vào là YUV BT.709
        // limited (đúng thứ MfEncoder phát), ra là RGB full range cho swapchain.
        // Bỏ trống thì driver tự đoán và có thể đoán khác đầu encode — màu trôi
        // cả khung (đen bị nâng, sáng bị cắt trần).
        //
        // Driver đời mới bỏ qua struct legacy, chỉ tôn trọng đường ColorSpace1
        // (đo thực tế 24/07/2026, xem ghi chú tương ứng ở MfEncoder) — gọi cả hai.
        ComPtr<ID3D11VideoContext1> vc1;
        if (SUCCEEDED(videoContext.As(&vc1))) {
            vc1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0,
                DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            vc1->VideoProcessorSetOutputColorSpace1(vp.Get(),
                DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
        inCs.YCbCr_Matrix = 1; // 1 = BT.709
        inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &inCs);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
        outCs.RGB_Range = 0; // 0 = full 0-255
        videoContext->VideoProcessorSetOutputColorSpace(vp.Get(), &outCs);

        // TẮT "video enhancement" tự động của driver — cùng lý do với MfEncoder
        // (Intel ACE tăng sáng nội dung tối, ngoài tầm mọi khai báo color space).
        videoContext->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);

        vpSrcW = w;
        vpSrcH = h;
        return true;
    }

    bool RenderNV12(ID3D11Texture2D* tex, UINT subresource, uint32_t w, uint32_t h) {
        std::lock_guard<std::mutex> lk(renderMutex);
        if (closed.load() || !swapchain) return false;
        if (!EnsureVideoProcessor(w, h)) return false;

        // Khoá là CẶP (texture, slice), không phải riêng con trỏ texture — xem ghi
        // chú ở đầu file về texture-array của decoder.
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

        // Present(0) = không chờ VSync. Chấp nhận xé hình để bỏ được tới ~16 ms độ
        // trễ — đánh đổi đúng cho ứng dụng tương tác.
        RND_CHECK(swapchain->Present(0, 0), "Present");
        return true;
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Init(ID3D11Device* device, uint32_t srcW, uint32_t srcH, const wchar_t* title) {
    impl_ = std::make_unique<Impl>();
    if (!impl_->Init(device, srcW, srcH, title)) {
        impl_.reset();
        return false;
    }
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
    if (!impl_ || !impl_->hwnd) return;
    // Ghép số liệu vào THANH TIÊU ĐỀ thay vì vẽ đè lên video: không che hình,
    // không đụng swapchain, và thấy được cả khi cửa sổ không focus.
    if (!text || !*text) {
        SetWindowTextW(impl_->hwnd, impl_->baseTitle.c_str());
        return;
    }
    const std::wstring t = impl_->baseTitle + L"   —   " + text;
    SetWindowTextW(impl_->hwnd, t.c_str());
}

void Renderer::SetToggleState(bool locked, bool paused) {
    if (!impl_) return;
    if (impl_->btnLock)
        SendMessageW(impl_->btnLock, BM_SETCHECK, locked ? BST_CHECKED : BST_UNCHECKED, 0);
    if (impl_->btnPause)
        SendMessageW(impl_->btnPause, BM_SETCHECK, paused ? BST_CHECKED : BST_UNCHECKED, 0);
}

HWND Renderer::Hwnd() const {
    return impl_ ? impl_->hwnd : nullptr;
}

void Renderer::ClientSize(uint32_t& w, uint32_t& h) const {
    w = impl_ ? impl_->clientW : 0;
    h = impl_ ? impl_->clientH : 0;
}

// Bơm message của cả LUỒNG, không riêng cửa sổ nào — đó là lý do hàm này static.
// PeekMessage với hwnd = nullptr lấy message của mọi cửa sổ trên luồng hiện tại,
// nên một lời gọi phục vụ hết các cửa sổ preview đang mở (GĐ6 mở nhiều nguồn).
void Renderer::Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool Renderer::Closed() const {
    return impl_ && impl_->closed.load();
}
