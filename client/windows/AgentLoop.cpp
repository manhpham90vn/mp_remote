// AgentLoop (--serve) - ghép chuỗi GD2 với mạng GD3:
//
//   Thread FrameArrived (WGC):  capture -> NVENC -> onPacket(NAL)
//       -> nếu session STREAMING: Packetizer.SendFrame -> sock.SendTo(peer)
//       -> chưa STREAMING: bỏ NAL (không đếm)
//   Thread chính (Recv):        recvfrom timeout 100ms -> HostSession.HandlePacket
//       -> gói hợp lệ: cập nhật peer theo địa chỉ nguồn (roaming theo sessionId)
//       -> mỗi vòng: HostSession.Tick + in thống kê mỗi 1s
//
// ForceKeyframe là ATOMIC FLAG: đặt từ thread Recv (onStart/onKeyframeRequest),
// tiêu thụ ở lần Encode kế tiếp trên thread FrameArrived (docs/06 §4).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "AgentLoop.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

#include <wrl/client.h>

#include "GpuSelect.h"
#include "InputInjector.h"
#include "IVideoEncoder.h"
#include "NetInfo.h"
#include "TimeUs.h"
#include "UdpSocket.h"
#include "WindowCapture.h"

#include "rgc/HostSession.h"
#include "rgc/Packetizer.h"

namespace {

std::atomic<bool> g_ctrlC{false};

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_ctrlC.store(true);
        return TRUE;
    }
    return FALSE;
}

const char* StateName(rgc::HostSession::State s) {
    switch (s) {
    case rgc::HostSession::State::Idle:      return "IDLE";
    case rgc::HostSession::State::Ready:     return "READY";
    case rgc::HostSession::State::Streaming: return "STREAMING";
    }
    return "?";
}

} // namespace

