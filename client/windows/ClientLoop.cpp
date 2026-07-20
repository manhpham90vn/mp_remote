// ClientLoop (--connect) - vòng đời phía client:
//
//   Thread Main: đợi biết kích thước đàm phán -> tạo cửa sổ preview (Renderer
//       phải Init/Pump trên thread bơm message) -> bơm message tới khi đóng.
//   Thread Recv (vòng chính):
//       recvfrom (timeout 100ms)
//       ├─ gói Video  -> ClientSession.NotifyVideoPacket + Reassembler.Push
//       │                -> PopReady -> đẩy vào hàng đợi cho Thread Decode
//       ├─ gói Control-> ClientSession.HandlePacket (HELLO_ACK/PONG/BYE)
//       └─ mỗi vòng   -> mất gói? xin IDR (retry trong Tick) ; Tick ; thống kê 1s
//   Thread Decode (mới, GD6): rút frame từ hàng đợi -> MfDecoder.Decode ->
//       Renderer.RenderNV12 (Renderer.renderMutex đã tự bảo vệ cho đúng trường
//       hợp này). Tách khỏi Thread Recv vì decode+render GPU có thể mất vài chục
//       ms khi máy bận (capture+encode trên cùng GPU, driver bị giật...) — nếu
//       chặn Thread Recv thì recvfrom ngừng nghe đúng lúc đó, UDP buffer của OS
//       đầy lên rồi tràn, gây mất gói THẬT chứ không còn là "giả" nữa. Hàng đợi
//       giới hạn kMaxQueuedFrames: nếu Thread Decode theo không kịp, bỏ frame cũ
//       nhất và xin IDR (chuỗi inter-frame đã đứt, giữ lại cũng vô nghĩa).
//
// Trễ e2e ước lượng (docs/06 §7): offset đồng hồ host-client
//   D = (t_client_nhận_ACK − timebase_host) − RTT_min/2 ;  e2e = now − D − ts_frame.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include "ClientLoop.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
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
        std::printf("[Client] Failed to create D3D11 device.\n");
        return 1;
    }
    std::wprintf(L"[Client] GPU: %ls [%ls]\n", gpu.description.c_str(), GpuVendorName(gpu.vendor));

    UdpSocket sock;
    if (!sock.Open(0)) return 1; // cổng ngẫu nhiên
    // GD4: 10ms thay vì 100ms. Input được gom và gửi trong Tick của vòng Recv,
    // nên timeout recvfrom chính là trần độ trễ input khi màn hình đang tĩnh
    // (không có gói video nào đánh thức vòng lặp). 100 wakeup/s là không đáng kể.
    sock.SetRecvTimeout(10);
    std::printf("[Client] Connecting to %s ...\n", opt.server.ToString().c_str());

    // --- Chia sẻ giữa thread Recv và thread Main ---
    std::atomic<bool>     quit{false};
    std::atomic<bool>     failed{false};
    std::atomic<uint32_t> negW{0}, negH{0}; // kích thước đàm phán — main tạo renderer
    Renderer renderer;
    std::atomic<bool> rendererReady{false};

    // GD4: InputCapture chạy trên luồng Main (WndProc), ClientSession trên luồng
    // Recv. Nối hai bên bằng một hàng đợi có khóa - khóa chỉ giữ vài chục nano
    // giây quanh push/swap, không nằm trên đường nóng của video.
    InputCapture input;
    std::mutex inputMutex;
    std::vector<rgc::InputEvent> inputQueue;

    // GD5: dòng số liệu cho overlay trên cửa sổ preview - ghi từ luồng Recv, đọc
    // từ luồng Main (giống mô hình inputQueue/inputMutex ở trên).
    std::mutex statsMutex;
    std::wstring statusText;

    std::thread recv([&] {
        std::unique_ptr<rgc::Reassembler> reasm; // tạo sau khi biết fps đàm phán
        std::unique_ptr<IVideoDecoder> decoder;  // tạo trên thread này, chỉ Decode() trên thread Decode

        // Ước lượng trễ e2e — đọc từ cả thread Recv (ghi) và thread Decode (đọc trong onDecoded).
        std::atomic<int64_t>  ackDeltaUs{0};  // t_client_nhận_ACK − timebase_host
        std::atomic<uint32_t> minRttUs{0};    // 0 = chưa có PONG nào
        std::atomic<int64_t>  lastE2eUs{-1};

        // Thống kê cửa sổ 1s.
        uint64_t stBytes = 0;                 // byte payload video nhận được (chỉ thread Recv)
        std::atomic<uint32_t> stRendered{0};  // ghi từ thread Decode, đọc từ thread Recv

        // GD6: hàng đợi frame đã ghép xong -> thread Decode. Bơm bởi thread Recv (PopReady),
        // rút bởi thread Decode (Decode + Render). Giới hạn kMaxQueuedFrames để không phình
        // vô hạn khi GPU chậm hơn tốc độ khung hình tới.
        constexpr size_t kMaxQueuedFrames = 3;
        std::mutex decQueueMutex;
        std::condition_variable decQueueCv;
        std::deque<rgc::Reassembler::Frame> decQueue;
        std::atomic<bool> decodeThreadStop{false};
        std::atomic<bool> decodeFailedFlag{false};   // thread Decode báo: Decode() lỗi -> xin IDR
        std::atomic<bool> queueOverflowFlag{false};  // thread Recv báo: đã bỏ frame vì đầy hàng đợi

        auto onDecoded = [&](const DecodedFrame& df) {
            if (!rendererReady.load(std::memory_order_acquire)) return;
            if (!renderer.RenderNV12(df.texture, df.subresource, df.width, df.height)) return;
            stRendered.fetch_add(1, std::memory_order_relaxed);
            const uint32_t rtt = minRttUs.load(std::memory_order_relaxed);
            if (rtt) {
                const int64_t offset = ackDeltaUs.load(std::memory_order_relaxed) - int64_t(rtt) / 2;
                lastE2eUs.store(int64_t(QpcUs()) - offset - int64_t(df.timestampUs));
            }
        };

        // Thread Decode (GD6): rút frame từ hàng đợi -> MfDecoder.Decode -> onDecoded ->
        // Renderer.RenderNV12. Tách khỏi thread Recv để decode+render GPU chậm không làm
        // ngừng recvfrom (xem comment đầu file).
        std::thread decodeThread([&] {
            for (;;) {
                rgc::Reassembler::Frame f;
                {
                    std::unique_lock<std::mutex> lk(decQueueMutex);
                    decQueueCv.wait(lk, [&] { return decodeThreadStop.load() || !decQueue.empty(); });
                    if (decQueue.empty()) {
                        if (decodeThreadStop.load()) return;
                        continue;
                    }
                    f = std::move(decQueue.front());
                    decQueue.pop_front();
                }
                if (!decoder) continue; // không nên xảy ra: decoder tạo trước khi có frame đầu

                const uint64_t decStartUs = QpcUs();
                const bool decodeOk = decoder->Decode(f.nal.data(), f.nal.size(), f.timestampUs);
                const uint64_t decMs = (QpcUs() - decStartUs) / 1000;
                if (decMs > 20) {
                    std::printf("[Client] WARNING: decode+render took %llu ms for one frame\n",
                                (unsigned long long)decMs);
                }
                if (!decodeOk) decodeFailedFlag.store(true, std::memory_order_release);
            }
        });

        rgc::ClientCallbacks cb;
        cb.send = [&](std::span<const uint8_t> d) { sock.SendTo(opt.server, d.data(), d.size()); };
        cb.onReady = [&](const rgc::NegotiatedParams& np) {
            ackDeltaUs.store(int64_t(QpcUs()) - int64_t(np.timebaseUs), std::memory_order_relaxed);
            std::printf("[Client] Negotiation done: H264 %ux%u @%ufps, %.1f Mbps\n",
                        np.width, np.height, np.fps, np.bitrateBps / 1e6);
            negW.store(np.width);
            negH.store(np.height); // main thấy kích thước -> tạo cửa sổ preview
        };
        cb.onRtt = [&](uint32_t rttUs) {
            uint32_t cur = minRttUs.load(std::memory_order_relaxed);
            while ((!cur || rttUs < cur) &&
                   !minRttUs.compare_exchange_weak(cur, rttUs, std::memory_order_relaxed)) {}
        };
        cb.onDisconnect = [&](const char* reason) {
            std::printf("[Client] Disconnected: %s\n", reason);
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
            if (n < 0) { std::printf("[Client] Socket error.\n"); failed.store(true); break; }

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
                    // GD6: chỉ đẩy vào hàng đợi cho thread Decode, không Decode() ở đây —
                    // giữ thread Recv luôn rảnh để quay lại recvfrom ngay.
                    {
                        std::lock_guard<std::mutex> lk(decQueueMutex);
                        if (decQueue.size() >= kMaxQueuedFrames) {
                            // Thread Decode theo không kịp — bỏ frame cũ nhất. Chuỗi inter-frame
                            // đã đứt nên xin IDR luôn để hình phục hồi đúng thay vì vỡ khối.
                            decQueue.pop_front();
                            queueOverflowFlag.store(true, std::memory_order_release);
                        }
                        decQueue.push_back(std::move(*f));
                    }
                    decQueueCv.notify_one();
                }
                if (reasm->TakeLossEvent() || reasm->WaitingForIdr())
                    session.RequestKeyframe();
            }
            if (decodeFailedFlag.exchange(false, std::memory_order_acq_rel)) session.RequestKeyframe();
            if (queueOverflowFlag.exchange(false, std::memory_order_acq_rel)) session.RequestKeyframe();

            // Vét input do luồng Main gom được -> ClientSession đánh seq, Tick gửi.
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
                const uint32_t rendered = stRendered.load(std::memory_order_relaxed);
                std::printf("[Client] %2.0f fps | %6.0f kbps | dropped %llu frame | lost %4.1f%%"
                            " pkts | RTT %.1f ms | e2e ~%.1f ms\n",
                            rendered / secs,
                            stBytes * 8.0 / 1000.0 / secs,
                            (unsigned long long)(st.framesDropped - lastStats.framesDropped),
                            lossPct,
                            session.lastRttUs() / 1000.0,
                            e2e >= 0 ? e2e / 1000.0 : 0.0);

                // GD5: cùng số liệu đó, cho overlay trên cửa sổ preview (luồng Main
                // đọc ở dưới, có khóa nhẹ - giống hệt mô hình inputQueue/inputMutex).
                wchar_t statusBuf[160];
                swprintf(statusBuf, 160,
                    L"%2.0f fps | %5.0f kbps | lost %4.1f%% pkts | RTT %4.1f ms | e2e ~%4.1f ms",
                    rendered / secs, stBytes * 8.0 / 1000.0 / secs, lossPct,
                    session.lastRttUs() / 1000.0, e2e >= 0 ? e2e / 1000.0 : 0.0);
                {
                    std::lock_guard<std::mutex> lk(statsMutex);
                    statusText = statusBuf;
                }

                lastStats = st;
                stRendered.store(0, std::memory_order_relaxed);
                stBytes = 0;
                lastStatUs = now;
            }
        }

        // GD6: dừng thread Decode trước khi decoder (biến cục bộ trên thread này) hủy.
        decodeThreadStop.store(true);
        decQueueCv.notify_one();
        decodeThread.join();

        session.SendBye(); // best-effort; buf_ của session chỉ dùng trên thread này
        quit.store(true);
    });

    // --- Thread Main: tạo preview khi biết kích thước, bơm message ---
    std::wstring lastAppliedStatus; // tránh gọi SetWindowText mỗi vòng khi không đổi
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
                    // GD5: 2 nút overlay đi cùng đường với phím tắt F9/F10.
                    renderer.SetCommandHook([&](int id) {
                        if (id == Renderer::kBtnLock) input.ToggleRelativeMode();
                        else if (id == Renderer::kBtnPause) input.TogglePause();
                        renderer.SetToggleState(input.relativeMode(), !input.enabled());
                    });
                }
            } else {
                std::printf("[Client] VIEW ONLY - not sending input (--noinput).\n");
            }
            rendererReady.store(true, std::memory_order_release);
        }
        if (renderer.Closed()) { quit.store(true); break; }

        // GD5: dòng số liệu từ luồng Recv -> nhãn overlay (chỉ khi đã có cửa sổ).
        if (rendererReady.load(std::memory_order_relaxed)) {
            std::wstring t;
            {
                std::lock_guard<std::mutex> lk(statsMutex);
                t = statusText;
            }
            if (t != lastAppliedStatus) {
                renderer.SetStatusText(t.c_str());
                lastAppliedStatus = t;
            }
        }
        Sleep(2);
    }

    quit.store(true);
    input.Detach(); // thả chuột/con trỏ TRƯỚC khi hủy cửa sổ preview
    renderer.SetMessageHook(nullptr);
    recv.join();
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    std::printf("[Client] Stopped.\n");
    return failed.load() ? 1 : 0;
}
