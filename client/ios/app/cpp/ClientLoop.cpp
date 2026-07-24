// =============================================================================
// ClientLoop.cpp — cài đặt vòng đời phiên, thread Net và thread Decode (bản iOS).
//                  Port sát của client/android/.../ClientLoop.cpp; chỉ hai điểm chạm
//                  phần cứng đổi (VtDecoder thay MediaCodecDecoder; layer thay Surface).
//
// THREAD NÀO CHẠM GÌ:
//   Main   : server_, sourceId_ (chỉ trước khi thread chạy), layer_, winGen_,
//            đọc statusLine_/endReason_ dưới khoá.
//   Net    : sock_, ClientSession, Reassembler, LinkStats, ghi statusLine_/
//            endReason_ dưới khoá, đẩy vào decQueue_.
//   Decode : VtDecoder, rút khỏi decQueue_, đọc layer_ dưới khoá, ghi winAckGen_.
//
// VÒNG LẶP THREAD NET LÀM 6 VIỆC MỖI VÒNG, THEO ĐÚNG THỨ TỰ NÀY:
//   1. recvfrom (chặn tối đa 10 ms — trần độ trễ của Tick khi màn hình tĩnh).
//   2. Phân loại gói: kênh Video vào Reassembler, còn lại giao ClientSession.
//   3. Rút frame đã ghép đủ, đẩy sang thread Decode.
//   4. Gom các lý do cần xin IDR (mất gói, lỗi decoder, hàng đợi tràn).
//   5. session.Tick() — phát ping/input/keyframe theo lịch.
//   6. Mỗi giây: đóng cửa sổ thống kê, in log, gửi FEEDBACK.
//
// LIÊN QUAN: ClientLoop.h (kiến trúc thread + hai cơ chế đồng bộ)
// =============================================================================
#include "ClientLoop.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <memory>

#include "Log.h"
#include "deskhubp/Clock.h"

#include "deskhub/control/LinkStats.h"
#include "deskhub/input/KeyMap.h"
#include "deskhub/session/ClientSession.h"
#include "deskhub/wire/Wire.h"

ClientLoop::~ClientLoop() {
    Stop();
}

std::string ClientLoop::StatusLine() {
    std::lock_guard<std::mutex> lk(textMutex_);
    return statusLine_;
}

std::string ClientLoop::EndReason() {
    std::lock_guard<std::mutex> lk(textMutex_);
    return endReason_;
}

// Gõ một phím rời: xếp cặp event nhấn + nhả vào hàng đợi cho thread Net vét.
// Nhấn và nhả đi cùng một lô nên không có nguy cơ kẹt phím do người dùng — phần
// chống mất gói đã có InputSender gửi lặp lo.
void ClientLoop::QueueKeyTap(int32_t vk, int32_t scan) {
    deskhub::InputEvent down;
    down.type = deskhub::InputType::Key;
    down.timestampUs = NowUs();
    down.a = vk;
    down.b = scan;
    down.state = 1;
    deskhub::InputEvent up = down;
    up.state = 0;
    {
        std::lock_guard<std::mutex> lk(inputMutex_);
        inputQueue_.push_back(down);
        inputQueue_.push_back(up);
    }
    wantFocus_.store(true, std::memory_order_release);
}

// Chuột tuyệt đối: kẹp biên rồi xếp hàng. Move không phải state event nên mất gói
// cũng vô hại — event kế tiếp thay thế.
void ClientLoop::QueueMouseMoveAbs(int32_t nx, int32_t ny) {
    auto clamp = [](int32_t v) { return v < 0 ? 0 : (v > 65535 ? 65535 : v); };
    deskhub::InputEvent e;
    e.type = deskhub::InputType::MouseMove;
    e.timestampUs = NowUs();
    e.a = clamp(nx);
    e.b = clamp(ny);
    e.absolute = 1;
    {
        std::lock_guard<std::mutex> lk(inputMutex_);
        inputQueue_.push_back(e);
    }
    wantFocus_.store(true, std::memory_order_release);
}

void ClientLoop::QueueMouseButton(int32_t button, bool down) {
    deskhub::InputEvent e;
    e.type = deskhub::InputType::MouseButton;
    e.timestampUs = NowUs();
    e.a = button;
    e.state = down ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk(inputMutex_);
        inputQueue_.push_back(e);
    }
    wantFocus_.store(true, std::memory_order_release);
}

