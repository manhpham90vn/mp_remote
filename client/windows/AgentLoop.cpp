// =============================================================================
// AgentLoop.cpp — vai trò HOST. Nơi mọi thứ của phía chia sẻ được ghép lại.
//
// NHIỆM VỤ
//   Ghép chuỗi bắt hình/mã hoá (GĐ2) với tầng mạng (GĐ3), rồi nhân lên cho nhiều
//   nguồn (GĐ6). Đây là file điều phối lớn nhất phía host — bản thân nó không cài
//   đặt thuật toán nào, mà nối các mảnh đã có và quản lý luồng giữa chúng.
//
// ⚠ KIẾN TRÚC LUỒNG — điều quan trọng nhất phải nắm trước khi sửa
//
//   MỖI NGUỒN có một thread FrameArrived riêng (do WGC tạo):
//       capture → encoder → onPacket(NAL) → Packetizer → Pacer → sock.SendTo(peer)
//
//   MỘT thread Recv DÙNG CHUNG cho mọi nguồn (chính là thread gọi RunAgent):
//       recvfrom (timeout 100ms) → định tuyến gói → Tick mọi phiên → thống kê 1s/lần
//
//   Nghĩa là với N nguồn thì có N+1 thread, và mọi trạng thái đi qua ranh giới giữa
//   chúng phải là atomic hoặc được mutex bảo vệ. SourcePipeline bên dưới ghi rõ
//   từng trường thuộc về thread nào — ĐỌC PHẦN ĐÓ trước khi thêm trường mới.
//
// ĐỊNH TUYẾN GÓI ĐẾN — ba loại, ba cách tìm chủ
//   LIST_SOURCES → không thuộc phiên nào; trả SOURCE_LIST liệt kê mọi nguồn.
//   HELLO        → tìm theo hello.sourceId (lúc này chưa có sessionId).
//   Còn lại      → tìm theo sessionId, khớp với phiên của từng nguồn.
//
// VÌ SAO MỘT SOCKET CHUNG, KHÔNG PHẢI MỖI NGUỒN MỘT CỔNG
//   Người dùng chỉ phải mở một cổng firewall và chỉ phải nhớ một địa chỉ; client
//   hỏi LIST_SOURCES là thấy hết. Cái giá là phải tự định tuyến gói như trên.
//
// HAI CƠ CHẾ ĐÁNG CHÚ Ý
//   1. forceIdr là ATOMIC FLAG. Đặt từ thread Recv (onStart / onKeyframeRequest),
//      tiêu thụ ở lần Encode kế tiếp trên thread FrameArrived. Không gọi thẳng
//      encoder từ thread Recv được — nó thuộc thread kia (docs/06 §4).
//   2. CACHE FRAME CUỐI. WGC chỉ phát frame khi nội dung ĐỔI. Nguồn đang tĩnh
//      (menu, màn hình đứng im) mà client xin IDR thì không có frame nào để nén —
//      không cache thì client vào xem màn hình tĩnh sẽ đen VĨNH VIỄN.
//
// LIÊN QUAN: AgentLoop.h (AgentSource/AgentOptions), ClientLoop.cpp (phía đối
//            diện), rgc/session/HostSession.h, rgc/transport/Packetizer.h,
//            net/Pacer.h, docs/06-phase3-transport.md §4
// =============================================================================
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

#include "capture/GpuSelect.h"
#include "input/InputInjector.h"
#include "encode/IVideoEncoder.h"
#include "net/NetInfo.h"
#include "rgcp/Clock.h"
#include "net/UdpSocket.h"
#include "capture/WindowCapture.h"
#include "Diag.h"

#include "rgc/control/BitrateController.h"
#include "rgc/session/HostSession.h"
#include "rgc/transport/Packetizer.h"

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

