// Agent - Giai doan 0+1+2: bat hinh cua so game (WGC), nen hardware, va loopback
// giai ma + hien thi de kiem chung ca chuoi codec truoc khi co mang.
//
// Luong thuong (--encode): capture -> encoder -> file (.h264/.mp4).
// Luong loopback (--loopback): capture -> NVENC (NAL Annex-B, khong file)
//     -> MfDecoder (D3D11VA, NV12 VRAM) -> Renderer (VideoProcessor -> swapchain).
//     Tat ca trong CUNG process, cung D3D11 device - chua co bien so mang (GD3).
//
// Build: mo ConsoleApplication1.slnx trong Visual Studio, cau hinh x64.
// Chay:  ConsoleApplication1.exe [game.exe] [--encode|--loopback] [--out FILE]
//                                [--bitrate Mbps] [--fps N] [--frames N] [--save]
// Khong truyen [game.exe] -> hien menu liet ke cua so dang mo de chon nguon stream,
// chon xong tu dong vao che do loopback (tru khi truyen --encode).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <atomic>
#include <clocale>
#include <cstdio>
#include <mutex>
#include <string>

#include "WindowCapture.h"
#include "WindowFinder.h"
#include "BmpWriter.h"
#include "GpuSelect.h"
#include "IVideoEncoder.h"
#include "IVideoDecoder.h"
#include "Renderer.h"

// Dong ho don dieu (micro giay) de do do tre end-to-end trong loopback.
static uint64_t QpcUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / freq.QuadPart) * 1'000'000ull +
           (uint64_t)(c.QuadPart % freq.QuadPart) * 1'000'000ull / (uint64_t)freq.QuadPart;
}

