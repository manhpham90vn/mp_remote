// ClientLoop - vòng đời phía client:
//
//   Thread Main: bơm message cho MỌI cửa sổ preview (PeekMessage không lọc theo
//       HWND nên một vòng Pump phục vụ hết), tạo cửa sổ khi phiên tương ứng đàm
//       phán xong, và lái InputCapture theo cửa sổ đang focus.
//   Mỗi nguồn có một Thread Recv riêng (vòng chính của phiên đó):
//       recvfrom (timeout 10ms)
//       ├─ gói Video/FEC -> ClientSession.NotifyVideoPacket + Reassembler.Push(Fec)
//       │                   -> PopReady -> đẩy vào hàng đợi cho Thread Decode
//       ├─ gói Control   -> ClientSession.HandlePacket (HELLO_ACK/PONG/RECONFIG/BYE)
//       └─ mỗi vòng      -> mất gói? xin IDR ; Tick ; thống kê + FEEDBACK mỗi 1s
//   Mỗi nguồn có một Thread Decode riêng: rút frame từ hàng đợi -> MfDecoder.Decode
//       -> Renderer.RenderNV12. Tách khỏi Thread Recv vì decode+render GPU có thể
//       mất vài chục ms khi máy bận — nếu chặn Thread Recv thì recvfrom ngừng nghe
//       đúng lúc đó, UDP buffer của OS đầy rồi tràn, gây mất gói THẬT chứ không
//       còn là "giả" nữa. Hàng đợi giới hạn kMaxQueuedFrames: theo không kịp thì
//       bỏ frame cũ nhất và xin IDR (chuỗi inter-frame đã đứt).
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

// Toàn bộ trạng thái của MỘT nguồn đang xem. Chứa mutex/atomic/thread nên không
// copy/move được — giữ trong vector<unique_ptr>.
struct ClientStream {
    rgc::SourceInfo src;
    UdpSocket sock;
    Renderer  renderer;

    std::atomic<bool>     rendererReady{false};
    std::atomic<uint32_t> negW{0}, negH{0}; // kích thước đàm phán — main tạo renderer
    std::atomic<bool>     quit{false};
    std::atomic<bool>     failed{false};

    // Input do luồng Main gom -> luồng Recv đánh seq và gửi. Khóa chỉ giữ vài chục
    // nano giây quanh push/swap, không nằm trên đường nóng của video.
    std::mutex inputMutex;
    std::vector<rgc::InputEvent> inputQueue;

    // Dòng số liệu cho overlay: ghi từ luồng Recv, đọc từ luồng Main.
    std::mutex   statsMutex;
    std::wstring statusText;
    std::wstring lastAppliedStatus; // chỉ luồng Main

    std::thread recvThread;
};

