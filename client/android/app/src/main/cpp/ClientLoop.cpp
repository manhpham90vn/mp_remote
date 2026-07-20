#include "ClientLoop.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>

#include "Log.h"
#include "TimeUs.h"

#include "rgc/ClientSession.h"
#include "rgc/Wire.h"

ClientLoop::~ClientLoop() { Stop(); }

std::string ClientLoop::StatusLine() {
    std::lock_guard<std::mutex> lk(textMutex_);
    return statusLine_;
}

std::string ClientLoop::EndReason() {
    std::lock_guard<std::mutex> lk(textMutex_);
    return endReason_;
}

bool ClientLoop::Start(const NetAddr& server) {
    server_ = server;
    if (!sock_.Open(0)) { // cổng ngẫu nhiên
        LOGE("[Client] Failed to open socket.");
        return false;
    }
    // 10ms: giống bên Windows — trần độ trễ của Tick khi màn hình đang tĩnh và
    // không có gói video nào đánh thức vòng lặp.
    sock_.SetRecvTimeout(10);

    quit_.store(false);
    finished_.store(false);
    phase_.store(Phase::Connecting, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(textMutex_);
        statusLine_.clear();
        endReason_.clear();
    }
    decodeThread_ = std::thread([this] { DecodeThread(); });
    netThread_    = std::thread([this] { NetThread(); });
    LOGI("[Client] Connecting to %s ...", server_.ToString().c_str());
    return true;
}

void ClientLoop::Stop() {
    quit_.store(true);
    decCv_.notify_all();
    winCv_.notify_all();
    if (netThread_.joinable()) netThread_.join();
    if (decodeThread_.joinable()) decodeThread_.join();
    sock_.Close();
}

void ClientLoop::SetWindow(ANativeWindow* window) {
    std::unique_lock<std::mutex> lk(winMutex_);
    window_ = window;
    ++winGen_;
    winCv_.notify_all();
    decCv_.notify_all(); // thread Decode có thể đang chờ frame — đánh thức để nó ack
    // Chờ Decode buông surface cũ. Bỏ chờ nếu thread Decode đã thoát, nếu không
    // main sẽ treo vĩnh viễn ở đây.
    winAckCv_.wait(lk, [this] { return winAckGen_ == winGen_ || decodeExited_; });
}

// ---------------------------------------------------------------------------
// Thread Decode
// ---------------------------------------------------------------------------
void ClientLoop::DecodeThread() {
    MediaCodecDecoder decoder;

    for (;;) {
        // 1) Surface đổi? Buông codec cũ rồi ack cho main. Codec luôn được dựng lại
        //    lười biếng ở bước (3) nên ở đây chỉ cần tắt.
        {
            std::unique_lock<std::mutex> lk(winMutex_);
            if (winAckGen_ != winGen_) {
                const uint64_t gen = winGen_;
                lk.unlock();
                decoder.Shutdown();
                lk.lock();
                winAckGen_ = gen;
                winAckCv_.notify_all();
                // Surface mới -> chuỗi inter-frame vô nghĩa với codec vừa dựng lại.
                rebuildDecoder_.store(false);
                decodeFailed_.store(true, std::memory_order_release); // -> xin IDR
            }
        }

        if (quit_.load()) break;

        // 2) Lấy frame kế tiếp. Chờ có giới hạn để còn quay lại phục vụ (1).
        rgc::Reassembler::Frame f;
        {
            std::unique_lock<std::mutex> lk(decMutex_);
            decCv_.wait_for(lk, std::chrono::milliseconds(20), [this] {
                return quit_.load() || !decQueue_.empty();
            });
            if (decQueue_.empty()) continue;
            f = std::move(decQueue_.front());
            decQueue_.pop_front();
        }

        // 3) Dựng codec nếu chưa có (cần cả Surface lẫn kích thước đàm phán).
        if (rebuildDecoder_.exchange(false) && decoder.IsOpen()) decoder.Shutdown();

        if (!decoder.IsOpen()) {
            const uint32_t w = negW_.load(), h = negH_.load();
            if (!w || !h) continue;

            ANativeWindow* win = nullptr;
            {
                std::lock_guard<std::mutex> lk(winMutex_);
                win = window_;
            }
            // Không có surface (app ở nền) -> bỏ frame. An toàn: quay lại nền
            // trước sẽ ack surface mới ở (1) và kéo theo một yêu cầu IDR.
            if (!win) continue;

            if (!decoder.Init(win, int(w), int(h))) {
                decodeFailed_.store(true, std::memory_order_release);
                continue;
            }
            // Codec mới chỉ decode được từ IDR trở đi. Frame đang cầm chắc chắn là
            // IDR nếu Reassembler đang ở chế độ chờ IDR, nhưng không đảm bảo —
            // decode sai chỉ sinh vỡ hình vài chục ms rồi IDR tới, chấp nhận được.
        }

        const uint64_t t0 = NowUs();
        const bool ok = decoder.Decode(f.nal.data(), f.nal.size(), f.timestampUs);
        const uint64_t decMs = (NowUs() - t0) / 1000;
        if (decMs > 20)
            LOGW("[Client] decode+render took %" PRIu64 " ms for one frame", decMs);

        if (!ok) {
            decodeFailed_.store(true, std::memory_order_release);
            decoder.Shutdown(); // dựng lại ở vòng sau
            continue;
        }

        if (const uint32_t n = decoder.TakeRenderedCount())
            stRendered_.fetch_add(n, std::memory_order_relaxed);

        // Trễ e2e: đo trên frame VỪA LÊN MÀN HÌNH, theo đồng hồ host đã hiệu chỉnh.
        const uint32_t rtt = minRttUs_.load(std::memory_order_relaxed);
        const uint64_t pts = decoder.lastRenderedPtsUs();
        if (rtt && pts) {
            const int64_t offset = ackDeltaUs_.load(std::memory_order_relaxed) - int64_t(rtt) / 2;
            lastE2eUs_.store(int64_t(NowUs()) - offset - int64_t(pts));
        }
    }

    decoder.Shutdown();
    // Cởi trói cho main nếu nó đang chờ ack trong SetWindow.
    {
        std::lock_guard<std::mutex> lk(winMutex_);
        decodeExited_ = true;
        winAckGen_ = winGen_;
    }
    winAckCv_.notify_all();
}