// Gõ một ký tự từ bàn phím ảo. Cả cụm [Shift↓] key↓ key↑ [Shift↑] vào hàng đợi
// trong MỘT lần giữ khóa để không bị event khác chen ngang giữa Shift↓ và Shift↑.
void ClientLoop::QueueCharTap(uint32_t codepoint) {
    const auto chord = deskhub::CharToKeyChord(codepoint);
    if (!chord) return; // ký tự ngoài bảng US-ASCII — không gõ được, bỏ qua
    const uint64_t now = NowUs();
    auto key = [now](int32_t vk, bool down) {
        deskhub::InputEvent e;
        e.type = deskhub::InputType::Key;
        e.timestampUs = now;
        e.a = vk;
        e.b = 0; // không có scancode thật — host lùi về wVk (xem KeyMap.h)
        e.state = down ? 1 : 0;
        return e;
    };
    {
        std::lock_guard<std::mutex> lk(inputMutex_);
        if (chord->shift) inputQueue_.push_back(key(deskhub::kVkShift, true));
        inputQueue_.push_back(key(chord->vk, true));
        inputQueue_.push_back(key(chord->vk, false));
        if (chord->shift) inputQueue_.push_back(key(deskhub::kVkShift, false));
    }
    wantFocus_.store(true, std::memory_order_release);
}

bool ClientLoop::Start(const NetAddr& server, uint8_t sourceId) {
    server_ = server;
    sourceId_ = sourceId;
    if (!sock_.Open(0)) { // cổng ngẫu nhiên
        LOGE("[Client] Failed to open socket.");
        return false;
    }
    // 10ms: trần độ trễ của Tick khi màn hình đang tĩnh và không có gói video nào
    // đánh thức vòng lặp.
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
    netThread_ = std::thread([this] { NetThread(); });
    LOGI("[Client] Connecting to %s (source %u) ...", server_.ToString().c_str(), sourceId_);
    return true;
}

// Dừng phiên và chờ cả hai thread thoát hẳn. Thứ tự bắt buộc: bật cờ quit_ TRƯỚC
// rồi mới đánh thức — đánh thức trước thì thread có thể kiểm tra cờ khi nó còn false
// rồi ngủ tiếp, và join() sẽ treo mãi.
void ClientLoop::Stop() {
    quit_.store(true);
    decCv_.notify_all();
    winCv_.notify_all();
    if (netThread_.joinable()) netThread_.join();
    if (decodeThread_.joinable()) decodeThread_.join();
    sock_.Close();
}

// Giao layer mới, hoặc nullptr khi app xuống nền / view biến mất.
//
// CHẶN main thread tới khi thread Decode xác nhận đã buông layer cũ — xem mục "bắt
// tay layer" ở ClientLoop.h. decodeExited_ là lối thoát: thread Decode chết trước
// khi ack thì main vẫn đi tiếp được, không treo vĩnh viễn.
void ClientLoop::SetLayer(void* layer) {
    std::unique_lock<std::mutex> lk(winMutex_);
    layer_ = layer;
    ++winGen_;
    winCv_.notify_all();
    decCv_.notify_all(); // thread Decode có thể đang chờ frame — đánh thức để nó ack
    winAckCv_.wait(lk, [this] { return winAckGen_ == winGen_ || decodeExited_; });
}