// Vòng đời mạng+giải mã của một nguồn. Chạy trên thread Recv riêng của nguồn đó.
void StreamRecvLoop(ClientStream& s, const ClientOptions& opt, ID3D11Device* device) {
    std::unique_ptr<rgc::Reassembler> reasm; // tạo sau khi biết fps đàm phán
    std::unique_ptr<IVideoDecoder> decoder;  // tạo ở đây, chỉ Decode() trên thread Decode

    // Ước lượng trễ e2e — ghi ở thread Recv, đọc ở thread Decode (trong onDecoded).
    std::atomic<int64_t>  ackDeltaUs{0};  // t_client_nhận_ACK − timebase_host
    std::atomic<uint32_t> minRttUs{0};    // 0 = chưa có PONG nào
    std::atomic<int64_t>  lastE2eUs{-1};

    uint64_t stBytes = 0;                 // byte payload video nhận được (chỉ thread Recv)
    std::atomic<uint32_t> stRendered{0};  // ghi từ thread Decode, đọc từ thread Recv

    constexpr size_t kMaxQueuedFrames = 3;
    std::mutex decQueueMutex;
    std::condition_variable decQueueCv;
    std::deque<rgc::Reassembler::Frame> decQueue;
    std::atomic<bool> decodeThreadStop{false};
    std::atomic<bool> decodeFailedFlag{false};   // Decode() lỗi -> xin IDR
    std::atomic<bool> queueOverflowFlag{false};  // đã bỏ frame vì đầy hàng đợi

    auto onDecoded = [&](const DecodedFrame& df) {
        if (!s.rendererReady.load(std::memory_order_acquire)) return;
        if (!s.renderer.RenderNV12(df.texture, df.subresource, df.width, df.height)) return;
        stRendered.fetch_add(1, std::memory_order_relaxed);
        const uint32_t rtt = minRttUs.load(std::memory_order_relaxed);
        if (rtt) {
            const int64_t offset = ackDeltaUs.load(std::memory_order_relaxed) - int64_t(rtt) / 2;
            lastE2eUs.store(int64_t(QpcUs()) - offset - int64_t(df.timestampUs));
        }
    };

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
            if (!decoder) continue; // không nên xảy ra: decoder tạo trước frame đầu

            const uint64_t decStartUs = QpcUs();
            const bool decodeOk = decoder->Decode(f.nal.data(), f.nal.size(), f.timestampUs);
            const uint64_t decMs = (QpcUs() - decStartUs) / 1000;
            if (decMs > 20) {
                std::printf("[Client][%s] WARNING: decode+render took %llu ms for one frame\n",
                            s.src.name.c_str(), (unsigned long long)decMs);
            }
            if (!decodeOk) decodeFailedFlag.store(true, std::memory_order_release);
        }
    });

    rgc::ClientCallbacks cb;
    cb.send = [&](std::span<const uint8_t> d) { s.sock.SendTo(opt.server, d.data(), d.size()); };
    cb.onReady = [&](const rgc::NegotiatedParams& np) {
        ackDeltaUs.store(int64_t(QpcUs()) - int64_t(np.timebaseUs), std::memory_order_relaxed);
        std::printf("[Client][%s] Negotiation done: H264 %ux%u @%ufps, %.1f Mbps\n",
                    s.src.name.c_str(), np.width, np.height, np.fps, np.bitrateBps / 1e6);
        s.negW.store(np.width);
        s.negH.store(np.height); // main thấy kích thước -> tạo cửa sổ preview
    };
    cb.onReconfig = [&](const rgc::NegotiatedParams& np) {
        std::printf("[Client][%s] Host reconfigured: %ux%u, %.1f Mbps\n",
                    s.src.name.c_str(), np.width, np.height, np.bitrateBps / 1e6);
        s.negW.store(np.width);
        s.negH.store(np.height);
        // Không dựng lại decoder/renderer: MfDecoder tự đàm phán lại kích thước khi
        // gặp SPS mới (MF_E_TRANSFORM_STREAM_CHANGE) và Renderer tự tạo lại video
        // processor theo kích thước frame giải mã (EnsureVideoProcessor).
    };
    cb.onRtt = [&](uint32_t rttUs) {
        uint32_t cur = minRttUs.load(std::memory_order_relaxed);
        while ((!cur || rttUs < cur) &&
               !minRttUs.compare_exchange_weak(cur, rttUs, std::memory_order_relaxed)) {}
    };
    cb.onDisconnect = [&](const char* reason) {
        std::printf("[Client][%s] Disconnected: %s\n", s.src.name.c_str(), reason);
        s.quit.store(true);
    };
    rgc::ClientSession session(cb);

    rgc::Hello hello;
    hello.clientId   = uint32_t(QpcUs()) ^ GetCurrentProcessId() ^ (uint32_t(s.src.sourceId) << 24);
    hello.codecMask  = rgc::kCodecMaskH264;
    hello.maxWidth   = uint16_t(GetSystemMetrics(SM_CXSCREEN));
    hello.maxHeight  = uint16_t(GetSystemMetrics(SM_CYSCREEN));
    hello.desiredFps = 60;
    hello.features   = 0;
    hello.sourceId   = s.src.sourceId;
    session.Start(hello, QpcUs());

    uint8_t buf[rgc::kMaxDatagram];
    uint64_t lastStatUs = QpcUs();
    rgc::Reassembler::Stats lastStats{};

    while (!s.quit.load() && !g_ctrlC.load() && !s.failed.load()) {
        NetAddr from;
        const int n = s.sock.RecvFrom(buf, sizeof(buf), from);
        const uint64_t now = QpcUs();
        if (n < 0) {
            std::printf("[Client][%s] Socket error.\n", s.src.name.c_str());
            s.failed.store(true);
            break;
        }

        if (n > 0) {
            const auto span = std::span<const uint8_t>(buf, size_t(n));
            const auto h = rgc::ParseCommonHeader(span);
            if (h && h->chan == rgc::Chan::Video) {
                if (h->sessionId == session.sessionId() && session.sessionId() != 0) {
                    const auto pl = rgc::PayloadOf(span);
                    if (!reasm) {
                        const uint32_t fps = session.params().fps ? session.params().fps : 60;
                        reasm = std::make_unique<rgc::Reassembler>(1'000'000 / fps);
                    }
                    if (h->type == rgc::MsgType::FecPacket) {
                        if (const auto v = rgc::ParseFecPacket(*h, pl)) {
                            session.NotifyVideoPacket(now);
                            reasm->PushFec(*v, now);
                            stBytes += v->parity.size();
                        }
                    } else if (const auto v = rgc::ParseVideoPacket(*h, pl)) {
                        session.NotifyVideoPacket(now);
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
                    decoder = CreateDecoder(device, dc, onDecoded);
                    if (!decoder) { s.failed.store(true); break; }
                }
                if (f->idr) session.CancelKeyframeRequest();
                // Chỉ đẩy vào hàng đợi, không Decode() ở đây — giữ thread Recv luôn
                // rảnh để quay lại recvfrom ngay.
                {
                    std::lock_guard<std::mutex> lk(decQueueMutex);
                    if (decQueue.size() >= kMaxQueuedFrames) {
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
                std::lock_guard<std::mutex> lk(s.inputMutex);
                batch.swap(s.inputQueue);
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
            const uint64_t recovered = st.packetsRecovered - lastStats.packetsRecovered;
            std::printf("[Client][%s] %2.0f fps | %6.0f kbps | dropped %llu frame | lost %4.1f%%"
                        " pkts | fec+%llu | RTT %.1f ms | e2e ~%.1f ms\n",
                        s.src.name.c_str(),
                        rendered / secs,
                        stBytes * 8.0 / 1000.0 / secs,
                        (unsigned long long)(st.framesDropped - lastStats.framesDropped),
                        lossPct,
                        (unsigned long long)recovered,
                        session.lastRttUs() / 1000.0,
                        e2e >= 0 ? e2e / 1000.0 : 0.0);

            wchar_t statusBuf[160];
            swprintf(statusBuf, 160,
                L"%2.0f fps | %5.0f kbps | lost %4.1f%% pkts | RTT %4.1f ms | e2e ~%4.1f ms",
                rendered / secs, stBytes * 8.0 / 1000.0 / secs, lossPct,
                session.lastRttUs() / 1000.0, e2e >= 0 ? e2e / 1000.0 : 0.0);
            {
                std::lock_guard<std::mutex> lk(s.statsMutex);
                s.statusText = statusBuf;
            }

            // Số liệu đó gửi ngược cho host để nó siết/nới bitrate. Gửi cả khi 0%
            // mất gói — host cần tín hiệu "đường thông" mới dám nới lên lại, im
            // lặng bị hiểu là mất kết nối.
            rgc::Feedback fb;
            fb.lostFrames      = uint16_t(st.framesDropped - lastStats.framesDropped);
            fb.lossPct         = uint8_t(lossPct + 0.5);
            fb.rttMs           = uint16_t(session.lastRttUs() / 1000);
            fb.recvBitrateKbps = uint32_t(stBytes * 8.0 / 1000.0 / secs);
            session.SendFeedback(fb);

            lastStats = st;
            stRendered.store(0, std::memory_order_relaxed);
            stBytes = 0;
            lastStatUs = now;
        }
    }

    // Dừng thread Decode trước khi decoder (biến cục bộ trên thread này) hủy.
    decodeThreadStop.store(true);
    decQueueCv.notify_one();
    decodeThread.join();

    session.SendBye(); // best-effort; buf_ của session chỉ dùng trên thread này
    s.quit.store(true);
}

} // namespace

bool QueryHostSources(const NetAddr& server, std::vector<rgc::SourceInfo>& out) {
    out.clear();
    UdpSocket sock;
    if (!sock.Open(0)) return false;
    sock.SetRecvTimeout(200);

    uint8_t buf[rgc::kMaxDatagram];
    const size_t qn = rgc::BuildListSources(buf);
    if (!qn) return false;

    // Phát lại mỗi 500ms trong ~3s: LIST_SOURCES đi trên UDP, gói đầu mất là chuyện
    // bình thường và người dùng đang đứng chờ ở hộp thoại.
    const uint64_t startUs = QpcUs();
    uint64_t lastSendUs = 0;
    while (QpcUs() - startUs < 3'000'000 && !g_ctrlC.load()) {
        const uint64_t now = QpcUs();
        if (now - lastSendUs >= 500'000) {
            lastSendUs = now;
            sock.SendTo(server, buf, qn);
        }
        NetAddr from;
        const int n = sock.RecvFrom(buf, sizeof(buf), from);
        if (n <= 0) continue;
        const auto span = std::span<const uint8_t>(buf, size_t(n));
        const auto h = rgc::ParseCommonHeader(span);
        if (!h || h->type != rgc::MsgType::SourceList) continue;

        rgc::SourceInfo tmp[rgc::kMaxSources];
        const size_t cnt = rgc::ParseSourceList(rgc::PayloadOf(span), tmp);
        for (size_t i = 0; i < cnt; ++i) out.push_back(std::move(tmp[i]));
        return true;
    }
    return false;
}

int RunClient(const ClientOptions& opt) {
    g_ctrlC.store(false);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    GpuChoice gpu;
    if (!CreateBestDevice({ GpuVendor::Nvidia, GpuVendor::Intel, GpuVendor::Amd }, gpu)) {
        std::printf("[Client] Failed to create D3D11 device.\n");
        return 1;
    }
    std::wprintf(L"[Client] GPU: %ls [%ls]\n", gpu.description.c_str(), GpuVendorName(gpu.vendor));

    // Nguồn cần xem. Rỗng = host chỉ chia sẻ một thứ / bản cũ -> xem nguồn 0.
    std::vector<rgc::SourceInfo> wanted = opt.sources;
    if (wanted.empty()) {
        rgc::SourceInfo s;
        s.sourceId = 0;
        s.name = "source 0";
        wanted.push_back(std::move(s));
    }
    if (wanted.size() > rgc::kMaxSources) wanted.resize(rgc::kMaxSources);

    std::vector<std::unique_ptr<ClientStream>> streams;
    for (auto& w : wanted) {
        auto s = std::make_unique<ClientStream>();
        s->src = std::move(w);
        if (!s->sock.Open(0)) { // cổng ngẫu nhiên, MỘT socket cho mỗi nguồn
            std::printf("[Client] Failed to open a socket for source %u.\n", s->src.sourceId);
            return 1;
        }
        // 10ms thay vì 100ms: input được gom và gửi trong Tick của vòng Recv, nên
        // timeout recvfrom chính là trần độ trễ input khi màn hình đang tĩnh (không
        // có gói video nào đánh thức vòng lặp). 100 wakeup/s là không đáng kể.
        s->sock.SetRecvTimeout(10);
        streams.push_back(std::move(s));
    }

    std::printf("[Client] Connecting to %s (%zu source(s)) ...\n",
                opt.server.ToString().c_str(), streams.size());

    for (auto& s : streams) {
        ClientStream* ps = s.get();
        ps->recvThread = std::thread([ps, &opt, &gpu] {
            StreamRecvLoop(*ps, opt, gpu.device.Get());
        });
    }

    // --- Thread Main: tạo cửa sổ khi biết kích thước, bơm message, lái input ---
    //
    // Raw Input đăng ký theo PROCESS chứ không theo cửa sổ: gọi Attach lần hai với
    // HWND khác sẽ hủy đăng ký của lần đầu. Nên chỉ có MỘT InputCapture, và nó được
    // gắn lại sang cửa sổ preview nào đang foreground. Đằng nào không dùng
    // RIDEV_INPUTSINK nên chỉ cửa sổ đang focus mới nhận được input.
    InputCapture input;
    ClientStream* inputOwner = nullptr;

    auto attachInputTo = [&](ClientStream* s) {
        if (inputOwner == s) return;
        input.Detach();
        inputOwner = nullptr;
        if (!s || !opt.sendInput) return;
        if (!input.Attach(s->renderer.Hwnd(), [s](const rgc::InputEvent& e) {
                std::lock_guard<std::mutex> lk(s->inputMutex);
                s->inputQueue.push_back(e);
            })) {
            return;
        }
        input.SetEnabled(true);
        inputOwner = s;
        std::printf("[Client] Input now goes to \"%s\".\n", s->src.name.c_str());
    };

    bool anyFailed = false;
    for (;;) {
        if (g_ctrlC.load()) break;

        // Một Pump phục vụ mọi cửa sổ preview: PeekMessage không lọc theo HWND.
        Renderer::Pump();

        bool anyAlive = false;
        for (auto& s : streams) {
            if (s->failed.load()) { anyFailed = true; continue; }

            // Đàm phán xong -> dựng cửa sổ preview (Renderer phải Init/Pump trên
            // luồng bơm message, tức là luồng này).
            if (!s->rendererReady.load() && s->negW.load() && !s->quit.load()) {
                wchar_t title[192];
                const int wn = MultiByteToWideChar(CP_UTF8, 0, s->src.name.c_str(), -1,
                                                   nullptr, 0);
                std::wstring wname(wn > 0 ? size_t(wn - 1) : 0, L'\0');
                if (wn > 0)
                    MultiByteToWideChar(CP_UTF8, 0, s->src.name.c_str(), -1, wname.data(), wn);
                swprintf(title, 192, L"%ls — %ux%u", wname.c_str(), s->negW.load(),
                         s->negH.load());
                if (!s->renderer.Init(gpu.device.Get(), s->negW.load(), s->negH.load(), title)) {
                    s->failed.store(true);
                    anyFailed = true;
                    continue;
                }
                if (opt.saveBmp) s->renderer.RequestDumpBmp("client.bmp");
                ClientStream* ps = s.get();
                s->renderer.SetMessageHook([&input, ps, &inputOwner](HWND h, UINT m, WPARAM w,
                                                                     LPARAM l) {
                    // Chỉ cửa sổ đang giữ input mới được tiêu thụ message của nó.
                    return inputOwner == ps && input.OnMessage(h, m, w, l);
                });
                s->renderer.SetCommandHook([&input, ps, &inputOwner](int id) {
                    if (inputOwner != ps) return;
                    if (id == Renderer::kBtnLock) input.ToggleRelativeMode();
                    else if (id == Renderer::kBtnPause) input.TogglePause();
                    ps->renderer.SetToggleState(input.relativeMode(), !input.enabled());
                });
                s->rendererReady.store(true, std::memory_order_release);
                if (!opt.sendInput)
                    std::printf("[Client][%s] VIEW ONLY - not sending input.\n",
                                s->src.name.c_str());
            }

            if (s->rendererReady.load() && s->renderer.Closed()) s->quit.store(true);
            if (!s->quit.load()) anyAlive = true;

            // Dòng số liệu từ luồng Recv -> nhãn overlay.
            if (s->rendererReady.load(std::memory_order_relaxed) && !s->quit.load()) {
                std::wstring t;
                {
                    std::lock_guard<std::mutex> lk(s->statsMutex);
                    t = s->statusText;
                }
                if (t != s->lastAppliedStatus) {
                    s->renderer.SetStatusText(t.c_str());
                    s->lastAppliedStatus = t;
                }
            }
        }
        if (!anyAlive) break;

        // Input đi theo cửa sổ preview đang foreground.
        if (opt.sendInput) {
            const HWND fg = GetForegroundWindow();
            ClientStream* owner = nullptr;
            for (auto& s : streams)
                if (s->rendererReady.load() && !s->quit.load() && s->renderer.Hwnd() == fg)
                    owner = s.get();
            // Không cửa sổ preview nào đang focus -> nhả input, người dùng gõ vào
            // máy mình như bình thường.
            attachInputTo(owner);
            if (inputOwner && inputOwner->quit.load()) attachInputTo(nullptr);
        }

        Sleep(2);
    }

    // Thả chuột/con trỏ TRƯỚC khi hủy cửa sổ preview.
    input.Detach();
    for (auto& s : streams) {
        s->quit.store(true);
        s->renderer.SetMessageHook(nullptr);
    }
    for (auto& s : streams)
        if (s->recvThread.joinable()) s->recvThread.join();

    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    std::printf("[Client] Stopped.\n");
    return anyFailed ? 1 : 0;
}
