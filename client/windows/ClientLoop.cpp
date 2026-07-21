// =============================================================================
// ClientLoop.cpp — vai trò CLIENT. Nơi mọi thứ của phía xem được ghép lại.
//
// NHIỆM VỤ
//   Đối xứng với AgentLoop: nối UdpSocket + ClientSession + Reassembler +
//   MfDecoder + Renderer + InputCapture thành một phiên xem chạy được. Không cài
//   đặt thuật toán nào, chỉ điều phối và quản lý luồng.
//
// ⚠ KIẾN TRÚC LUỒNG — với N nguồn thì có 2N+1 thread
//
//   THREAD MAIN (một, dùng chung):
//       Bơm message cho MỌI cửa sổ preview — PeekMessage không lọc theo HWND nên
//       một vòng Pump phục vụ hết (xem Renderer::Pump). Tạo cửa sổ khi phiên tương
//       ứng đàm phán xong, và lái InputCapture theo cửa sổ đang focus.
//
//   THREAD RECV (mỗi nguồn một cái) — vòng chính của phiên đó:
//       recvfrom (timeout 10ms)
//       ├─ gói Video/FEC → ClientSession.NotifyVideoPacket + Reassembler.Push(Fec)
//       │                  → PopReady → đẩy vào hàng đợi cho Thread Decode
//       ├─ gói Control   → ClientSession.HandlePacket (HELLO_ACK/PONG/RECONFIG/BYE)
//       └─ mỗi vòng      → mất gói? xin IDR ; Tick ; thống kê + FEEDBACK mỗi 1s
//
//   THREAD DECODE (mỗi nguồn một cái):
//       dựng decoder (lười, ~150ms — vì vậy KHÔNG dựng trên thread Recv)
//       → rút frame khỏi hàng đợi → MfDecoder.Decode → Renderer.RenderNV12.
//
// VÌ SAO DECODE PHẢI TÁCH KHỎI RECV — lý do quan trọng nhất của cả file
//   Decode + render GPU có thể mất vài chục mili-giây khi máy bận. Nếu việc đó chặn
//   Thread Recv thì recvfrom ngừng nghe đúng lúc đó, buffer UDP của hệ điều hành
//   đầy rồi tràn, và ta mất gói THẬT — loại mất mát mà FEC lẫn xin IDR đều không
//   cứu được, vì gói bị vứt trước khi đến tay chương trình.
//   Hàng đợi giới hạn kMaxQueuedFrames: theo không kịp thì BỎ frame cũ nhất và xin
//   IDR (chuỗi inter-frame đã đứt). Thà bỏ hình còn hơn nghẽn đường nhận.
//   (Bản Android dựng đúng kiến trúc này — xem client/android/.../ClientLoop.h.)
//
// ƯỚC LƯỢNG TRỄ E2E (docs/06 §7)
//   Hai đồng hồ không đồng bộ, nên phải ước lượng độ lệch D giữa chúng:
//       D   = (t_client_nhận_ACK − timebase_host) − RTT_min/2
//       e2e = now − D − ts_frame
//   Trừ RTT_min/2 vì HELLO_ACK mất nửa vòng để tới nơi. Dùng RTT NHỎ NHẤT từng thấy
//   vì đó là mẫu ít bị hàng đợi trên đường làm nhiễu nhất. Đây là con số để theo
//   dõi, không phải phép đo chính xác.
//
// LIÊN QUAN: ClientLoop.h, AgentLoop.cpp (phía đối diện), decode/Renderer.h,
//            input/InputCapture.h, rgc/session/ClientSession.h,
//            rgc/transport/Reassembler.h, docs/06 §5 §7
// =============================================================================
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

#include "capture/GpuSelect.h"
#include "input/InputCapture.h"
#include "decode/IVideoDecoder.h"
#include "decode/Renderer.h"
#include "rgcp/Clock.h"
#include "Diag.h"