int RunAgent(HWND target, const AgentOptions& opt) {
    g_ctrlC.store(false);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    GpuChoice gpu;
    if (!CreateBestDevice({ GpuVendor::Nvidia, GpuVendor::Intel, GpuVendor::Amd }, gpu)) {
        std::printf("[Agent] Failed to create D3D11 device.\n");
        return 1;
    }
    std::wprintf(L"[Agent] GPU: %ls [%ls]\n", gpu.description.c_str(), GpuVendorName(gpu.vendor));
    {
        // Immediate context được dùng từ 2 thread (FrameArrived copy cache,
        // Recv encode lại frame tĩnh) -> bật multithread protection.
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(gpu.device.As(&mt))) mt->SetMultithreadProtected(TRUE);
    }

    UdpSocket sock;
    if (!sock.Open(opt.port)) return 1;
    sock.SetRecvTimeout(100);
    std::printf("[Agent] Listening on UDP port %u. On the other machine, open client.exe"
                " and enter one of:\n", opt.port);
    for (const auto& a : ListLocalIPv4())
        std::wprintf(L"    %hs:%u    (%ls)\n", a.ip.c_str(), opt.port, a.name.c_str());

    // --- Trạng thái chia sẻ giữa thread FrameArrived và thread Recv ---
    std::atomic<uint32_t> srcW{0}, srcH{0};
    std::atomic<bool>     netReady{false};   // session da tao xong (sau frame dau)
    std::atomic<bool>     failed{false};
    std::atomic<bool>     forceIdr{false};   // cau IDR: dat o Recv, tieu thu o Encode
    std::atomic<uint64_t> peerPacked{0};     // NetAddr::Pack cua client hien tai (0 = chua co)
    std::atomic<uint64_t> bytesSent{0}, framesSent{0};
    std::atomic<uint32_t> captured{0};

    std::unique_ptr<rgc::HostSession> session; // tạo sau khi biết kích thước nguồn
    rgc::Packetizer packetizer;
    std::atomic<uint32_t> nextFrameId{0}; // chạm từ cả 2 thread (frame mới / re-encode tĩnh)

    std::mutex encMutex; // bảo vệ encoder + cachedTex giữa 2 thread
    std::unique_ptr<IVideoEncoder> encoder;

    // WGC chỉ phát frame khi nội dung ĐỔI. Cache frame cuối để khi client xin IDR
    // mà nguồn đang tĩnh (menu, màn hình đứng im) vẫn có cái để encode gửi đi —
    // không cache thì client join màn hình tĩnh sẽ đen vĩnh viễn.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cachedTex;
    std::atomic<bool>     haveCached{false};
    std::atomic<uint64_t> lastFrameUs{0};

    // NAL vừa nén xong (thread FrameArrived) -> cắt gói -> UDP tới client.
    auto onPacket = [&](const uint8_t* data, size_t size, uint64_t tsUs, bool keyframe) {
        if (!session || session->state() != rgc::HostSession::State::Streaming) return;
        const uint64_t pp = peerPacked.load(std::memory_order_acquire);
        if (!pp) return;
        const NetAddr peer = NetAddr::Unpack(pp);
        packetizer.SetSessionId(session->sessionId());
        const size_t pkts = packetizer.SendFrame(std::span<const uint8_t>(data, size),
                                                 nextFrameId++, tsUs, keyframe,
            [&](std::span<const uint8_t> d) {
                sock.SendTo(peer, d.data(), d.size());
                bytesSent.fetch_add(d.size(), std::memory_order_relaxed);
            });
        if (pkts) framesSent.fetch_add(1, std::memory_order_relaxed);
    };

    // Tạo encoder nếu chưa có. GỌI DƯỚI encMutex. false = backend không dùng được.
    auto ensureEncoder = [&](uint32_t w, uint32_t h) -> bool {
        if (encoder) return true;
        EncoderConfig cfg;
        cfg.width = w;
        cfg.height = h;
        cfg.fps = opt.fps;
        cfg.bitrateBps = opt.bitrateMbps * 1'000'000u;
        cfg.outputPath.clear(); // không file — NAL chỉ đi qua onPacket
        cfg.onPacket = onPacket;
        encoder = CreateEncoder(gpu.device.Get(), cfg);
        if (!encoder) {
            std::printf("[Agent] GD3 needs NVENC (MF doesn't emit NAL yet) — stopping.\n");
            failed.store(true);
            return false;
        }
        return true;
    };

    auto onFrame = [&](const FrameInfo& fi) {
        captured.fetch_add(1, std::memory_order_relaxed);
        if (!srcW.load()) { srcW.store(fi.width); srcH.store(fi.height); }
        if (failed.load()) return;

        std::lock_guard<std::mutex> lk(encMutex);
        // Lưu bản sao frame cuối (texture của WGC chỉ sống trong callback).
        if (!cachedTex) {
            D3D11_TEXTURE2D_DESC d{};
            fi.texture->GetDesc(&d);
            d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            d.MiscFlags = 0;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.CPUAccessFlags = 0;
            if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, cachedTex.GetAddressOf())))
                cachedTex.Reset();
        }
        if (cachedTex) {
            gpu.context->CopyResource(cachedTex.Get(), fi.texture);
            haveCached.store(true, std::memory_order_release);
        }
        lastFrameUs.store(QpcUs(), std::memory_order_relaxed);

        if (!netReady.load(std::memory_order_acquire)) return;
        if (!ensureEncoder(fi.width, fi.height)) return;
        // Encode liên tục kể cả khi chưa có client (đơn giản, VBV ổn định);
        // NAL bị bỏ ở onPacket nếu chưa STREAMING.
        encoder->Encode(fi.texture, QpcUs(), forceIdr.exchange(false));
    };

    WindowCapture capture;
    if (!capture.Start(target, gpu.device.Get(), onFrame)) return 1;

    // Đợi frame đầu để biết kích thước nguồn (offer trong HELLO_ACK).
    for (int i = 0; i < 1000 && !srcW.load() && !capture.Closed() && !g_ctrlC.load(); ++i)
        Sleep(10);
    if (!srcW.load()) {
        std::printf("[Agent] Did not receive first frame from window — stopping.\n");
        capture.Stop();
        return 1;
    }

    rgc::StreamParams offer;
    offer.width      = uint16_t(srcW.load());
    offer.height     = uint16_t(srcH.load());
    offer.fps        = uint8_t(opt.fps);
    offer.bitrateBps = opt.bitrateMbps * 1'000'000u;
    std::printf("[Agent] Source %ux%u @%ufps, %u Mbps. Waiting for client...\n",
                offer.width, offer.height, opt.fps, opt.bitrateMbps);

    // GD4: bơm input từ client vào cửa sổ đang chia sẻ. Chỉ thread Recv chạm vào.
    InputInjector injector;
    if (opt.allowInput) {
        injector.SetEnabled(injector.Init(target));
        std::printf("[Agent] Client control allowed (mouse + keyboard).\n");
    } else {
        injector.SetEnabled(false);
        std::printf("[Agent] VIEW ONLY - input from client is ignored (--noinput).\n");
    }

    NetAddr replyAddr; // địa chỉ nguồn của gói đang xử lý (chỉ thread Recv dùng)
    rgc::HostCallbacks cb;
    cb.send = [&](std::span<const uint8_t> d) { sock.SendTo(replyAddr, d.data(), d.size()); };
    cb.onStart = [&] {
        forceIdr.store(true); // IDR mở màn (kèm SPS/PPS — repeatSPSPPS=1)
        std::printf("[Agent] Client START — beginning video push.\n");
    };
    cb.onKeyframeRequest = [&] { forceIdr.store(true); };
    cb.onInput = [&](const rgc::InputEvent& e) { injector.Apply(e); };
    cb.onDisconnect = [&] {
        peerPacked.store(0, std::memory_order_release);
        injector.ReleaseAll(); // BẮT BUỘC: mất kết nối giữa lúc giữ phím = kẹt phím
        std::printf("[Agent] Client left (BYE/timeout) — waiting for a new client.\n");
    };
    session = std::make_unique<rgc::HostSession>(cb, offer);
    netReady.store(true, std::memory_order_release);

    // --- Vòng Recv (thread chính) ---
    uint8_t buf[rgc::kMaxDatagram];
    uint64_t lastStatUs = QpcUs();
    uint32_t lastCaptured = 0;
    uint64_t lastBytes = 0, lastFrames = 0;

    while (!g_ctrlC.load() && !failed.load() && !capture.Closed()) {
        NetAddr from;
        const int n = sock.RecvFrom(buf, sizeof(buf), from);
        const uint64_t now = QpcUs();
        if (n < 0) { std::printf("[Agent] Socket error — stopping.\n"); break; }
        if (n > 0) {
            replyAddr = from;
            if (session->HandlePacket(std::span<const uint8_t>(buf, size_t(n)), now)) {
                // Gói hợp lệ thuộc phiên — cập nhật peer (client roaming đổi IP/port).
                const uint64_t pk = from.Pack();
                if (peerPacked.load(std::memory_order_relaxed) != pk) {
                    peerPacked.store(pk, std::memory_order_release);
                    std::printf("[Agent] Peer: %s\n", from.ToString().c_str());
                }
            }
        }
        session->Tick(now);

        // Yêu cầu IDR đang treo mà nguồn đang TĨNH (>200ms không có FrameArrived —
        // WGC chỉ phát khi nội dung đổi) -> encode lại frame cache để client có hình.
        if (session->state() == rgc::HostSession::State::Streaming &&
            forceIdr.load() && haveCached.load(std::memory_order_acquire) &&
            now - lastFrameUs.load(std::memory_order_relaxed) > 200'000) {
            std::lock_guard<std::mutex> lk(encMutex);
            if (ensureEncoder(srcW.load(), srcH.load()) && forceIdr.exchange(false))
                encoder->Encode(cachedTex.Get(), QpcUs(), true);
        }

        if (now - lastStatUs >= 1'000'000) {
            const double secs = (now - lastStatUs) / 1e6;
            const uint32_t cap = captured.load();
            const uint64_t by = bytesSent.load(), fr = framesSent.load();
            const auto& ist = session->inputStats();
            std::printf("[Agent] %-9s | capture %.0f fps | send %.0f fps, %.0f kbps"
                        " | input %llu (lost %llu)\n",
                        StateName(session->state()),
                        (cap - lastCaptured) / secs,
                        (fr - lastFrames) / secs,
                        (by - lastBytes) * 8.0 / 1000.0 / secs,
                        (unsigned long long)ist.applied,
                        (unsigned long long)ist.lost);
            lastStatUs = now;
            lastCaptured = cap;
            lastBytes = by;
            lastFrames = fr;
        }
    }

    injector.ReleaseAll(); // thoát giữa lúc client đang giữ phím -> nhả ra

    // Chia tay tử tế: báo BYE cho client nếu còn phiên.
    if (session->state() != rgc::HostSession::State::Idle) {
        const uint64_t pp = peerPacked.load();
        if (pp) {
            const NetAddr peer = NetAddr::Unpack(pp);
            uint8_t bye[rgc::kCommonHeaderSize];
            const size_t bn = rgc::BuildBye(bye, session->sessionId());
            if (bn) sock.SendTo(peer, bye, bn);
        }
    }

    capture.Stop(); // hết callback rồi mới dọn encoder
    {
        std::lock_guard<std::mutex> lk(encMutex);
        if (encoder) encoder->Finish();
    }
    netReady.store(false);
    std::printf("[Agent] Stopped. Total: %llu frames sent, %.2f MB.\n",
                (unsigned long long)framesSent.load(), bytesSent.load() / 1e6);
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    return failed.load() ? 1 : 0;
}