// ---------------------------------------------------------------------------
// Thread Net — bản port sát của StreamRecvLoop bên Windows
// ---------------------------------------------------------------------------
void ClientLoop::NetThread() {
    std::unique_ptr<rgc::Reassembler> reasm; // tạo sau khi biết fps đàm phán

    rgc::ClientCallbacks cb;
    cb.send = [this](std::span<const uint8_t> d) {
        sock_.SendTo(server_, d.data(), d.size());
    };
    cb.onReady = [this](const rgc::NegotiatedParams& np) {
        ackDeltaUs_.store(int64_t(NowUs()) - int64_t(np.timebaseUs), std::memory_order_relaxed);
        LOGI("[Client] Negotiation done: H264 %ux%u @%ufps, %.1f Mbps",
             np.width, np.height, np.fps, np.bitrateBps / 1e6);
        negW_.store(np.width);
        negH_.store(np.height);
    };
    cb.onReconfig = [this](const rgc::NegotiatedParams& np) {
        LOGI("[Client] Host reconfigured: %ux%u, %.1f Mbps",
             np.width, np.height, np.bitrateBps / 1e6);
        negW_.store(np.width);
        negH_.store(np.height);
        // Khác MfDecoder (tự đàm phán lại qua MF_E_TRANSFORM_STREAM_CHANGE):
        // MediaCodec đã configure với kích thước cũ, đổi kích thước giữa chừng thì
        // dựng lại codec là đường chắc chắn nhất. Host gửi kèm IDR nên không mất gì.
        rebuildDecoder_.store(true);
    };
    cb.onRtt = [this](uint32_t rttUs) {
        uint32_t cur = minRttUs_.load(std::memory_order_relaxed);
        while ((!cur || rttUs < cur) &&
               !minRttUs_.compare_exchange_weak(cur, rttUs, std::memory_order_relaxed)) {}
    };
    cb.onDisconnect = [this](const char* reason) {
        LOGI("[Client] Disconnected: %s", reason);
        {
            std::lock_guard<std::mutex> lk(textMutex_);
            endReason_ = reason ? reason : "disconnected";
        }
        quit_.store(true);
    };
    rgc::ClientSession session(cb);

    rgc::Hello hello;
    hello.clientId   = uint32_t(NowUs());
    hello.codecMask  = rgc::kCodecMaskH264;
    // Chưa biết kích thước surface lúc gửi HELLO (và đằng nào host cũng stream đúng
    // kích thước cửa sổ nguồn) -> khai trần rộng rãi, để host tự quyết.
    hello.maxWidth   = 3840;
    hello.maxHeight  = 2160;
    hello.desiredFps = 60;
    hello.features   = 0;
    hello.sourceId   = 0; // view-only v1: luôn nguồn đầu tiên
    session.Start(hello, NowUs());

    uint8_t buf[rgc::kMaxDatagram];
    uint64_t stBytes = 0;
    uint64_t lastStatUs = NowUs();
    rgc::Reassembler::Stats lastStats{};

    while (!quit_.load()) {
        NetAddr from;
        const int n = sock_.RecvFrom(buf, sizeof(buf), from);
        const uint64_t now = NowUs();
        if (n < 0) {
            LOGE("[Client] Socket error.");
            std::lock_guard<std::mutex> lk(textMutex_);
            endReason_ = "socket error";
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
                if (f->idr) session.CancelKeyframeRequest();
                {
                    std::lock_guard<std::mutex> lk(decMutex_);
                    if (decQueue_.size() >= kMaxQueuedFrames) {
                        decQueue_.pop_front();
                        queueOverflow_.store(true, std::memory_order_release);
                    }
                    decQueue_.push_back(std::move(*f));
                }
                decCv_.notify_one();
            }
            if (reasm->TakeLossEvent() || reasm->WaitingForIdr()) session.RequestKeyframe();
        }
        if (decodeFailed_.exchange(false, std::memory_order_acq_rel)) session.RequestKeyframe();
        if (queueOverflow_.exchange(false, std::memory_order_acq_rel)) session.RequestKeyframe();

        session.Tick(now);
        if (session.state() == rgc::ClientSession::State::Dead) break;

        phase_.store(session.state() == rgc::ClientSession::State::Streaming
                         ? Phase::Streaming
                         : Phase::Connecting,
                     std::memory_order_release);

        if (now - lastStatUs >= 1'000'000) {
            const double secs = (now - lastStatUs) / 1e6;
            const auto st = reasm ? reasm->stats() : rgc::Reassembler::Stats{};
            const uint64_t pkts = st.packetsReceived - lastStats.packetsReceived;
            const uint64_t lost = st.packetsLost - lastStats.packetsLost;
            const double lossPct = (pkts + lost) ? 100.0 * lost / double(pkts + lost) : 0.0;
            const int64_t e2e = lastE2eUs_.load();
            const uint32_t rendered = stRendered_.exchange(0, std::memory_order_relaxed);

            LOGI("[Client] %2.0f fps | %6.0f kbps | dropped %" PRIu64 " frame | lost %4.1f%% pkts"
                 " | fec+%" PRIu64 " | RTT %.1f ms | e2e ~%.1f ms",
                 rendered / secs,
                 stBytes * 8.0 / 1000.0 / secs,
                 st.framesDropped - lastStats.framesDropped,
                 lossPct,
                 st.packetsRecovered - lastStats.packetsRecovered,
                 session.lastRttUs() / 1000.0,
                 e2e >= 0 ? e2e / 1000.0 : 0.0);

            // Bản gọn cho overlay trên màn hình (logcat giữ bản đầy đủ ở trên).
            char ui[160];
            std::snprintf(ui, sizeof(ui),
                          "%.0f fps  %.1f Mbps  loss %.1f%%  RTT %.0f ms  e2e %.0f ms",
                          rendered / secs,
                          stBytes * 8.0 / 1e6 / secs,
                          lossPct,
                          session.lastRttUs() / 1000.0,
                          e2e >= 0 ? e2e / 1000.0 : 0.0);
            {
                std::lock_guard<std::mutex> lk(textMutex_);
                statusLine_ = ui;
            }

            // Gửi cả khi mất gói 0% — host cần tín hiệu "đường thông" mới dám nới
            // bitrate lên lại; im lặng bị hiểu là mất kết nối.
            rgc::Feedback fb;
            fb.lostFrames      = uint16_t(st.framesDropped - lastStats.framesDropped);
            fb.lossPct         = uint8_t(lossPct + 0.5);
            fb.rttMs           = uint16_t(session.lastRttUs() / 1000);
            fb.recvBitrateKbps = uint32_t(stBytes * 8.0 / 1000.0 / secs);
            session.SendFeedback(fb);

            lastStats = st;
            stBytes = 0;
            lastStatUs = now;
        }
    }

    session.SendBye(); // best-effort; buf_ của session chỉ dùng trên thread này
    quit_.store(true);
    decCv_.notify_all();
    {
        // Kết thúc mà chưa ai ghi lý do (vd. người dùng bấm Back) — vẫn phải có
        // chữ để UI hiển thị, không để trống.
        std::lock_guard<std::mutex> lk(textMutex_);
        if (endReason_.empty()) endReason_ = "stopped";
    }
    phase_.store(Phase::Ended, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    LOGI("[Client] Session ended.");
}
