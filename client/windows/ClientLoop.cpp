// ClientLoop (--connect) - vong doi phia client:
//
//   Thread Main: doi biet kich thuoc dam phan -> tao cua so preview (Renderer
//       phai Init/Pump tren thread bom message) -> bom message toi khi dong.
//   Thread Recv (vong chinh):
//       recvfrom (timeout 100ms)
//       ├─ goi Video  -> ClientSession.NotifyVideoPacket + Reassembler.Push
//       │                -> PopReady -> MfDecoder.Decode -> Renderer.RenderNV12
//       │                (ca chuoi tren 1 thread — dung mo hinh GD2, ~3.5ms)
//       ├─ goi Control-> ClientSession.HandlePacket (HELLO_ACK/PONG/BYE)
//       └─ moi vong   -> mat goi? xin IDR (retry trong Tick) ; Tick ; thong ke 1s
//
// Tre e2e uoc luong (docs/06 §7): offset dong ho host-client
//   D = (t_client_nhan_ACK − timebase_host) − RTT_min/2 ;  e2e = now − D − ts_frame.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "ClientLoop.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "GpuSelect.h"
#include "InputCapture.h"
#include "IVideoDecoder.h"
#include "Renderer.h"
#include "TimeUs.h"

#include "rgc/ClientSession.h"
#include "rgc/Reassembler.h"

namespace {

std::atomic<bool> g_ctrlC{false};

BOOL WINAPI CtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_ctrlC.store(true);
        return TRUE;
    }
    return FALSE;
}

} // namespace