// ---------------------------------------------------------------------------
// Thread Decode
// ---------------------------------------------------------------------------
// Vòng lặp bốn bước, lặp tới khi quit_. Decoder dựng LƯỜI ở bước (3) — chỉ khi đã
// có đủ cả layer lẫn kích thước đàm phán, mà hai thứ đó đến từ hai thread khác nhau
// và không theo thứ tự cố định.
void ClientLoop::DecodeThread() {
    VtDecoder decoder;

    for (;;) {
        // 1) Layer đổi? Buông decoder cũ rồi ack cho main. Decoder luôn được dựng lại
        //    lười biếng ở bước (3) nên ở đây chỉ cần tắt.
        {
            std::unique_lock<std::mutex> lk(winMutex_);
            if (winAckGen_ != winGen_) {
                // Chụp lại thế hệ TRƯỚC khi nhả khoá: main có thể đổi layer lần nữa
                // trong lúc ta đang Shutdown, và ack nhầm thế hệ mới sẽ khiến lần đổi
                // đó bị bỏ qua vĩnh viễn.
                const uint64_t gen = winGen_;
                lk.unlock();
                decoder.Shutdown();
                lk.lock();
                winAckGen_ = gen;
                winAckCv_.notify_all();
                // Layer mới -> chuỗi inter-frame vô nghĩa với decoder vừa dựng lại.
                rebuildDecoder_.store(false);
                decodeFailed_.store(true, std::memory_order_release); // -> xin IDR
            }
        }

        if (quit_.load()) break;

        // 2) Lấy frame kế tiếp. Chờ có giới hạn để còn quay lại phục vụ (1).
        deskhub::Reassembler::Frame f;
        {
            std::unique_lock<std::mutex> lk(decMutex_);
            // wait_for chứ không wait: phải tỉnh dậy định kỳ để quay lại bước (1)
            // phục vụ yêu cầu đổi layer, kể cả khi không có frame nào tới.
            decCv_.wait_for(lk, std::chrono::milliseconds(20), [this] {
                return quit_.load() || !decQueue_.empty();
            });
            if (decQueue_.empty()) continue;
            f = std::move(decQueue_.front());
            decQueue_.pop_front();
        }

        // 3) Dựng decoder nếu chưa có (cần cả layer lẫn kích thước đàm phán).
        if (rebuildDecoder_.exchange(false) && decoder.IsOpen()) decoder.Shutdown();

        if (!decoder.IsOpen()) {
            const uint32_t w = negW_.load(), h = negH_.load();
            if (!w || !h) continue;

            void* lay = nullptr;
            {
                std::lock_guard<std::mutex> lk(winMutex_);
                lay = layer_;
            }
            // Không có layer (app ở nền) -> bỏ frame. An toàn: quay lại nền trước sẽ
            // ack layer mới ở (1) và kéo theo một yêu cầu IDR.
            if (!lay) continue;

            if (!decoder.Init(lay, int(w), int(h))) {
                decodeFailed_.store(true, std::memory_order_release);
                continue;
            }
        }

        // 4) Giải mã và hiển thị. Đo thời gian để phát hiện máy quá yếu hoặc decoder
        //    đang vật lộn; cộng vào bộ đếm cửa sổ 1s cho dòng [DIAG] (t_dec, docs/09).
        const uint64_t t0 = NowUs();
        const bool ok = decoder.Decode(f.nal.data(), f.nal.size(), f.timestampUs);
        const uint64_t decMs = (NowUs() - t0) / 1000;
        dgDecMsSum_.fetch_add(uint32_t(decMs), std::memory_order_relaxed);
        dgDecCount_.fetch_add(1, std::memory_order_relaxed);
        {
            uint32_t cur = dgDecMsMax_.load(std::memory_order_relaxed);
            while (uint32_t(decMs) > cur &&
                   !dgDecMsMax_.compare_exchange_weak(cur, uint32_t(decMs), std::memory_order_relaxed)) {}
        }
        if (decMs > 20)
            LOGW("[Client] decode+enqueue took %" PRIu64 " ms for one frame", decMs);

        if (!ok) {
            decodeFailed_.store(true, std::memory_order_release);
            decoder.Shutdown(); // dựng lại ở vòng sau
            continue;
        }

        if (const uint32_t n = decoder.TakeRenderedCount())
            stRendered_.fetch_add(n, std::memory_order_relaxed);

        // Trễ e2e: đo trên frame vừa enqueue, theo đồng hồ host đã hiệu chỉnh.
        // (Xem VtDecoder::lastRenderedPtsUs về caveat "enqueue vs lên màn hình".)
        // Hai đồng hồ không đồng bộ, nên ước lượng độ lệch:
        //   ackDeltaUs_ = đồng hồ ta - timebaseUs của host, chụp lúc nhận HELLO_ACK.
        //   offset      = ackDelta - rtt/2 (bù nửa vòng HELLO_ACK mất để tới nơi).
        //   e2e         = bây giờ - offset - pts của frame.
        // Dùng RTT NHỎ NHẤT từng thấy vì mẫu nhỏ nhất ít bị hàng đợi làm nhiễu nhất.
        const uint32_t rtt = minRttUs_.load(std::memory_order_relaxed);
        const uint64_t pts = decoder.lastRenderedPtsUs();
        if (rtt && pts) {
            const int64_t offset = ackDeltaUs_.load(std::memory_order_relaxed) - int64_t(rtt) / 2;
            lastE2eUs_.store(int64_t(NowUs()) - offset - int64_t(pts));
        }
    }

    decoder.Shutdown();
    // Cởi trói cho main nếu nó đang chờ ack trong SetLayer.
    {
        std::lock_guard<std::mutex> lk(winMutex_);
        decodeExited_ = true;
        winAckGen_ = winGen_;
    }
    winAckCv_.notify_all();
}