// Cỡ khung nhỏ nhất còn encode được. Encoder phần cứng từ chối khung quá nhỏ —
// NVENC trả "status=8 Frame Dimension less than the minimum supported value", MF
// trả 0xC00D6D76 ở SetOutputType. Ca gặp thật: người dùng THU NHỎ cửa sổ đang share,
// WGC báo về 146x20 (log 21/07/2026).
//
// Ngưỡng đặt cao hơn mức tối thiểu thật của NVENC một quãng an toàn, vì hai backend
// có ngưỡng khác nhau và ta không muốn dò từng cái. Cửa sổ nhỏ hơn cỡ này thì có
// stream được cũng chẳng ai xem nổi.
inline constexpr uint32_t kMinEncodeW = 160;
inline constexpr uint32_t kMinEncodeH = 64;

// Toàn bộ trạng thái của MỘT nguồn. Chứa mutex/atomic nên không copy/move được —
// giữ trong vector<unique_ptr>.
struct SourcePipeline {
    SourcePipeline(uint32_t startBps, uint32_t minBps)
        : curBitrateBps(startBps), rate(startBps, minBps) {}

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
    // failed = HỎNG THẬT, một chiều: không có backend encoder, capture không start
    // được. Nguồn coi như chết tới hết phiên.
    std::atomic<bool>     failed{false};
    // paused = TẠM không encode được (nguồn nhỏ hơn kMinEncode*), HAI CHIỀU. Tách
    // khỏi `failed` vì trước đây gộp chung: cửa sổ thu nhỏ làm ensureEncoder hỏng →
    // failed=true → onFrame thoát ngay ở đầu hàm → không bao giờ thấy cửa sổ mở lại
    // → phiên chết vĩnh viễn dù nguồn đã bình thường trở lại (log 21/07/2026).
    std::atomic<bool>     paused{false};
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
    uint64_t lastKeepaliveUs = 0; // lần bơm lại frame cache gần nhất — chỉ thread Recv chạm

    // --- Congestion control, chỉ thread Recv chạm ---
    // Policy thuần ở core; curBitrateBps/wantFec ở trên là bản sao atomic cho thread
    // FrameArrived đọc (nó không được chạm vào rate).
    rgc::BitrateController rate;

    // --- Thống kê cửa sổ 1s, chỉ thread Recv chạm ---
    uint32_t lastCaptured = 0;
    uint64_t lastBytes = 0, lastFrames = 0;

    // --- Chẩn đoán (docs/09): bộ đếm cửa sổ 1s. Ghi từ thread FrameArrived (và
    // thread Recv lúc encode lại frame tĩnh), đọc-và-reset ở khối thống kê 1s. ---
    std::atomic<uint32_t> dgEncMsSum{0}, dgEncMsMax{0}, dgEncCount{0};
    std::atomic<uint32_t> dgBurstMsMax{0}; // thời gian bắn hết gói của MỘT frame
    std::atomic<uint32_t> dgSendFail{0};   // sendto trả lỗi (buffer gửi đầy...)
    std::atomic<uint32_t> dgIdrCount{0};
    // Sự kiện IDR gần nhất — thread FrameArrived ghi, thread Recv in (giữ I/O
    // ngoài đường nóng, bài học Pacer ở docs/06 §7b). bytes==0 = không có sự kiện;
    // ghi bytes CUỐI CÙNG với release để hai trường kia nhìn thấy trước nó.
    std::atomic<uint64_t> dgIdrBytes{0};
    std::atomic<uint32_t> dgIdrPkts{0}, dgIdrBurstMs{0};

    // Đo thời gian một lần Encode + cộng vào bộ đếm cửa sổ. Gọi từ CẢ HAI thread.
    void DiagEncode(IVideoEncoder* enc, ID3D11Texture2D* tex, bool idr) {
        const uint64_t t0 = NowUs();
        const bool ok = enc->Encode(tex, t0, idr);
        const uint32_t ms = uint32_t((NowUs() - t0) / 1000);
        dgEncMsSum.fetch_add(ms, std::memory_order_relaxed);
        dgEncCount.fetch_add(1, std::memory_order_relaxed);
        DiagAtomicMax(dgEncMsMax, ms);
        // Encode hỏng trên đường keepalive/IDR tĩnh trước giờ bị nuốt im lặng —
        // nguồn tĩnh mà encoder chết là client trắng hình không dấu vết.
        if (!ok)
            std::printf("[DIAG][%s] evt=enc_fail idr=%d ms=%u\n", name.c_str(), idr ? 1 : 0, ms);
    }
};

} // namespace