int RunClient(const ClientOptions& opt) {
    g_ctrlC.store(false);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    GpuChoice gpu;
    if (!CreateBestDevice({ GpuVendor::Nvidia, GpuVendor::Intel, GpuVendor::Amd }, gpu)) {
        std::printf("[Client] Khong tao duoc D3D11 device.\n");
        return 1;
    }
    std::wprintf(L"[Client] GPU: %ls [%ls]\n", gpu.description.c_str(), GpuVendorName(gpu.vendor));

    UdpSocket sock;
    if (!sock.Open(0)) return 1; // port ngau nhien
    // GD4: 10ms thay vi 100ms. Input duoc gom va gui trong Tick cua vong Recv,
    // nen timeout recvfrom chinh la tran do tre input khi man hinh dang tinh
    // (khong co goi video nao danh thuc vong lap). 100 wakeup/s la khong dang ke.
    sock.SetRecvTimeout(10);
    std::printf("[Client] Ket noi toi %s ...\n", opt.server.ToString().c_str());

    // --- Chia se giua thread Recv va thread Main ---
    std::atomic<bool>     quit{false};
    std::atomic<bool>     failed{false};
    std::atomic<uint32_t> negW{0}, negH{0}; // kich thuoc dam phan — main tao renderer
    Renderer renderer;
    std::atomic<bool> rendererReady{false};

    // GD4: InputCapture chay tren luong Main (WndProc), ClientSession tren luong
    // Recv. Noi hai ben bang mot hang doi co khoa - khoa chi giu vai chuc nano
    // giay quanh push/swap, khong nam tren duong nong cua video.
    InputCapture input;
    std::mutex inputMutex;
    std::vector<rgc::InputEvent> inputQueue;

    std::thread recv([&] {
        std::unique_ptr<rgc::Reassembler> reasm; // tao sau khi biet fps dam phan
        std::unique_ptr<IVideoDecoder> decoder;

        // Uoc luong tre e2e.
        int64_t  ackDeltaUs = 0;   // t_client_nhan_ACK − timebase_host
        uint32_t minRttUs = 0;     // 0 = chua co PONG nao
        std::atomic<int64_t> lastE2eUs{-1};

        // Thong ke cua so 1s.
        uint64_t stBytes = 0;      // byte payload video nhan duoc
        uint32_t stRendered = 0;

        auto onDecoded = [&](const DecodedFrame& df) {
            if (!rendererReady.load(std::memory_order_acquire)) return;
            if (!renderer.RenderNV12(df.texture, df.subresource, df.width, df.height)) return;
            ++stRendered;
            if (minRttUs) {
                const int64_t offset = ackDeltaUs - int64_t(minRttUs) / 2;
                lastE2eUs.store(int64_t(QpcUs()) - offset - int64_t(df.timestampUs));
            }
        };

        rgc::ClientCallbacks cb;
        cb.send = [&](std::span<const uint8_t> d) { sock.SendTo(opt.server, d.data(), d.size()); };
        cb.onReady = [&](const rgc::NegotiatedParams& np) {
            ackDeltaUs = int64_t(QpcUs()) - int64_t(np.timebaseUs);
            std::printf("[Client] Dam phan xong: H264 %ux%u @%ufps, %.1f Mbps\n",
                        np.width, np.height, np.fps, np.bitrateBps / 1e6);
            negW.store(np.width);
            negH.store(np.height); // main thay kich thuoc -> tao cua so preview
        };
        cb.onRtt = [&](uint32_t rttUs) {
            if (!minRttUs || rttUs < minRttUs) minRttUs = rttUs;
        };
        cb.onDisconnect = [&](const char* reason) {
            std::printf("[Client] Ket thuc: %s\n", reason);
            quit.store(true);
        };
        rgc::ClientSession session(cb);

        rgc::Hello hello;
        hello.clientId   = uint32_t(QpcUs()) ^ GetCurrentProcessId();
        hello.codecMask  = rgc::kCodecMaskH264;
        hello.maxWidth   = uint16_t(GetSystemMetrics(SM_CXSCREEN));
        hello.maxHeight  = uint16_t(GetSystemMetrics(SM_CYSCREEN));
        hello.desiredFps = 60;
        hello.features   = 0;
        session.Start(hello, QpcUs());

        uint8_t buf[rgc::kMaxDatagram];
        uint64_t lastStatUs = QpcUs();
        rgc::Reassembler::Stats lastStats{};

        while (!quit.load() && !g_ctrlC.load() && !failed.load()) {
            NetAddr from;
            const int n = sock.RecvFrom(buf, sizeof(buf), from);
            const uint64_t now = QpcUs();
            if (n < 0) { std::printf("[Client] Loi socket.\n"); failed.store(true); break; }

            if (n > 0) {
                const auto span = std::span<const uint8_t>(buf, size_t(n));
                const auto h = rgc::ParseCommonHeader(span);
                if (h && h->chan == rgc::Chan::Video) {
                    if (h->sessionId == session.sessionId() && session.sessionId() != 0) {
                        if (const auto v = rgc::ParseVideoPacket(*h, rgc::PayloadOf(span))) {
                            session.NotifyVideoPacket(now);
                            if (!reasm) {
                                const uint32_t fps = session.params().fps ? session.params().fps : 60;
                                reasm = std::make_unique<rgc::Reassembler>(1'000'000 / fps);
                            }
                            reasm->Push(*v, now);
                            stBytes += v->payload.size();
                        }
                    }
                } else if (h) {
                    session.HandlePacket(span, now);
                }
            }

            if (reasm) {
                while (auto f = reasm->PopReady(now)) {
                    if (!decoder) {
                        DecoderConfig dc;
                        dc.codec  = Codec::H264;
                        dc.width  = session.params().width;
                        dc.height = session.params().height;
                        dc.fps    = session.params().fps;
                        decoder = CreateDecoder(gpu.device.Get(), dc, onDecoded);
                        if (!decoder) { failed.store(true); break; }
                    }
                    if (f->idr) session.CancelKeyframeRequest();
                    if (!decoder->Decode(f->nal.data(), f->nal.size(), f->timestampUs)) {
                        // Frame loi (hiem): coi nhu mat — xin IDR lam lai tu dau.
                        session.RequestKeyframe();
                    }
                }
                if (reasm->TakeLossEvent() || reasm->WaitingForIdr())
                    session.RequestKeyframe();
            }

            // Vet input do luong Main gom duoc -> ClientSession danh seq, Tick gui.
            {
                std::vector<rgc::InputEvent> batch;
                {
                    std::lock_guard<std::mutex> lk(inputMutex);
                    batch.swap(inputQueue);
                }
                for (const auto& e : batch) session.QueueInput(e);
            }

            session.Tick(now);
            if (session.state() == rgc::ClientSession::State::Dead) break;

            if (now - lastStatUs >= 1'000'000) {
                const double secs = (now - lastStatUs) / 1e6;
                const auto st = reasm ? reasm->stats() : rgc::Reassembler::Stats{};
                const uint64_t pkts = st.packetsReceived - lastStats.packetsReceived;
                const uint64_t lost = st.packetsLost - lastStats.packetsLost;
                const double lossPct = (pkts + lost) ? 100.0 * lost / double(pkts + lost) : 0.0;
                const int64_t e2e = lastE2eUs.load();
                std::printf("[Client] %2.0f fps | %6.0f kbps | bo %llu frame | mat %4.1f%% goi"
                            " | RTT %.1f ms | e2e ~%.1f ms\n",
                            stRendered / secs,
                            stBytes * 8.0 / 1000.0 / secs,
                            (unsigned long long)(st.framesDropped - lastStats.framesDropped),
                            lossPct,
                            session.lastRttUs() / 1000.0,
                            e2e >= 0 ? e2e / 1000.0 : 0.0);
                lastStats = st;
                stRendered = 0;
                stBytes = 0;
                lastStatUs = now;
            }
        }

        session.SendBye(); // best-effort; buf_ cua session chi dung tren thread nay
        quit.store(true);
    });

    // --- Thread Main: tao preview khi biet kich thuoc, bom message ---
    while (!quit.load() && !g_ctrlC.load()) {
        renderer.Pump();
        if (!rendererReady.load() && negW.load()) {
            wchar_t title[64];
            swprintf(title, 64, L"RemoteGame Client — %ux%u", negW.load(), negH.load());
            if (!renderer.Init(gpu.device.Get(), negW.load(), negH.load(), title)) {
                failed.store(true);
                break;
            }
            if (opt.saveBmp) renderer.RequestDumpBmp("client.bmp");
            if (opt.sendInput) {
                renderer.SetMessageHook([&](HWND h, UINT m, WPARAM w, LPARAM l) {
                    return input.OnMessage(h, m, w, l);
                });
                if (input.Attach(renderer.Hwnd(), [&](const rgc::InputEvent& e) {
                        std::lock_guard<std::mutex> lk(inputMutex);
                        inputQueue.push_back(e);
                    })) {
                    input.SetEnabled(true);
                }
            } else {
                std::printf("[Client] CHI XEM - khong gui input (--noinput).\n");
            }
            rendererReady.store(true, std::memory_order_release);
        }
        if (renderer.Closed()) { quit.store(true); break; }
        Sleep(2);
    }

    quit.store(true);
    input.Detach(); // tha chuot/con tro TRUOC khi huy cua so preview
    renderer.SetMessageHook(nullptr);
    recv.join();
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    std::printf("[Client] Da dung.\n");
    return failed.load() ? 1 : 0;
}