// ---------------------------------------------------------------------------
// Thread Net — bản port sát của StreamRecvLoop bên Windows/Android
// ---------------------------------------------------------------------------
// Các callback của ClientSession chạy TRÊN CHÍNH THREAD NÀY (bên trong HandlePacket/
// Tick), nên chúng đọc/ghi trạng thái thoải mái, chỉ cần khoá khi chạm vào thứ mà
// thread khác cũng chạm.
void ClientLoop::NetThread() {
    std::unique_ptr<deskhub::Reassembler> reasm; // tạo sau khi biết fps đàm phán

    deskhub::ClientCallbacks cb;
    cb.send = [this](std::span<const uint8_t> d) {
        sock_.SendTo(server_, d.data(), d.size());
    };
    cb.onReady = [this](const deskhub::NegotiatedParams& np) {
        ackDeltaUs_.store(int64_t(NowUs()) - int64_t(np.timebaseUs), std::memory_order_relaxed);
        LOGI("[Client] Negotiation done: H264 %ux%u @%ufps, %.1f Mbps",
            np.width, np.height, np.fps, np.bitrateBps / 1e6);
        negW_.store(np.width);
        negH_.store(np.height);
    };
    cb.onReconfig = [this](const deskhub::NegotiatedParams& np) {
        LOGI("[Client] Host reconfigured: %ux%u, %.1f Mbps",
            np.width, np.height, np.bitrateBps / 1e6);
        negW_.store(np.width);
        negH_.store(np.height);
        // VtDecoder dựng CMVideoFormatDescription cứng theo SPS cũ; đổi kích thước
        // giữa chừng thì dựng lại decoder là đường chắc nhất. Host gửi kèm IDR nên
        // không mất gì.
        rebuildDecoder_.store(true);
    };
    // Giữ RTT NHỎ NHẤT từng thấy (ước lượng trễ e2e). Vòng compare_exchange là cách
    // chuẩn cập nhật "giá trị nhỏ nhất" trên atomic mà không bao giờ ghi đè bằng số
    // lớn hơn.
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
    deskhub::ClientSession session(cb);

    deskhub::Hello hello;
    // clientId chỉ cần phân biệt hai client — đồng hồ micro-giây đủ ngẫu nhiên.
    hello.clientId = uint32_t(NowUs());
    hello.codecMask = deskhub::kCodecMaskH264;
    // Chưa biết kích thước layer lúc gửi HELLO (và host đằng nào cũng stream đúng
    // kích thước cửa sổ nguồn) -> khai trần rộng, để host tự quyết.
    hello.maxWidth = 3840;
    hello.maxHeight = 2160;
    hello.desiredFps = 60;
    hello.features = 0;
    hello.sourceId = sourceId_;
    session.Start(hello, NowUs());

    uint8_t buf[deskhub::kMaxDatagram];
    uint64_t stBytes = 0;
    deskhub::LinkStats linkStats(NowUs());

    // Bộ đếm chẩn đoán cửa sổ 1s, chỉ thread Net chạm (xem docs/09).
    uint32_t dgAsmMsSum = 0, dgAsmMsMax = 0, dgAsmCount = 0; // t_asm: mảnh đầu → ghép xong
    uint32_t dgDqDrop = 0;                                   // frame vứt vì hàng đợi đầy
    uint32_t dgLoopBusyMaxMs = 0;                            // vòng Net bận nhất
    uint64_t kfReqUs = 0;                                    // thời điểm bắt đầu xin keyframe; 0 = không treo

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
            const auto h = deskhub::ParseCommonHeader(span);
            // Kênh Video đi thẳng vào Reassembler, KHÔNG qua ClientSession — đường
            // nóng, mỗi giây hàng nghìn gói. ClientSession chỉ được báo bằng
            // NotifyVideoPacket để nuôi timeout và thoát khỏi trạng thái Starting.
            if (h && h->chan == deskhub::Chan::Video) {
                if (h->sessionId == session.sessionId() && session.sessionId() != 0) {
                    const auto pl = deskhub::PayloadOf(span);
                    // Dựng Reassembler lười: nó cần fps đàm phán để đặt mốc timeout.
                    if (!reasm) {
                        const uint32_t fps = session.params().fps ? session.params().fps : 60;
                        reasm = std::make_unique<deskhub::Reassembler>(1'000'000 / fps);
                        reasm->onFrameDrop = [](const deskhub::Reassembler::FrameDropInfo& d) {
                            static const char* const kReason[] =
                                {"timeout", "overtaken", "evicted", "pre_idr"};
                            const char* pos = "-";
                            if (d.missing) {
                                const bool head = d.firstMissing == 0;
                                const bool tail = d.lastMissing + 1 == d.total;
                                pos = head && tail ? "all" : tail ? "tail"
                                                         : head   ? "head"
                                                                  : "mid";
                            }
                            LOGW(
                                "[DIAG] evt=frame_drop id=%u reason=%s miss=%u/%u pos=%s"
                                " idr=%u waited_ms=%u got_bytes=%u",
                                d.frameId, kReason[size_t(d.reason)], d.missing, d.total,
                                pos, d.idr ? 1 : 0, d.waitedMs, d.bytesGot);
                        };
                    }
                    if (h->type == deskhub::MsgType::FecPacket) {
                        if (const auto v = deskhub::ParseFecPacket(*h, pl)) {
                            session.NotifyVideoPacket(now);
                            reasm->PushFec(*v, now);
                            stBytes += v->parity.size();
                        }
                    } else if (const auto v = deskhub::ParseVideoPacket(*h, pl)) {
                        session.NotifyVideoPacket(now);
                        reasm->Push(*v, now);
                        stBytes += v->payload.size();
                    }
                }
            } else if (h) {
                session.HandlePacket(span, now);
            }
        }

        // Gom mọi đường dẫn tới "xin IDR" về một chỗ, kèm lý do và mốc thời gian.
        // Gọi lặp là vô hại: chỉ log ở lần chuyển từ "không treo" sang "đang treo".
        auto requestKf = [&](const char* reason) {
            if (!kfReqUs) {
                kfReqUs = now;
                LOGI("[DIAG] evt=kf_req reason=%s", reason);
            }
            session.RequestKeyframe();
        };

        if (reasm) {
            // Vét hết frame đã đủ mảnh: một lần recvfrom có thể hoàn thành nhiều frame.
            while (auto f = reasm->PopReady(now)) {
                // IDR đã về → thôi xin keyframe, kẻo host phát IDR liên tục.
                if (f->idr) {
                    session.CancelKeyframeRequest();
                    if (kfReqUs) {
                        LOGI("[DIAG] evt=idr_rx bytes=%zu after_ms=%" PRIu64,
                            f->nal.size(), (now - kfReqUs) / 1000);
                        kfReqUs = 0;
                    }
                }
                // t_asm: mảnh đầu tiên tới → frame ghép xong và rời Reassembler.
                if (f->firstSeenUs) {
                    const uint32_t asmMs = uint32_t((now - f->firstSeenUs) / 1000);
                    dgAsmMsSum += asmMs;
                    ++dgAsmCount;
                    if (asmMs > dgAsmMsMax) dgAsmMsMax = asmMs;
                }
                {
                    std::lock_guard<std::mutex> lk(decMutex_);
                    // Hàng đợi đầy: VỨT frame CŨ NHẤT chứ không chặn thread Net và
                    // cũng không vứt frame vừa tới. Frame cũ nhất là frame lỗi thời
                    // nhất — giữ nó lại chỉ làm hình trễ thêm.
                    if (decQueue_.size() >= kMaxQueuedFrames) {
                        decQueue_.pop_front();
                        queueOverflow_.store(true, std::memory_order_release);
                        ++dgDqDrop;
                    }
                    decQueue_.push_back(std::move(*f));
                }
                decCv_.notify_one();
            }
            if (reasm->TakeLossEvent())
                requestKf("loss");
            else if (reasm->WaitingForIdr())
                requestKf("wait_idr");
        }
        // Hai lý do còn lại đến từ thread Decode. exchange() đọc-và-xoá nguyên tử.
        if (decodeFailed_.exchange(false, std::memory_order_acq_rel)) requestKf("dec_fail");
        if (queueOverflow_.exchange(false, std::memory_order_acq_rel)) requestKf("q_overflow");

        // Vét input do UI thread gom -> ClientSession đánh seq, Tick gửi. Đã từng có
        // input thì báo SET_FOCUS để host kéo cửa sổ nguồn lên foreground (SetFocused
        // tự lọc trùng nên gọi mỗi vòng là vô hại).
        {
            std::vector<deskhub::InputEvent> batch;
            {
                std::lock_guard<std::mutex> lk(inputMutex_);
                batch.swap(inputQueue_);
            }
            for (const auto& e : batch) session.QueueInput(e);
        }
        if (wantFocus_.load(std::memory_order_acquire)) session.SetFocused(true);

        session.Tick(now);
        if (session.state() == deskhub::ClientSession::State::Dead) break;

        phase_.store(session.state() == deskhub::ClientSession::State::Streaming
                         ? Phase::Streaming
                         : Phase::Connecting,
            std::memory_order_release);

        // Mỗi giây: chốt cửa sổ thống kê, in log, cập nhật overlay, gửi FEEDBACK.
        if (linkStats.Due(now)) {
            const auto st = reasm ? reasm->stats() : deskhub::Reassembler::Stats{};
            const uint32_t rendered = stRendered_.exchange(0, std::memory_order_relaxed);
            const deskhub::LinkWindow w = linkStats.Close(st, stBytes, rendered, now);
            const int64_t e2e = lastE2eUs_.load();

            LOGI("[Client] %2.0f fps | %6.0f kbps | dropped %" PRIu64
                 " frame | lost %4.1f%% pkts"
                 " | fec+%" PRIu64 " | RTT %.1f ms | e2e ~%.1f ms",
                w.fps,
                w.kbps,
                w.framesDropped,
                w.lossPct,
                w.packetsRecovered,
                session.lastRttUs() / 1000.0,
                e2e >= 0 ? e2e / 1000.0 : 0.0);

            // Bản gọn cho overlay trên màn hình (log giữ bản đầy đủ ở trên).
            char ui[160];
            std::snprintf(ui, sizeof(ui),
                "%.0f fps  %.1f Mbps  loss %.1f%%  RTT %.0f ms  e2e %.0f ms",
                w.fps,
                w.kbps / 1000.0,
                w.lossPct,
                session.lastRttUs() / 1000.0,
                e2e >= 0 ? e2e / 1000.0 : 0.0);
            {
                std::lock_guard<std::mutex> lk(textMutex_);
                statusLine_ = ui;
            }

            session.SendFeedback(deskhub::MakeFeedback(w, session.lastRttUs()));

            // Dòng chẩn đoán 1s (docs/09) — đọc-và-reset mọi bộ đếm cửa sổ.
            {
                const uint32_t dc = dgDecCount_.exchange(0, std::memory_order_relaxed);
                const uint32_t ds = dgDecMsSum_.exchange(0, std::memory_order_relaxed);
                const uint32_t dm = dgDecMsMax_.exchange(0, std::memory_order_relaxed);
                LOGI(
                    "[DIAG] evt=sum asm_ms=%.1f/%u dec_ms=%.1f/%u dq_drop=%u"
                    " late=%" PRIu64 " late_ms_avg=%.0f late_ms_max=%" PRIu64
                    " gap_ms_max=%u loop_busy_ms_max=%u",
                    dgAsmCount ? double(dgAsmMsSum) / dgAsmCount : 0.0, dgAsmMsMax,
                    dc ? double(ds) / dc : 0.0, dm,
                    dgDqDrop,
                    w.latePackets, w.lateMsAvg, w.lateMsMax,
                    reasm ? reasm->TakeMaxGapMs() : 0, dgLoopBusyMaxMs);
                dgAsmMsSum = dgAsmMsMax = dgAsmCount = 0;
                dgDqDrop = 0;
                dgLoopBusyMaxMs = 0;
            }

            stBytes = 0;
        }

        // Vòng này bận bao lâu. Thread Net nghẽn thì buffer UDP của kernel gánh —
        // tràn là mất gói thật.
        const uint32_t busyMs = uint32_t((NowUs() - now) / 1000);
        if (busyMs > dgLoopBusyMaxMs) dgLoopBusyMaxMs = busyMs;
        if (busyMs > 50) LOGW("[DIAG] evt=recv_stall busy_ms=%u", busyMs);
    }

    // Chào host trước khi đi, gửi một lần và không chờ hồi đáp. Nó giải phóng phiên
    // ngay, để lần kết nối sau không bị từ chối vì host tưởng còn client cũ.
    session.SendBye();
    quit_.store(true);
    decCv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(textMutex_);
        if (endReason_.empty()) endReason_ = "stopped";
    }
    phase_.store(Phase::Ended, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    LOGI("[Client] Session ended.");
}
