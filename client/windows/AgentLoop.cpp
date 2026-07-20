// AgentLoop - ghép chuỗi GD2 với mạng GD3, GD6 mở rộng cho nhiều nguồn:
//
//   Mỗi nguồn (cửa sổ hoặc màn hình) = một SourcePipeline độc lập:
//     Thread FrameArrived (WGC, MỘT thread riêng cho mỗi nguồn):
//         capture -> encoder -> onPacket(NAL) -> Packetizer -> sock.SendTo(peer)
//     Trạng thái phiên, encoder, bitrate, FEC, injector đều RIÊNG từng nguồn.
//   Thread chính (Recv, DÙNG CHUNG cho mọi nguồn):
//         recvfrom timeout 100ms -> định tuyến gói:
//           LIST_SOURCES        -> trả SOURCE_LIST (danh sách nguồn đang chia sẻ)
//           HELLO               -> theo hello.sourceId (phiên chưa có sessionId)
//           còn lại             -> theo sessionId, khớp với phiên của từng nguồn
//         -> mỗi vòng: Tick mọi phiên + in thống kê mỗi 1s
//
// Vì sao một socket chung mà không phải mỗi nguồn một cổng: người dùng chỉ phải mở
// một cổng firewall và chỉ phải nhớ một địa chỉ; client hỏi LIST_SOURCES là thấy hết.
//
// ForceKeyframe là ATOMIC FLAG: đặt từ thread Recv (onStart/onKeyframeRequest),
// tiêu thụ ở lần Encode kế tiếp trên thread FrameArrived (docs/06 §4).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "AgentLoop.h"

#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

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

// Toàn bộ trạng thái của MỘT nguồn. Chứa mutex/atomic nên không copy/move được —
// giữ trong vector<unique_ptr>.
struct SourcePipeline {
    // --- Cấu hình, cố định sau khi dựng ---
    uint8_t       sourceId = 0;
    CaptureTarget target;
    std::string   name;

    WindowCapture capture;
    InputInjector injector;                    // chỉ thread Recv chạm
    std::unique_ptr<rgc::HostSession> session; // tạo sau khi biết kích thước nguồn
    rgc::StreamParams offer;                   // chỉ thread Recv chạm
    rgc::Packetizer   packetizer;              // chỉ thread FrameArrived chạm

    // --- Chia sẻ giữa thread FrameArrived và thread Recv ---
    std::atomic<uint32_t> srcW{0}, srcH{0};       // kích thước NÉN (đã làm chẵn)
    std::atomic<uint32_t> srcTexW{0}, srcTexH{0}; // kích thước texture WGC thật
    std::atomic<bool>     sizeChanged{false};
    std::atomic<bool>     wantFec{false};
    std::atomic<uint32_t> curBitrateBps{0};
    std::atomic<bool>     netReady{false};
    std::atomic<bool>     failed{false};
    std::atomic<bool>     forceIdr{false};
    std::atomic<uint64_t> peerPacked{0}; // NetAddr::Pack của client hiện tại (0 = chưa có)
    std::atomic<uint64_t> bytesSent{0}, framesSent{0};
    std::atomic<uint32_t> captured{0};
    std::atomic<uint32_t> nextFrameId{0};

    std::mutex encMutex; // bảo vệ encoder + cachedTex giữa 2 thread
    std::unique_ptr<IVideoEncoder> encoder;
    // Dựng encoder theo (kích thước nén, kích thước texture thật). Thread Recv cũng
    // cần gọi (encode lại frame tĩnh khi có yêu cầu IDR) nên phải giữ được sau khi
    // vòng khởi tạo kết thúc. GỌI DƯỚI encMutex.
    std::function<bool(uint32_t, uint32_t, uint32_t, uint32_t)> ensureEncoderFn;

    // WGC chỉ phát frame khi nội dung ĐỔI. Cache frame cuối để khi client xin IDR
    // mà nguồn đang tĩnh (menu, màn hình đứng im) vẫn có cái để encode gửi đi —
    // không cache thì client join màn hình tĩnh sẽ đen vĩnh viễn.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cachedTex;
    std::atomic<bool>     haveCached{false};
    std::atomic<uint64_t> lastFrameUs{0};