#include "rgc/control/LinkStats.h"
#include "rgc/session/ClientSession.h"
#include "rgc/transport/Reassembler.h"

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
//
// ĐỌC KỸ CHÚ THÍCH TỪNG TRƯỜNG trước khi thêm trường mới: mỗi cái đều ghi rõ thread
// nào ghi và thread nào đọc. Ba loại dữ liệu qua ranh giới thread ở đây:
//   atomic đơn lẻ  — cờ và số đơn (negW/negH, quit, hasFocus...): không cần đồng bộ
//                    với gì khác nên atomic là đủ và rẻ.
//   inputMutex     — hàng đợi input: Main gom, Recv gửi.
//   statsMutex     — chuỗi hiển thị: Recv ghi, Main đọc.
// Đối chiếu với SourcePipeline bên AgentLoop.cpp — cùng khuôn tổ chức.
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

    // Cửa sổ preview của nguồn này có đang là cửa sổ nhận input không: đặt ở luồng
    // Main, luồng Recv soi mỗi vòng và báo host (SET_FOCUS) để host kéo cửa sổ nguồn
    // tương ứng lên foreground. ClientSession::SetFocused tự lọc trùng nên gọi mỗi
    // vòng là vô hại — khỏi phải bắt cạnh lên/xuống qua thread.
    std::atomic<bool> hasFocus{false};

    // Dòng số liệu cho overlay: ghi từ luồng Recv, đọc từ luồng Main.
    std::mutex   statsMutex;
    std::wstring statusText;
    std::wstring lastAppliedStatus; // chỉ luồng Main

    std::thread recvThread;
};