// Chạy trọn một phiên chia sẻ. CHẶN tới khi mọi nguồn đóng / Ctrl+C / lỗi.
//
// Sáu giai đoạn, đánh dấu bằng các mốc "--- ... ---" bên dưới:
//   1. Kiểm tra đầu vào, chọn GPU, mở socket.
//   2. Dựng SourcePipeline cho từng nguồn.
//   3. Khởi động capture — từ đây các thread FrameArrived bắt đầu chạy.
//   4. ĐỢI frame đầu của từng nguồn: phải biết kích thước thật rồi mới chào được
//      trong HELLO_ACK. Nguồn không phát frame nào trong 10 giây thì bỏ, không kéo
//      cả phiên xuống theo.
//   5. Dựng HostSession + InputInjector cho từng nguồn còn sống.
//   6. Vòng Recv — phần thân chính, chạy tới khi kết thúc.
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
    // Sàn bitrate: dưới mức này hình nát tới mức vô dụng, thà bỏ frame còn hơn.
    // Đây là tham số `minBps` của BitrateController — nó không bao giờ tụt quá đây.
    const uint32_t minBitrate   = 1'000'000u;

    std::vector<std::unique_ptr<SourcePipeline>> pipes;
    for (size_t i = 0; i < sources.size(); ++i) {
        auto p = std::make_unique<SourcePipeline>(startBitrate, minBitrate);
        p->sourceId = uint8_t(i);
        p->target   = sources[i].target;
        p->name     = sources[i].name;
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
            // Đo burst gửi (H2): từ gói đầu tới gói cuối của frame này, và bắt lỗi
            // sendto — trước đây trị trả về bị vứt, buffer gửi đầy là mất gói ngay
            // tại host mà không ai hay.
            const uint64_t sendT0 = NowUs();
            const size_t pkts = p->packetizer.SendFrame(
                std::span<const uint8_t>(data, size), p->nextFrameId++, tsUs, keyframe,
                [p, &sock, &peer](std::span<const uint8_t> d) {
                    if (sock.SendTo(peer, d.data(), d.size()))
                        p->bytesSent.fetch_add(d.size(), std::memory_order_relaxed);
                    else
                        p->dgSendFail.fetch_add(1, std::memory_order_relaxed);
                });
            const uint32_t burstMs = uint32_t((NowUs() - sendT0) / 1000);
            DiagAtomicMax(p->dgBurstMsMax, burstMs);
            if (pkts) p->framesSent.fetch_add(1, std::memory_order_relaxed);
            // Sự kiện IDR (H1): ghi lại cho thread Recv in — cỡ IDR là con số quyết
            // định chẩn đoán chùm mất gói (docs/06 §7b).
            if (pkts && keyframe) {
                p->dgIdrCount.fetch_add(1, std::memory_order_relaxed);
                p->dgIdrPkts.store(uint32_t(pkts), std::memory_order_relaxed);
                p->dgIdrBurstMs.store(burstMs, std::memory_order_relaxed);
                p->dgIdrBytes.store(uint64_t(size), std::memory_order_release);
            }
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

        // Đường nóng của nguồn này. CHẠY TRÊN THREAD FrameArrived CỦA WGC — không
        // phải thread Recv. Bốn việc, theo thứ tự:
        //   1. Làm chẵn kích thước (ràng buộc NV12).
        //   2. Phát hiện đổi kích thước → vứt encoder + cache, báo cho thread Recv.
        //   3. Chép frame vào cache (để còn cái mà encode khi nguồn đứng yên).
        //   4. Encode.
        // Giữ encMutex suốt từ bước 2: thread Recv cũng chạm vào encoder và cachedTex
        // khi nó phải encode lại frame tĩnh lúc client xin IDR.
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

            // Nguồn nhỏ hơn mức encoder nhận (cửa sổ vừa bị thu nhỏ). TRẠNG THÁI
            // TẠM, không phải lỗi: bỏ qua frame và giữ nguyên phiên.
            //
            // Chặn ở ĐÚNG chỗ này mới thoát được: trên nó là đoạn ghi nhận kích
            // thước — vẫn phải chạy, vì đó là thứ duy nhất cho ta biết cửa sổ đã mở
            // to trở lại. Dưới nó là cache + encode — đều vô nghĩa ở cỡ này. Thoát
            // sớm hơn (như `failed` làm) là tự bịt mắt: không còn đường nhận ra
            // nguồn đã bình thường, phiên treo vĩnh viễn.
            //
            // Không đụng cachedTex: đoạn đổi kích thước ở trên đã haveCached=false,
            // nên nhánh keepalive của thread Recv tự nhiên không bắn — khỏi phải
            // thêm điều kiện ở đó.
            if (encW < kMinEncodeW || encH < kMinEncodeH) {
                if (!p->paused.exchange(true, std::memory_order_acq_rel))
                    std::printf("[Agent][%s] Source too small to encode (%ux%u) —"
                                " paused, waiting for it to grow back.\n",
                                p->name.c_str(), encW, encH);
                return;
            }
            if (p->paused.exchange(false, std::memory_order_acq_rel))
                std::printf("[Agent][%s] Source back to %ux%u — resuming.\n",
                            p->name.c_str(), encW, encH);

            // Lưu bản SAO của frame cuối. Bắt buộc phải copy chứ không giữ con trỏ:
            // texture của WGC chỉ sống trong phạm vi callback (xem CaptureTypes.h).
            // Đây là thứ cứu trường hợp nguồn đứng yên — xem "cache frame cuối" ở
            // đầu file. Texture cache tạo lười, một lần, rồi CopyResource mỗi frame.
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
            p->lastFrameUs.store(NowUs(), std::memory_order_relaxed);

            // Chặn ở đây chứ không sớm hơn: mọi bước trên (cache frame, ghi nhận
            // kích thước) vẫn phải chạy TRƯỚC khi có client, vì giai đoạn 4 của
            // RunAgent đang đợi đúng srcW để dựng offer.
            if (!p->netReady.load(std::memory_order_acquire)) return;
            if (!ensureEncoder(encW, encH, fi.width, fi.height)) return;
            // Encode liên tục kể cả khi chưa có client (đơn giản, VBV ổn định);
            // NAL bị bỏ ở onPacket nếu chưa STREAMING. DiagEncode = Encode + đo
            // thời gian cho cửa sổ chẩn đoán (H1).
            p->DiagEncode(p->encoder.get(), fi.texture, p->forceIdr.exchange(false));
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
        // Client chuyển cửa sổ preview -> kéo đúng cửa sổ nguồn đó lên foreground.
        // Chia sẻ nhiều nguồn thì chỉ một cửa sổ được foreground, mà SendInput bơm
        // vào cửa sổ foreground; không có bước này thì client xem N nguồn nhưng chỉ
        // điều khiển được nguồn nào người ở máy host tự bấm vào.
        // Gác bằng enabled(): không cho điều khiển thì cũng không cho giành foreground.
        cb.onFocus = [p](bool focused) {
            if (!p->injector.enabled()) return;
            if (!focused) {
                p->injector.ReleaseAll(); // client rời đi giữa lúc giữ phím
                return;
            }
            if (!p->injector.FocusTarget())
                std::printf("[Agent][%s] Windows refused to bring this window to the front — "
                            "click it once on this machine.\n", p->name.c_str());
        };
        cb.onDisconnect = [p] {
            p->peerPacked.store(0, std::memory_order_release);
            p->injector.ReleaseAll(); // mất kết nối giữa lúc giữ phím = kẹt phím
            std::printf("[Agent][%s] Client left (BYE/timeout).\n", p->name.c_str());
        };
        // GD5 congestion control, RIÊNG từng nguồn: hai nguồn có thể đi cùng một
        // đường mạng nhưng bitrate của chúng độc lập nhau, và client có thể chỉ
        // đang xem một trong hai.
        // GD5 congestion control: policy nằm ở rgc::BitrateController (core, test
        // được offline). Ở đây chỉ còn phần dính thiết bị — đẩy quyết định xuống
        // encoder, cập nhật atomic cho thread FrameArrived, và in log.
        cb.onFeedback = [p](const rgc::Feedback& fb) {
            const rgc::BitrateDecision d = p->rate.Update(fb, NowUs());

            if (d.fecToggled) {
                p->wantFec.store(d.fecEnabled, std::memory_order_relaxed);
                if (d.fecEnabled)
                    std::printf("[Agent][%s] FEC on (loss %u%%).\n", p->name.c_str(), fb.lossPct);
                else
                    std::printf("[Agent][%s] FEC off (link clean).\n", p->name.c_str());
            }

            if (!d.changeBitrate) return;

            const uint32_t cur = p->rate.bitrateBps();
            std::lock_guard<std::mutex> lk(p->encMutex);
            // Encoder từ chối thì KHÔNG commit: lần Feedback sau tính lại từ mức cũ.
            if (p->encoder && p->encoder->SetBitrate(d.bitrateBps)) {
                p->rate.CommitBitrate(d.bitrateBps);
                p->curBitrateBps.store(d.bitrateBps, std::memory_order_relaxed);
                std::printf("[Agent][%s] Bitrate %.1f -> %.1f Mbps (loss %u%%, RTT %u ms)\n",
                            p->name.c_str(), cur / 1e6, d.bitrateBps / 1e6, fb.lossPct, fb.rttMs);
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
    //
    // Mỗi vòng làm bốn việc, theo thứ tự:
    //   1. Kiểm tra điều kiện dừng (Ctrl+C, hoặc MỌI nguồn đã đóng).
    //   2. recvfrom — chặn tối đa 100 ms, nên vòng lặp luôn quay đủ nhanh để Tick.
    //   3. Định tuyến gói vừa nhận về đúng SourcePipeline (xem sơ đồ đầu file).
    //   4. Tick mọi phiên + in thống kê mỗi giây.
    //
    // Một cửa sổ đóng KHÔNG giết cả phiên: chỉ dừng khi không còn nguồn nào sống.
    uint8_t buf[rgc::kMaxDatagram];
    uint64_t lastStatUs = NowUs();
    bool anyFailed = false;
    // H3: thời gian BẬN dài nhất của một vòng Recv trong cửa sổ 1s (không tính lúc
    // chờ recvfrom). Vòng này mà nghẽn thì buffer UDP của kernel gánh — tràn là mất
    // gói thật. Chỉ thread Recv chạm nên không cần atomic.
    uint32_t dgLoopBusyMaxMs = 0;

    for (;;) {
        if (g_ctrlC.load()) break;
        // Dừng khi MỌI nguồn đã đóng/hỏng — một cửa sổ đóng không nên giết cả phiên.
        bool anyAlive = false;
        for (SourcePipeline* p : live)
            if (!p->failed.load() && !p->capture.Closed()) anyAlive = true;
        if (!anyAlive) break;

        NetAddr from;
        const int n = sock.RecvFrom(buf, sizeof(buf), from);
        const uint64_t now = NowUs();
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

            // Sự kiện IDR do thread FrameArrived ghi lại (H1) — in ở đây để I/O
            // không nằm trên đường nóng. Luôn bật: IDR hiếm và cỡ của nó là con số
            // chẩn đoán quan trọng nhất phía host.
            if (const uint64_t ib = p->dgIdrBytes.exchange(0, std::memory_order_acquire)) {
                std::printf("[DIAG][%s] evt=idr bytes=%llu pkts=%u burst_ms=%u\n",
                            p->name.c_str(), (unsigned long long)ib,
                            p->dgIdrPkts.load(std::memory_order_relaxed),
                            p->dgIdrBurstMs.load(std::memory_order_relaxed));
            }

            // Nguồn vừa đổi kích thước (thread FrameArrived đã dựng lại encoder).
            // Báo client kích thước mới + IDR: stream đổi SPS giữa chừng, không có
            // IDR thì decoder client chỉ có rác cho tới keyframe kế tiếp.
            // `paused` đứng TRƯỚC exchange là cố ý: short-circuit giữ nguyên cờ
            // sizeChanged trong lúc tạm dừng. Client không phải dựng lại decoder cho
            // một cỡ suy biến (146x20) rồi lát nữa dựng lại lần nữa; và lúc nguồn to
            // trở lại, onFrame set cờ lần nữa nên RECONFIG vẫn được gửi đúng cỡ mới.
            if (!p->paused.load(std::memory_order_acquire) &&
                p->sizeChanged.exchange(false, std::memory_order_acq_rel)) {
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

            // Nguồn đang TĨNH (WGC chỉ phát frame khi nội dung đổi) — encoder không
            // có input mới. Hai nhu cầu gộp chung một chỗ:
            //   1. Yêu cầu IDR đang treo (>200ms không FrameArrived) → encode lại
            //      frame cache với IDR, không thì client join màn hình tĩnh đen mãi.
            //   2. KEEPALIVE ~2fps: MFT async (QSV) NGẬM output tới khi có input kế
            //      tiếp — nguồn đứng im là frame cuối kẹt trong encoder với timestamp
            //      cũ, client thấy nội dung trễ một nhịp và e2e ảo tới hàng chục giây
            //      (đo 2026-07-21). Bơm lại frame cache để đẩy nó ra; nội dung không
            //      đổi nên P-frame chỉ vài KB, chi phí không đáng kể.
            const uint64_t sinceFrameUs = now - p->lastFrameUs.load(std::memory_order_relaxed);
            const bool wantIdrFlush  = p->forceIdr.load() && sinceFrameUs > 200'000;
            const bool wantKeepalive = sinceFrameUs > 500'000 &&
                                       now - p->lastKeepaliveUs >= 500'000;
            if (p->session->state() == rgc::HostSession::State::Streaming &&
                p->haveCached.load(std::memory_order_acquire) &&
                (wantIdrFlush || wantKeepalive)) {
                std::lock_guard<std::mutex> lk(p->encMutex);
                if (p->ensureEncoderFn(p->srcW.load(), p->srcH.load(),
                                       p->srcTexW.load(), p->srcTexH.load())) {
                    p->DiagEncode(p->encoder.get(), p->cachedTex.Get(),
                                  p->forceIdr.exchange(false));
                    p->lastKeepaliveUs = now;
                }
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

                // Dòng chẩn đoán 1s của nguồn này (H1+H2): đọc-và-reset bộ đếm
                // cửa sổ.
                {
                    const uint32_t ec = p->dgEncCount.exchange(0, std::memory_order_relaxed);
                    const uint32_t es = p->dgEncMsSum.exchange(0, std::memory_order_relaxed);
                    const uint32_t em = p->dgEncMsMax.exchange(0, std::memory_order_relaxed);
                    std::printf("[DIAG][%s] evt=sum enc_ms_avg=%.1f enc_ms_max=%u idr=%u"
                                " burst_ms_max=%u send_fail=%u\n",
                                p->name.c_str(), ec ? double(es) / ec : 0.0, em,
                                p->dgIdrCount.exchange(0, std::memory_order_relaxed),
                                p->dgBurstMsMax.exchange(0, std::memory_order_relaxed),
                                p->dgSendFail.exchange(0, std::memory_order_relaxed));
                }
            }
            // Sức khỏe thread Recv (H3), chung cho mọi nguồn.
            std::printf("[DIAG][agent] evt=sum loop_busy_ms_max=%u\n", dgLoopBusyMaxMs);
            dgLoopBusyMaxMs = 0;
            lastStatUs = now;
        }

        // H3: vòng này bận bao lâu (từ lúc recvfrom trả về tới đây). Nghẽn nặng thì
        // báo ngay, không đợi cửa sổ 1s.
        const uint32_t busyMs = uint32_t((NowUs() - now) / 1000);
        if (busyMs > dgLoopBusyMaxMs) dgLoopBusyMaxMs = busyMs;
        if (busyMs > 250)
            std::printf("[DIAG][agent] evt=recv_stall busy_ms=%u\n", busyMs);
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