// Menu console: liet ke cua so dang mo, cho nguoi dung chon nguon stream.
// Tra ve nullptr neu nguoi dung thoat (q) hoac khong con cua so nao.
static HWND PickWindowFromConsole() {
    for (;;) {
        auto windows = ListCapturableWindows();
        if (windows.empty()) {
            std::printf("Khong tim thay cua so nao de stream.\n");
            return nullptr;
        }
        std::printf("\nChon cua so de stream:\n");
        for (size_t i = 0; i < windows.size(); ++i) {
            if (windows[i].minimized) {
                std::wprintf(L"  %2u. [%ls] %ls (thu nho)\n", (unsigned)(i + 1),
                    windows[i].exeName.c_str(), windows[i].title.c_str());
            } else {
                std::wprintf(L"  %2u. [%ls] %ls (%ux%u)\n", (unsigned)(i + 1),
                    windows[i].exeName.c_str(), windows[i].title.c_str(),
                    windows[i].width, windows[i].height);
            }
        }
        std::printf("Nhap so (r = quet lai, q = thoat): ");
        char line[64] = {};
        if (!std::fgets(line, sizeof(line), stdin)) return nullptr;
        if (line[0] == 'q' || line[0] == 'Q') return nullptr;
        if (line[0] == 'r' || line[0] == 'R') continue;
        const int sel = std::atoi(line);
        if (sel >= 1 && (size_t)sel <= windows.size()) {
            if (IsWindow(windows[sel - 1].hwnd)) return windows[sel - 1].hwnd;
            std::printf("Cua so do vua dong, quet lai...\n");
            continue;
        }
        std::printf("Lua chon khong hop le.\n");
    }
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // UTF-8 cho console de wprintf in dung tieu de cua so co dau (tieng Viet...).
    std::setlocale(LC_ALL, ".UTF8");
    SetConsoleOutputCP(CP_UTF8);
    capture::InitRuntime();

    // --- Tham so dong lenh ---
    std::wstring targetExe;   // rong -> hien menu chon cua so
    std::wstring outPath = L"output.mp4";
    bool     saveBmp = false;
    bool     doEncode = false;
    bool     doLoopback = false;
    bool     framesSet = false;
    int      targetFrames = 120;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 20;

    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        auto next = [&](uint32_t def) -> uint32_t {
            if (i + 1 < argc) { int v = std::atoi(argv[++i]); return v > 0 ? (uint32_t)v : def; }
            return def;
        };
        if (a == "--save")          saveBmp = true;
        else if (a == "--encode")   doEncode = true;
        else if (a == "--loopback") doLoopback = true;
        else if (a == "--frames") { targetFrames = (int)next(120); framesSet = true; }
        else if (a == "--fps")      fps = next(60);
        else if (a == "--bitrate")  bitrateMbps = next(20);
        else if (a == "--out" && i + 1 < argc) {
            std::string s(argv[++i]); outPath.assign(s.begin(), s.end());
        }
        else if (!a.empty() && a[0] != '-') targetExe.assign(a.begin(), a.end());
    }

    // --- Chon GPU theo chuoi uu tien: NVIDIA -> Intel -> CPU (WARP) ---
    GpuChoice gpu;
    if (!CreateBestDevice({ GpuVendor::Nvidia, GpuVendor::Intel, GpuVendor::Amd }, gpu)) {
        std::printf("Khong tao duoc D3D11 device tren GPU nao.\n");
        return 1;
    }
    std::wprintf(L"GPU da chon: %ls [%ls]%ls\n", gpu.description.c_str(),
        GpuVendorName(gpu.vendor), gpu.hardware ? L"" : L" (software)");

    // --- Tim cua so game: theo ten exe neu duoc truyen, khong thi cho chon tu menu ---
    HWND target = nullptr;
    if (targetExe.empty()) {
        target = PickWindowFromConsole();
        if (!target) return 0;   // nguoi dung thoat
        // Chon tu menu -> mac dinh vao loopback luon (tru khi da chi dinh --encode).
        if (!doEncode) doLoopback = true;
    } else {
        target = FindWindowByProcessName(targetExe);
        if (!target) {
            std::wprintf(L"Khong tim thay cua so cua %ls. Hay chac chan game dang chay.\n",
                targetExe.c_str());
            return 1;
        }
    }
    // Cua so minimize thi WGC khong nhan duoc frame - restore truoc khi capture,
    // cho het animation (client rect cua cua so iconic chi ~144x20).
    if (IsIconic(target)) {
        std::printf("Cua so dang thu nho - dang restore...\n");
        ShowWindow(target, SW_RESTORE);
        for (int i = 0; i < 100 && IsIconic(target); ++i) Sleep(10);
        if (IsIconic(target)) {
            std::printf("Khong restore duoc cua so - dung.\n");
            return 1;
        }
        Sleep(300);  // doi animation restore xong de frame dau khong meo kich thuoc
    }

    wchar_t title[256] = {};
    GetWindowTextW(target, title, 256);
    std::wprintf(L"Da bam vao cua so: \"%ls\"\n", title);

    // --- Trang thai dung chung giua main va cac callback ---
    // Thu tu khai bao = nguoc thu tu huy: encoder huy truoc decoder, decoder truoc renderer
    // (onPacket tham chieu decoder, onDecoded tham chieu renderer).
    WindowCapture capture;
    Renderer      renderer;
    std::unique_ptr<IVideoDecoder> decoder;

    std::atomic<int>      captured{ 0 };
    std::atomic<bool>     saved{ false };
    std::atomic<uint32_t> srcW{ 0 }, srcH{ 0 };    // kich thuoc frame dau (loopback)
    std::atomic<bool>     rendererReady{ false };
    std::atomic<bool>     pipelineFailed{ false };
    std::atomic<uint64_t> rendered{ 0 };
    std::atomic<uint64_t> latSumUs{ 0 }, latMaxUs{ 0 };

    std::mutex                      encMutex;   // bao ve encoder giua callback va main
    std::unique_ptr<IVideoEncoder>  encoder;    // khoi tao lazy o frame dau (biet kich thuoc)

    // Frame giai ma xong -> ve len preview + cong don do tre end-to-end.
    auto onDecoded = [&](const DecodedFrame& df) {
        if (!renderer.RenderNV12(df.texture, df.subresource, df.width, df.height)) return;
        const uint64_t now = QpcUs();
        const uint64_t lat = now > df.timestampUs ? now - df.timestampUs : 0;
        latSumUs.fetch_add(lat);
        uint64_t prevMax = latMaxUs.load();
        while (lat > prevMax && !latMaxUs.compare_exchange_weak(prevMax, lat)) {}
        const uint64_t n = rendered.fetch_add(1) + 1;
        if (n <= 3 || n % 60 == 0) {
            std::printf("[Loopback] frame %llu: tre capture->hien thi %.1f ms (max %.1f ms)\n",
                (unsigned long long)n, lat / 1000.0, latMaxUs.load() / 1000.0);
        }
    };

    // NAL tu NVENC -> decoder (cung process; GD3 se la UDP o giua).
    auto onPacket = [&](const uint8_t* data, size_t size, uint64_t tsUs, bool /*keyframe*/) {
        if (!decoder) {
            DecoderConfig dc;
            dc.codec = Codec::H264;
            dc.width = srcW.load();
            dc.height = srcH.load();
            dc.fps = fps;
            decoder = CreateDecoder(gpu.device.Get(), dc, onDecoded);
            if (!decoder) { pipelineFailed.store(true); return; }
        }
        if (!decoder->Decode(data, size, tsUs)) pipelineFailed.store(true);
    };

    // Callback chay tren luong thread-pool cua WGC. Giu that nhe.
    auto onFrame = [&](const FrameInfo& fi) {
        captured.fetch_add(1);

        if (doLoopback) {
            // Doi main tao xong cua so preview (phai o main de bom message); toi luc do
            // moi biet kich thuoc frame -> vai frame dau bi bo qua, khong sao.
            if (!srcW.load()) { srcW.store(fi.width); srcH.store(fi.height); }
            if (!rendererReady.load() || pipelineFailed.load()) return;

            std::lock_guard<std::mutex> lk(encMutex);
            if (!encoder) {
                EncoderConfig cfg;
                cfg.width = fi.width;
                cfg.height = fi.height;
                cfg.fps = fps;
                cfg.bitrateBps = bitrateMbps * 1'000'000u;
                cfg.outputPath.clear();          // khong file - chi callback
                cfg.onPacket = onPacket;
                encoder = CreateEncoder(gpu.device.Get(), cfg);
                if (!encoder) {
                    std::printf("[Loopback] Can NVENC (MF chua xuat NAL) - dung.\n");
                    pipelineFailed.store(true);
                    return;
                }
            }
            // Dung QPC lam timestamp de do do tre end-to-end o onDecoded.
            if (encoder) encoder->Encode(fi.texture, QpcUs(), false);
        } else if (doEncode) {
            std::lock_guard<std::mutex> lk(encMutex);
            if (!encoder) {
                EncoderConfig cfg;
                cfg.width = fi.width;
                cfg.height = fi.height;
                cfg.fps = fps;
                cfg.bitrateBps = bitrateMbps * 1'000'000u;
                cfg.outputPath = outPath;
                encoder = CreateEncoder(gpu.device.Get(), cfg);
            }
            if (encoder) encoder->Encode(fi.texture, fi.timestampUs, false);
        }

        // Duong DEBUG (cham) - chi chay 1 lan de kiem chung bang mat thuong.
        if (saveBmp && !saved.exchange(true)) {
            if (SaveTextureToBmp(capture.Device(), capture.Context(), fi.texture, "window.bmp"))
                std::printf("Da luu window.bmp (%ux%u)\n", fi.width, fi.height);
        }
    };

    if (!capture.Start(target, gpu.device.Get(), onFrame)) return 1;

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);

    if (doLoopback) {
        // --- Vong lap loopback: tao preview khi biet kich thuoc, bom message, cho dong ---
        std::printf("Loopback: dong cua so preview (hoac ESC) de dung%s.\n",
            framesSet ? " (hoac du frame)" : "");
        for (;;) {
            renderer.Pump();
            if (!rendererReady.load() && srcW.load()) {
                if (!renderer.Init(gpu.device.Get(), srcW.load(), srcH.load(),
                                   L"Loopback Preview - GD2")) {
                    pipelineFailed.store(true);
                    break;
                }
                if (saveBmp) renderer.RequestDumpBmp("loopback.bmp");
                rendererReady.store(true);
            }
            if (renderer.Closed() || capture.Closed() || pipelineFailed.load()) break;
            if (framesSet && captured.load() >= targetFrames) break;
            Sleep(2);
        }
    } else {
        // --- Cho du frame hoac cua so dong; do fps ---
        const DWORD timeoutMs = 30'000;
        while (captured.load() < targetFrames && !capture.Closed()) {
            QueryPerformanceCounter(&now);
            double elapsedMs = double(now.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart);
            if (elapsedMs > timeoutMs) break;
            Sleep(5);  // main chi cho; frame den qua su kien
        }
    }

    // Dung capture TRUOC (khong con callback) roi moi finalize encoder -> decoder.
    capture.Stop();
    {
        std::lock_guard<std::mutex> lk(encMutex);
        if (encoder) encoder->Finish();
    }

    QueryPerformanceCounter(&now);
    const double seconds = double(now.QuadPart - start.QuadPart) / double(freq.QuadPart);
    const int total = captured.load();
    std::printf("Lay duoc %d frame trong %.2f giay (%.1f fps)%s\n",
        total, seconds, seconds > 0 ? total / seconds : 0.0,
        capture.Closed() ? " [cua so da dong]" : "");
    if (doLoopback) {
        const uint64_t n = rendered.load();
        std::printf("Loopback: hien thi %llu frame, tre trung binh %.1f ms, max %.1f ms%s\n",
            (unsigned long long)n,
            n ? latSumUs.load() / 1000.0 / n : 0.0, latMaxUs.load() / 1000.0,
            pipelineFailed.load() ? " [PIPELINE LOI]" : "");
    } else if (doEncode) {
        std::wprintf(L"Video da ghi: %ls\n", outPath.c_str());
    }

    return pipelineFailed.load() ? 1 : 0;
}