// Vòng đời mạng + giải mã của một nguồn. Chạy trên thread Recv riêng của nguồn đó.
//
// Hàm dài nhất file, gồm ba phần:
//   1. Dựng các callback của ClientSession (chúng chạy TRÊN CHÍNH THREAD NÀY, bên
//      trong HandlePacket/Tick — nên đọc/ghi trạng thái thoải mái, chỉ khoá khi
//      chạm vào thứ thread Main cũng chạm).
//   2. Gửi HELLO và phóng thread Decode khi đàm phán xong.
//   3. Vòng lặp chính: recvfrom → phân loại gói → rút frame → Tick → thống kê.
//
// Gói Video đi THẲNG vào Reassembler, không qua ClientSession — đó là đường nóng,
// mỗi giây hàng nghìn gói. ClientSession chỉ được báo bằng NotifyVideoPacket để
// nuôi timeout và thoát khỏi trạng thái Starting.
void StreamRecvLoop(ClientStream& s, const ClientOptions& opt, ID3D11Device* device) {
    std::unique_ptr<rgc::Reassembler> reasm; // tạo sau khi biết fps đàm phán
    // Decoder tạo VÀ dùng trên thread Decode: MfDecoder init mất ~150ms, dựng nó
    // trên thread Recv là recvfrom ngừng nghe đúng lúc IDR mở màn đang dồn về
    // (đo được recv_stall busy_ms=148, 2026-07-21). Tham số đàm phán đi qua bộ
    // atomic decW/decH/decFps — thread Recv ghi khi biết, thread Decode đọc khi dựng.
    std::unique_ptr<IVideoDecoder> decoder;
    std::atomic<uint32_t> decW{0}, decH{0}, decFps{0};

    // Ước lượng trễ e2e — ghi ở thread Recv, đọc ở thread Decode (trong onDecoded).
    std::atomic<int64_t>  ackDeltaUs{0};  // t_client_nhận_ACK − timebase_host
    std::atomic<uint32_t> minRttUs{0};    // 0 = chưa có PONG nào
    std::atomic<int64_t>  lastE2eUs{-1};

    uint64_t stBytes = 0;                 // byte payload video nhận được (chỉ thread Recv)
    std::atomic<uint32_t> stRendered{0};  // ghi từ thread Decode, đọc từ thread Recv

    constexpr size_t kMaxQueuedFrames = 3;
    std::mutex decQueueMutex;
    std::condition_variable decQueueCv;
    // Frame kèm thời điểm vào hàng — để đo t_queue (K1): nằm chờ lâu nghĩa là
    // thread Decode không theo kịp, khác hẳn với mạng chậm hay ghép chậm.
    struct QItem {
        rgc::Reassembler::Frame frame;
        uint64_t enqUs = 0;
    };
    std::deque<QItem> decQueue;
    std::atomic<bool> decodeThreadStop{false};
    std::atomic<bool> decodeFailedFlag{false};   // Decode() lỗi -> xin IDR
    std::atomic<bool> queueOverflowFlag{false};  // đã bỏ frame vì đầy hàng đợi

    // --- Chẩn đoán (docs/09), bộ đếm cửa sổ 1s ---
    // Thread Decode ghi (atomic), thread Recv đọc-và-reset ở khối thống kê:
    std::atomic<uint32_t> dgQMsSum{0}, dgQMsMax{0};     // t_queue: chờ trong hàng đợi
    std::atomic<uint32_t> dgDecMsSum{0}, dgDecMsMax{0}; // t_dec: decode + render
    std::atomic<uint32_t> dgDecCount{0};
    // Chỉ thread Recv chạm (không cần atomic):
    uint32_t dgAsmMsSum = 0, dgAsmMsMax = 0, dgAsmCount = 0; // t_asm: mảnh đầu → ghép xong
    uint32_t dgDqDepthMax = 0, dgDqDrop = 0;                 // K2: hàng đợi decode
    uint32_t dgLoopBusyMaxMs = 0;                            // K4: vòng Recv bận
    uint64_t kfReqUs = 0; // K3: thời điểm bắt đầu xin keyframe; 0 = không treo

    auto onDecoded = [&](const DecodedFrame& df) {
        if (!s.rendererReady.load(std::memory_order_acquire)) return;
        if (!s.renderer.RenderNV12(df.texture, df.subresource, df.width, df.height)) return;
        stRendered.fetch_add(1, std::memory_order_relaxed);
        const uint32_t rtt = minRttUs.load(std::memory_order_relaxed);
        if (rtt) {
            const int64_t offset = ackDeltaUs.load(std::memory_order_relaxed) - int64_t(rtt) / 2;
            lastE2eUs.store(int64_t(NowUs()) - offset - int64_t(df.timestampUs));
        }
    };

    std::thread decodeThread([&] {
        for (;;) {
            QItem it;
            {
                std::unique_lock<std::mutex> lk(decQueueMutex);
                decQueueCv.wait(lk, [&] { return decodeThreadStop.load() || !decQueue.empty(); });
                if (decQueue.empty()) {
                    if (decodeThreadStop.load()) return;
                    continue;
                }
                it = std::move(decQueue.front());
                decQueue.pop_front();
            }
            // Dựng decoder LƯỜI ở frame đầu, ngay trên thread này (xem chú thích ở
            // khai báo `decoder`). Frame chỉ vào hàng đợi sau khi đàm phán xong nên
            // decW/decH/decFps chắc chắn đã có giá trị.
            if (!decoder) {
                DecoderConfig dc;
                dc.codec  = Codec::H264;
                dc.width  = decW.load(std::memory_order_relaxed);
                dc.height = decH.load(std::memory_order_relaxed);
                dc.fps    = decFps.load(std::memory_order_relaxed);
                decoder = CreateDecoder(device, dc, onDecoded);
                if (!decoder) { s.failed.store(true); return; } // vòng Recv thấy failed
            }
            rgc::Reassembler::Frame& f = it.frame;

            // K1: t_queue = nằm chờ trong hàng đợi, t_dec = decode + render. Hai số
            // này tách "client đuối" khỏi "mạng chậm" khi mổ xẻ trễ e2e.
            const uint64_t decStartUs = NowUs();
            const uint32_t qMs = uint32_t((decStartUs - it.enqUs) / 1000);
            dgQMsSum.fetch_add(qMs, std::memory_order_relaxed);
            DiagAtomicMax(dgQMsMax, qMs);

            const bool decodeOk = decoder->Decode(f.nal.data(), f.nal.size(), f.timestampUs);
            const uint64_t decMs = (NowUs() - decStartUs) / 1000;
            dgDecMsSum.fetch_add(uint32_t(decMs), std::memory_order_relaxed);
            DiagAtomicMax(dgDecMsMax, uint32_t(decMs));
            dgDecCount.fetch_add(1, std::memory_order_relaxed);
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
        ackDeltaUs.store(int64_t(NowUs()) - int64_t(np.timebaseUs), std::memory_order_relaxed);
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
    hello.clientId   = uint32_t(NowUs()) ^ GetCurrentProcessId() ^ (uint32_t(s.src.sourceId) << 24);
    hello.codecMask  = rgc::kCodecMaskH264;
    hello.maxWidth   = uint16_t(GetSystemMetrics(SM_CXSCREEN));
    hello.maxHeight  = uint16_t(GetSystemMetrics(SM_CYSCREEN));
    hello.desiredFps = 60;
    hello.features   = 0;
    hello.sourceId   = s.src.sourceId;
    session.Start(hello, NowUs());

    uint8_t buf[rgc::kMaxDatagram];
    rgc::LinkStats linkStats(NowUs());

    while (!s.quit.load() && !g_ctrlC.load() && !s.failed.load()) {
        NetAddr from;
        const int n = s.sock.RecvFrom(buf, sizeof(buf), from);
        const uint64_t now = NowUs();
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
                        // Tham số cho thread Decode dựng decoder (ghi TRƯỚC khi bất
                        // kỳ frame nào có thể vào hàng đợi).
                        decW.store(session.params().width, std::memory_order_relaxed);
                        decH.store(session.params().height, std::memory_order_relaxed);
                        decFps.store(fps, std::memory_order_relaxed);
                        // C1: bản khám nghiệm từng frame bị khai tử.
                        reasm->onFrameDrop = [&s](const rgc::Reassembler::FrameDropInfo& d) {
                            static const char* const kReason[] =
                                {"timeout", "overtaken", "evicted", "pre_idr"};
                            // Vị trí chùm thiếu: đuôi frame là dấu vân tay của
                            // burst (docs/06 §7b) — nhìn pos= là biết ngay.
                            const char* pos = "-";
                            if (d.missing) {
                                const bool head = d.firstMissing == 0;
                                const bool tail = d.lastMissing + 1 == d.total;
                                pos = head && tail ? "all" : tail ? "tail"
                                                   : head ? "head" : "mid";
                            }
                            std::printf("[DIAG][%s] evt=frame_drop id=%u reason=%s"
                                        " miss=%u/%u pos=%s idr=%u waited_ms=%u"
                                        " got_bytes=%u\n",
                                        s.src.name.c_str(), d.frameId,
                                        kReason[size_t(d.reason)], d.missing, d.total,
                                        pos, d.idr ? 1 : 0, d.waitedMs, d.bytesGot);
                        };
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

        // K3: gom mọi đường dẫn tới "xin IDR" về một chỗ, kèm lý do và mốc thời
        // gian — để log cho thấy vòng xoáy IDR (xin dồn dập, IDR về chậm) nếu có.
        // Gọi lặp là vô hại: chỉ in ở lần chuyển từ "không treo" sang "đang treo".
        auto requestKf = [&](const char* reason) {
            if (!kfReqUs) {
                kfReqUs = now;
                std::printf("[DIAG][%s] evt=kf_req reason=%s\n", s.src.name.c_str(), reason);
            }
            session.RequestKeyframe();
        };

        if (reasm) {
            while (auto f = reasm->PopReady(now)) {
                if (f->idr) {
                    session.CancelKeyframeRequest();
                    // K3: IDR đã về — bao lâu kể từ lúc bắt đầu xin?
                    if (kfReqUs) {
                        std::printf("[DIAG][%s] evt=idr_rx bytes=%zu after_ms=%llu\n",
                                    s.src.name.c_str(), f->nal.size(),
                                    (unsigned long long)((now - kfReqUs) / 1000));
                        kfReqUs = 0;
                    }
                }
                // K1: t_asm = mảnh đầu tiên tới → frame ghép xong và rời Reassembler.
                if (f->firstSeenUs) {
                    const uint32_t asmMs = uint32_t((now - f->firstSeenUs) / 1000);
                    dgAsmMsSum += asmMs;
                    ++dgAsmCount;
                    if (asmMs > dgAsmMsMax) dgAsmMsMax = asmMs;
                }
                // Chỉ đẩy vào hàng đợi, không Decode() ở đây — giữ thread Recv luôn
                // rảnh để quay lại recvfrom ngay.
                {
                    std::lock_guard<std::mutex> lk(decQueueMutex);
                    if (decQueue.size() >= kMaxQueuedFrames) {
                        decQueue.pop_front();
                        queueOverflowFlag.store(true, std::memory_order_release);
                        ++dgDqDrop;
                    }
                    decQueue.push_back(QItem{std::move(*f), now});
                    if (decQueue.size() > dgDqDepthMax) dgDqDepthMax = uint32_t(decQueue.size());
                }
                decQueueCv.notify_one();
            }
            if (reasm->TakeLossEvent()) requestKf("loss");
            else if (reasm->WaitingForIdr()) requestKf("wait_idr");
        }
        if (decodeFailedFlag.exchange(false, std::memory_order_acq_rel)) requestKf("dec_fail");
        if (queueOverflowFlag.exchange(false, std::memory_order_acq_rel)) requestKf("q_overflow");

        // Vét input do luồng Main gom được -> ClientSession đánh seq, Tick gửi.
        {
            std::vector<rgc::InputEvent> batch;
            {
                std::lock_guard<std::mutex> lk(s.inputMutex);
                batch.swap(s.inputQueue);
            }
            for (const auto& e : batch) session.QueueInput(e);
        }

        session.SetFocused(s.hasFocus.load(std::memory_order_relaxed));
        session.Tick(now);
        if (session.state() == rgc::ClientSession::State::Dead) break;

        if (linkStats.Due(now)) {
            const auto st = reasm ? reasm->stats() : rgc::Reassembler::Stats{};
            const uint32_t rendered = stRendered.exchange(0, std::memory_order_relaxed);
            const rgc::LinkWindow w = linkStats.Close(st, stBytes, rendered, now);
            const int64_t e2e = lastE2eUs.load();

            std::printf("[Client][%s] %2.0f fps | %6.0f kbps | dropped %llu frame | lost %4.1f%%"
                        " pkts | fec+%llu | RTT %.1f ms | e2e ~%.1f ms\n",
                        s.src.name.c_str(),
                        w.fps,
                        w.kbps,
                        (unsigned long long)w.framesDropped,
                        w.lossPct,
                        (unsigned long long)w.packetsRecovered,
                        session.lastRttUs() / 1000.0,
                        e2e >= 0 ? e2e / 1000.0 : 0.0);

            wchar_t statusBuf[160];
            swprintf(statusBuf, 160,
                L"%2.0f fps | %5.0f kbps | lost %4.1f%% pkts | RTT %4.1f ms | e2e ~%4.1f ms",
                w.fps, w.kbps, w.lossPct,
                session.lastRttUs() / 1000.0, e2e >= 0 ? e2e / 1000.0 : 0.0);
            {
                std::lock_guard<std::mutex> lk(s.statsMutex);
                s.statusText = statusBuf;
            }

            // Số liệu đó gửi ngược cho host để nó siết/nới bitrate.
            session.SendFeedback(rgc::MakeFeedback(w, session.lastRttUs()));

            // Dòng chẩn đoán 1s (K1/K2/K4 + late/gap từ core) — đọc-và-reset mọi bộ
            // đếm cửa sổ. late= là con số phân xử "mất thật vs tới muộn" (docs/06 §7b).
            {
                const uint32_t dc = dgDecCount.exchange(0, std::memory_order_relaxed);
                const uint32_t qs = dgQMsSum.exchange(0, std::memory_order_relaxed);
                const uint32_t qm = dgQMsMax.exchange(0, std::memory_order_relaxed);
                const uint32_t ds = dgDecMsSum.exchange(0, std::memory_order_relaxed);
                const uint32_t dm = dgDecMsMax.exchange(0, std::memory_order_relaxed);
                std::printf("[DIAG][%s] evt=sum asm_ms=%.1f/%u q_ms=%.1f/%u dec_ms=%.1f/%u"
                            " dq_max=%u dq_drop=%u late=%llu late_ms_avg=%.0f late_ms_max=%llu"
                            " gap_ms_max=%u loop_busy_ms_max=%u\n",
                            s.src.name.c_str(),
                            dgAsmCount ? double(dgAsmMsSum) / dgAsmCount : 0.0, dgAsmMsMax,
                            dc ? double(qs) / dc : 0.0, qm,
                            dc ? double(ds) / dc : 0.0, dm,
                            dgDqDepthMax, dgDqDrop,
                            (unsigned long long)w.latePackets, w.lateMsAvg,
                            (unsigned long long)w.lateMsMax,
                            reasm ? reasm->TakeMaxGapMs() : 0, dgLoopBusyMaxMs);
                dgAsmMsSum = dgAsmMsMax = dgAsmCount = 0;
                dgDqDepthMax = dgDqDrop = 0;
                dgLoopBusyMaxMs = 0;
            }

            stBytes = 0;
        }

        // K4: vòng này bận bao lâu (từ lúc recvfrom trả về tới đây). Thread Recv
        // nghẽn thì buffer UDP của kernel gánh — tràn là mất gói thật.
        const uint32_t busyMs = uint32_t((NowUs() - now) / 1000);
        if (busyMs > dgLoopBusyMaxMs) dgLoopBusyMaxMs = busyMs;
        if (busyMs > 50)
            std::printf("[DIAG][%s] evt=recv_stall busy_ms=%u\n", s.src.name.c_str(), busyMs);
    }

    // Dừng thread Decode trước khi decoder (biến cục bộ trên thread này) hủy.
    decodeThreadStop.store(true);
    decQueueCv.notify_one();
    decodeThread.join();

    session.SendBye(); // best-effort; buf_ của session chỉ dùng trên thread này
    s.quit.store(true);
}

} // namespace

// Hỏi host đang chia sẻ những gì, TRƯỚC khi có phiên nào.
//
// Socket riêng, sống đúng trong hàm này — ClientStream chưa tồn tại lúc này. Cùng
// thiết kế với SourceQuery.cpp bên Android, kể cả hai mốc thời gian: hạn nhận
// 200 ms phải NGẮN HƠN nhịp phát lại 500 ms, nếu không vòng lặp ngủ quên trong
// recvfrom và bỏ lỡ thời điểm phát lại.
//
// Trả false = host im lặng suốt 3 giây. Không phải lỗi tử vong: có thể là host bản
// trước GĐ6 không biết LIST_SOURCES, và người gọi sẽ lùi về nguồn 0.
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
    const uint64_t startUs = NowUs();
    uint64_t lastSendUs = 0;
    while (NowUs() - startUs < 3'000'000 && !g_ctrlC.load()) {
        const uint64_t now = NowUs();
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

// Chạy trọn một phiên xem. CHẶN tới khi đóng hết cửa sổ preview / Ctrl+C / mất
// kết nối.
//
// Bốn giai đoạn:
//   1. Chọn GPU, chuẩn hoá danh sách nguồn muốn xem.
//   2. Dựng ClientStream + mở socket riêng cho từng nguồn.
//   3. Phóng cặp thread Recv (Decode được thread Recv phóng khi đàm phán xong).
//   4. Vòng Main: bơm message, tạo cửa sổ khi phiên sẵn sàng, lái InputCapture.
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
        // MỘT socket riêng cho mỗi nguồn (cổng ngẫu nhiên). Khác phía host — bên đó
        // dùng chung một cổng và tự định tuyến gói. Ở đây tách ra thì mỗi thread
        // Recv có socket của riêng nó, khỏi phải khoá hay phân phối gói giữa thread.
        if (!s->sock.Open(0)) {
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
        if (inputOwner) inputOwner->hasFocus.store(false, std::memory_order_relaxed);
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
        // Luồng Recv của nguồn này sẽ gửi SET_FOCUS -> host đưa cửa sổ nguồn lên
        // foreground, nếu không thì input bơm ở host rơi vào cửa sổ khác.
        s->hasFocus.store(true, std::memory_order_relaxed);
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