    // --- Congestion control, chỉ thread Recv chạm ---
    uint64_t lastDecreaseUs = 0;
    int      cleanSeconds = 0;

    // --- Thống kê cửa sổ 1s, chỉ thread Recv chạm ---
    uint32_t lastCaptured = 0;
    uint64_t lastBytes = 0, lastFrames = 0;
};

} // namespace

int RunAgent(std::span<const AgentSource> sources, const AgentOptions& opt) {
    g_ctrlC.store(false);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    if (sources.empty()) {
        std::printf("[Agent] No source selected.\n");
        return 1;
    }
    if (sources.size() > rgc::kMaxSources) {
        std::printf("[Agent] At most %zu sources can be shared at once.\n", rgc::kMaxSources);
        return 1;
    }

    GpuChoice gpu;
    if (!CreateBestDevice({ GpuVendor::Nvidia, GpuVendor::Intel, GpuVendor::Amd }, gpu)) {
        std::printf("[Agent] Failed to create D3D11 device.\n");
        return 1;
    }
    std::wprintf(L"[Agent] GPU: %ls [%ls]\n", gpu.description.c_str(), GpuVendorName(gpu.vendor));
    {
        // Immediate context được dùng từ NHIỀU thread (mỗi nguồn một thread
        // FrameArrived, cộng thread Recv encode lại frame tĩnh) -> bắt buộc bật.
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

    const uint32_t startBitrate = opt.bitrateMbps * 1'000'000u;
    const uint32_t maxBitrate   = startBitrate;
    const uint32_t minBitrate   = 1'000'000u; // dưới mức này hình nát, thà bỏ frame

    std::vector<std::unique_ptr<SourcePipeline>> pipes;
    for (size_t i = 0; i < sources.size(); ++i) {
        auto p = std::make_unique<SourcePipeline>();
        p->sourceId = uint8_t(i);
        p->target   = sources[i].target;
        p->name     = sources[i].name;
        p->curBitrateBps.store(startBitrate);
        pipes.push_back(std::move(p));
    }

    // --- Khởi động capture cho từng nguồn ---
    for (auto& up : pipes) {
        SourcePipeline* p = up.get();

        // NAL vừa nén xong (thread FrameArrived của nguồn này) -> cắt gói -> UDP.
        auto onPacket = [p, &sock](const uint8_t* data, size_t size, uint64_t tsUs,
                                   bool keyframe) {
            if (!p->session || p->session->state() != rgc::HostSession::State::Streaming) return;
            const uint64_t pp = p->peerPacked.load(std::memory_order_acquire);
            if (!pp) return;
            const NetAddr peer = NetAddr::Unpack(pp);
            p->packetizer.SetSessionId(p->session->sessionId());
            // Packetizer là single-thread (thread này). Thread Recv chỉ đặt ý muốn
            // qua atomic, việc bật/tắt thật diễn ra ở đây — khỏi cần khóa.
            p->packetizer.SetFecEnabled(p->wantFec.load(std::memory_order_relaxed));
            const size_t pkts = p->packetizer.SendFrame(
                std::span<const uint8_t>(data, size), p->nextFrameId++, tsUs, keyframe,
                [p, &sock, &peer](std::span<const uint8_t> d) {
                    sock.SendTo(peer, d.data(), d.size());
                    p->bytesSent.fetch_add(d.size(), std::memory_order_relaxed);
                });
            if (pkts) p->framesSent.fetch_add(1, std::memory_order_relaxed);
        };

        // Tạo encoder nếu chưa có. GỌI DƯỚI encMutex. false = backend không dùng được.
        // `w`/`h` là kích thước NÉN (chẵn); `sw`/`sh` là kích thước texture thật.
        auto ensureEncoder = [p, &gpu, &opt, onPacket](uint32_t w, uint32_t h,
                                                       uint32_t sw, uint32_t sh) -> bool {
            if (p->encoder) return true;
            EncoderConfig cfg;
            cfg.width  = w;
            cfg.height = h;
            cfg.srcWidth  = sw;
            cfg.srcHeight = sh;
            cfg.fps = opt.fps;
            cfg.bitrateBps = p->curBitrateBps.load(std::memory_order_relaxed);
            cfg.outputPath.clear(); // không file — NAL chỉ đi qua onPacket
            cfg.onPacket = onPacket;
            p->encoder = CreateEncoder(gpu.device.Get(), cfg);
            if (!p->encoder) {
                std::printf("[Agent][%s] No usable encoder backend (NVENC + Media Foundation"
                            " both failed).\n", p->name.c_str());
                p->failed.store(true);
                return false;
            }
            return true;
        };
        p->ensureEncoderFn = ensureEncoder;

        auto onFrame = [p, &gpu, ensureEncoder](const FrameInfo& fi) {
            p->captured.fetch_add(1, std::memory_order_relaxed);
            if (p->failed.load()) return;

            // NV12 lấy mẫu chroma 2x2 -> bề rộng/cao lẻ làm CreateTexture2D(NV12) trả
            // E_INVALIDARG. Nén ở kích thước chẵn nhỏ hơn; cột/hàng lẻ dư bị cắt.
            const uint32_t encW = fi.width & ~1u, encH = fi.height & ~1u;
            if (!encW || !encH) return;

            std::lock_guard<std::mutex> lk(p->encMutex);

            // Nguồn đổi kích thước (người dùng kéo cửa sổ / đổi độ phân giải màn
            // hình). Encoder và texture cache đều gắn chặt với kích thước cũ -> vứt
            // cả hai, dựng lại ngay ở frame này. Cờ sizeChanged để thread Recv báo
            // RECONFIG + IDR cho client.
            if (p->srcW.load() != encW || p->srcH.load() != encH) {
                if (p->srcW.load())
                    std::printf("[Agent][%s] Source resized %ux%u -> %ux%u,"
                                " rebuilding encoder.\n",
                                p->name.c_str(), p->srcW.load(), p->srcH.load(), encW, encH);
                p->srcW.store(encW);
                p->srcH.store(encH);
                p->srcTexW.store(fi.width);
                p->srcTexH.store(fi.height);
                p->encoder.reset();
                p->cachedTex.Reset();
                p->haveCached.store(false, std::memory_order_release);
                p->sizeChanged.store(true, std::memory_order_release);
            }

            // Lưu bản sao frame cuối (texture của WGC chỉ sống trong callback).
            if (!p->cachedTex) {
                D3D11_TEXTURE2D_DESC d{};
                fi.texture->GetDesc(&d);
                d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                d.MiscFlags = 0;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.CPUAccessFlags = 0;
                if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, p->cachedTex.GetAddressOf())))
                    p->cachedTex.Reset();
            }
            if (p->cachedTex) {
                gpu.context->CopyResource(p->cachedTex.Get(), fi.texture);
                p->haveCached.store(true, std::memory_order_release);
            }
            p->lastFrameUs.store(QpcUs(), std::memory_order_relaxed);

            if (!p->netReady.load(std::memory_order_acquire)) return;
            if (!ensureEncoder(encW, encH, fi.width, fi.height)) return;
            // Encode liên tục kể cả khi chưa có client (đơn giản, VBV ổn định);
            // NAL bị bỏ ở onPacket nếu chưa STREAMING.
            p->encoder->Encode(fi.texture, QpcUs(), p->forceIdr.exchange(false));
        };

        if (!p->capture.Start(p->target, gpu.device.Get(), onFrame)) {
            std::printf("[Agent][%s] Failed to start capture — skipping this source.\n",
                        p->name.c_str());
            p->failed.store(true);
        }
    }

    // --- Đợi frame đầu của từng nguồn để biết kích thước (offer trong HELLO_ACK) ---
    for (int i = 0; i < 1000 && !g_ctrlC.load(); ++i) {
        bool allKnown = true;
        for (auto& p : pipes)
            if (!p->failed.load() && !p->srcW.load() && !p->capture.Closed()) allKnown = false;
        if (allKnown) break;
        Sleep(10);
    }

    // Nguồn không phát frame nào trong 10s thì bỏ, không kéo cả phiên xuống theo.
    std::vector<SourcePipeline*> live;
    for (auto& p : pipes) {
        if (p->failed.load() || !p->srcW.load()) {
            if (!p->failed.load())
                std::printf("[Agent][%s] No frame within 10s — not sharing this source.\n",
                            p->name.c_str());
            p->capture.Stop();
            p->failed.store(true);
            continue;
        }
        live.push_back(p.get());
    }
    if (live.empty()) {
        std::printf("[Agent] No usable source — stopping.\n");
        return 1;
    }

    // --- Dựng phiên + injector cho từng nguồn còn sống ---
    NetAddr replyAddr; // địa chỉ nguồn của gói đang xử lý (chỉ thread Recv dùng)
    for (SourcePipeline* p : live) {
        p->offer.width      = uint16_t(p->srcW.load());
        p->offer.height     = uint16_t(p->srcH.load());
        p->offer.fps        = uint8_t(opt.fps);
        p->offer.bitrateBps = startBitrate;
        std::printf("[Agent] Source %u \"%s\": %ux%u @%ufps, %u Mbps.\n",
                    p->sourceId, p->name.c_str(), p->offer.width, p->offer.height,
                    opt.fps, opt.bitrateMbps);

        if (opt.allowInput) {
            const bool ok = p->target.hwnd ? p->injector.Init(p->target.hwnd)
                                           : p->injector.InitMonitor(p->target.monitor);
            p->injector.SetEnabled(ok);
        } else {
            p->injector.SetEnabled(false);
        }

        rgc::HostCallbacks cb;
        cb.send = [&sock, &replyAddr](std::span<const uint8_t> d) {
            sock.SendTo(replyAddr, d.data(), d.size());
        };
        cb.onStart = [p] {
            p->forceIdr.store(true); // IDR mở màn (kèm SPS/PPS — repeatSPSPPS=1)
            std::printf("[Agent][%s] Client START — beginning video push.\n", p->name.c_str());
        };
        cb.onKeyframeRequest = [p] { p->forceIdr.store(true); };
        cb.onInput = [p](const rgc::InputEvent& e) { p->injector.Apply(e); };
        cb.onDisconnect = [p] {
            p->peerPacked.store(0, std::memory_order_release);
            p->injector.ReleaseAll(); // mất kết nối giữa lúc giữ phím = kẹt phím
            std::printf("[Agent][%s] Client left (BYE/timeout).\n", p->name.c_str());
        };
        // GD5 congestion control, RIÊNG từng nguồn: hai nguồn có thể đi cùng một
        // đường mạng nhưng bitrate của chúng độc lập nhau, và client có thể chỉ
        // đang xem một trong hai.
        cb.onFeedback = [p, maxBitrate, minBitrate](const rgc::Feedback& fb) {
            const uint64_t now = QpcUs();

            // FEC tốn 1/kFecGroupSize băng thông nên chỉ bật khi đang thực sự mất
            // gói. Tắt chậm hơn bật (5 giây sạch): mất gói thường đến theo cụm.
            if (fb.lossPct >= 1) {
                p->cleanSeconds = 0;
                if (!p->wantFec.exchange(true, std::memory_order_relaxed))
                    std::printf("[Agent][%s] FEC on (loss %u%%).\n", p->name.c_str(), fb.lossPct);
            } else if (++p->cleanSeconds >= 5) {
                if (p->wantFec.exchange(false, std::memory_order_relaxed))
                    std::printf("[Agent][%s] FEC off (link clean).\n", p->name.c_str());
            }

            const uint32_t cur = p->curBitrateBps.load(std::memory_order_relaxed);
            uint32_t next = cur;
            if (fb.lossPct >= 5) {
                next = cur - cur / 4;          // ×0.75
                p->lastDecreaseUs = now;
            } else if (fb.lossPct >= 2) {
                next = cur - cur / 10;         // ×0.90
                p->lastDecreaseUs = now;
            } else if (fb.lossPct <= 1 && now - p->lastDecreaseUs > 2'000'000) {
                next = cur + maxBitrate / 20;  // +5% trần mỗi giây
            }
            if (next > maxBitrate) next = maxBitrate;
            if (next < minBitrate) next = minBitrate;
            // Bỏ qua thay đổi vụn: mỗi lần gọi là một lần đàm phán lại rate control.
            if (next == cur || (next > cur ? next - cur : cur - next) < cur / 50) return;

            std::lock_guard<std::mutex> lk(p->encMutex);
            if (p->encoder && p->encoder->SetBitrate(next)) {
                p->curBitrateBps.store(next, std::memory_order_relaxed);
                std::printf("[Agent][%s] Bitrate %.1f -> %.1f Mbps (loss %u%%, RTT %u ms)\n",
                            p->name.c_str(), cur / 1e6, next / 1e6, fb.lossPct, fb.rttMs);
            }
        };

        p->session = std::make_unique<rgc::HostSession>(cb, p->offer);
        p->netReady.store(true, std::memory_order_release);
    }

    if (opt.allowInput)
        std::printf("[Agent] Client control allowed (mouse + keyboard).\n");
    else
        std::printf("[Agent] VIEW ONLY - input from client is ignored.\n");
    std::printf("[Agent] Sharing %zu source(s). Waiting for client...\n", live.size());

    // --- Vòng Recv (thread chính), dùng chung cho mọi nguồn ---
    uint8_t buf[rgc::kMaxDatagram];
    uint64_t lastStatUs = QpcUs();
    bool anyFailed = false;

    for (;;) {
        if (g_ctrlC.load()) break;
        // Dừng khi MỌI nguồn đã đóng/hỏng — một cửa sổ đóng không nên giết cả phiên.
        bool anyAlive = false;
        for (SourcePipeline* p : live)
            if (!p->failed.load() && !p->capture.Closed()) anyAlive = true;
        if (!anyAlive) break;

        NetAddr from;
        const int n = sock.RecvFrom(buf, sizeof(buf), from);
        const uint64_t now = QpcUs();
        if (n < 0) { std::printf("[Agent] Socket error — stopping.\n"); anyFailed = true; break; }

        if (n > 0) {
            replyAddr = from;
            const auto span = std::span<const uint8_t>(buf, size_t(n));
            const auto h = rgc::ParseCommonHeader(span);
            if (h && h->type == rgc::MsgType::ListSources) {
                // Chỉ liệt kê nguồn còn sống, kèm kích thước hiện tại.
                std::vector<rgc::SourceInfo> infos;
                for (SourcePipeline* p : live) {
                    if (p->failed.load() || p->capture.Closed()) continue;
                    rgc::SourceInfo si;
                    si.sourceId = p->sourceId;
                    si.width    = uint16_t(p->srcW.load());
                    si.height   = uint16_t(p->srcH.load());
                    si.name     = p->name;
                    infos.push_back(std::move(si));
                }
                const size_t sn = rgc::BuildSourceList(buf, infos);
                if (sn) sock.SendTo(from, buf, sn);
            } else if (h) {
                // HELLO chưa có sessionId -> định tuyến theo sourceId. Mọi gói khác
                // đã mang sessionId -> tìm phiên khớp.
                SourcePipeline* dst = nullptr;
                if (h->type == rgc::MsgType::Hello) {
                    const auto m = rgc::ParseHello(rgc::PayloadOf(span));
                    if (m)
                        for (SourcePipeline* p : live)
                            if (p->sourceId == m->sourceId) dst = p;
                } else if (h->sessionId) {
                    for (SourcePipeline* p : live)
                        if (p->session->sessionId() == h->sessionId) dst = p;
                }
                if (dst && !dst->failed.load() && dst->session->HandlePacket(span, now)) {
                    // Gói hợp lệ thuộc phiên — cập nhật peer (client đổi IP/port).
                    const uint64_t pk = from.Pack();
                    if (dst->peerPacked.load(std::memory_order_relaxed) != pk) {
                        dst->peerPacked.store(pk, std::memory_order_release);
                        std::printf("[Agent][%s] Peer: %s\n", dst->name.c_str(),
                                    from.ToString().c_str());
                    }
                }
            }
        }

        for (SourcePipeline* p : live) {
            if (p->failed.load()) continue;
            p->session->Tick(now);

            // Nguồn vừa đổi kích thước (thread FrameArrived đã dựng lại encoder).
            // Báo client kích thước mới + IDR: stream đổi SPS giữa chừng, không có
            // IDR thì decoder client chỉ có rác cho tới keyframe kế tiếp.
            if (p->sizeChanged.exchange(false, std::memory_order_acq_rel)) {
                p->offer.width      = uint16_t(p->srcW.load());
                p->offer.height     = uint16_t(p->srcH.load());
                p->offer.bitrateBps = p->curBitrateBps.load(std::memory_order_relaxed);
                p->session->SetOffer(p->offer); // HELLO phát lại sau phải mang số mới
                const uint64_t pp = p->peerPacked.load(std::memory_order_acquire);
                if (pp && p->session->state() == rgc::HostSession::State::Streaming) {
                    rgc::Reconfig rc{p->offer.width, p->offer.height, p->offer.bitrateBps};
                    uint8_t rbuf[rgc::kMaxDatagram];
                    const size_t rn = rgc::BuildReconfig(rbuf, p->session->sessionId(), rc);
                    if (rn) sock.SendTo(NetAddr::Unpack(pp), rbuf, rn);
                    p->forceIdr.store(true);
                }
            }

            // Yêu cầu IDR đang treo mà nguồn đang TĨNH (>200ms không có FrameArrived
            // — WGC chỉ phát khi nội dung đổi) -> encode lại frame cache để có hình.
            if (p->session->state() == rgc::HostSession::State::Streaming &&
                p->forceIdr.load() && p->haveCached.load(std::memory_order_acquire) &&
                now - p->lastFrameUs.load(std::memory_order_relaxed) > 200'000) {
                std::lock_guard<std::mutex> lk(p->encMutex);
                if (p->ensureEncoderFn(p->srcW.load(), p->srcH.load(),
                                       p->srcTexW.load(), p->srcTexH.load()) &&
                    p->forceIdr.exchange(false))
                    p->encoder->Encode(p->cachedTex.Get(), QpcUs(), true);
            }
        }

        if (now - lastStatUs >= 1'000'000) {
            const double secs = (now - lastStatUs) / 1e6;
            for (SourcePipeline* p : live) {
                if (p->failed.load()) continue;
                const uint32_t cap = p->captured.load();
                const uint64_t by = p->bytesSent.load(), fr = p->framesSent.load();
                const auto& ist = p->session->inputStats();
                std::printf("[Agent][%s] %-9s | capture %.0f fps | send %.0f fps, %.0f kbps"
                            " | input %llu (lost %llu)\n",
                            p->name.c_str(), StateName(p->session->state()),
                            (cap - p->lastCaptured) / secs,
                            (fr - p->lastFrames) / secs,
                            (by - p->lastBytes) * 8.0 / 1000.0 / secs,
                            (unsigned long long)ist.applied,
                            (unsigned long long)ist.lost);
                p->lastCaptured = cap;
                p->lastBytes = by;
                p->lastFrames = fr;
            }
            lastStatUs = now;
        }
    }

    // --- Dọn dẹp ---
    uint64_t totalFrames = 0;
    double   totalMB = 0;
    for (SourcePipeline* p : live) {
        p->injector.ReleaseAll(); // thoát giữa lúc client đang giữ phím -> nhả ra

        // Chia tay tử tế: báo BYE cho client nếu còn phiên.
        if (p->session->state() != rgc::HostSession::State::Idle) {
            const uint64_t pp = p->peerPacked.load();
            if (pp) {
                uint8_t bye[rgc::kCommonHeaderSize];
                const size_t bn = rgc::BuildBye(bye, p->session->sessionId());
                if (bn) sock.SendTo(NetAddr::Unpack(pp), bye, bn);
            }
        }
        p->capture.Stop(); // hết callback rồi mới dọn encoder
        {
            std::lock_guard<std::mutex> lk(p->encMutex);
            if (p->encoder) p->encoder->Finish();
        }
        p->netReady.store(false);
        totalFrames += p->framesSent.load();
        totalMB += p->bytesSent.load() / 1e6;
    }
    std::printf("[Agent] Stopped. Total: %llu frames sent, %.2f MB.\n",
                (unsigned long long)totalFrames, totalMB);
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    return anyFailed ? 1 : 0;
}
