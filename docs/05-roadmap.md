# 05 — Lộ trình triển khai

Lộ trình có **hai chiều**:

1. **Chiều sâu (GĐ0–GĐ6)** — dựng pipeline hoàn chỉnh trên **Windows làm bản tham chiếu**,
   thứ tự theo **rủi ro giảm dần**: làm phần dễ hỏng nhất trước (encode GĐ1, input GĐ4) để
   xác nhận dự án khả thi sớm, tránh xây nhiều rồi mới phát hiện tắc.
2. **Chiều rộng (rollout nền tảng)** — nhân bản tham chiếu ra **3 agent (Win/mac/Ubuntu) +
   6 client (Win/mac/Ubuntu/iOS/Android/Web)**. Đây là **mục tiêu quan trọng nhất** của dự
   án; `core/` chung khiến mỗi nền tảng mới chỉ tốn phần backend, không đụng giao thức.

## Rollout theo nền tảng

| Nền tảng | Agent | Client | Trạng thái | Doc |
|----------|:-----:|:------:|-----------|-----|
| Windows | ✅ | ✅ | **Chạy thật 2 máy LAN + Tailscale** (Internet/NAT); GĐ0–GĐ6 | 02 / 03 |
| Android | — | 🔶 | Stream video chạy (emulator ~33fps); chưa gửi input | 08 |
| Web | — | 📐 | Thiết kế xong, chưa code | 10 |
| macOS | ⬜ | ⬜ | Chưa bắt đầu — backend ở 02 §1b / 03 §1b | — |
| Ubuntu | ⬜ | ⬜ | Chưa bắt đầu | — |
| iOS | — | 📐 | Thiết kế xong, chưa code (SwiftUI + VideoToolbox) | 12 |

Ma trận + vì sao agent chỉ desktop: `11-platform-transport.md`. Các GĐ dưới đây là **chiều
sâu trên Windows**; khi mở một nền tảng mới, phần `core/` (GĐ3–GĐ6) dùng lại nguyên trạng,
chỉ viết backend capture/encode/inject (agent) hoặc decode/render/input (client).

## Thứ tự ưu tiên (chiều sâu)

Làm phần dễ hỏng nhất trước để xác nhận rủi ro sớm.

## Giai đoạn 0 — Nền tảng ✅ XONG
- ✅ WGC capture cửa sổ game theo tên process.
- ✅ Refactor: tách thành module — `WindowCapture` (PIMPL, giấu winrt), `WindowFinder`, `BmpWriter` (debug).
- ✅ Chuyển polling → event `FrameArrived` (callback trên luồng thread-pool WGC).
- ✅ Chia sẻ D3D11 device qua `Device()`/`Context()` (COM thuần, không rò winrt) để encoder dùng chung.
- ✅ `CopyToCpu`/`WriteBmp` tách ra `BmpWriter`, chỉ chạy khi có cờ `--save`, ngoài đường nóng.
- **Tiêu chí xong**: ✅ build sạch (0 warning), capture chạy bằng event, đo fps; đường nóng không đụng CPU.

**Cấu trúc file sau GĐ0** (nay đã gom vào `client/windows/capture/`):
```
client/windows/
├── main.cpp                  main: tìm cửa sổ → capture theo event → đếm frame/đo fps
└── capture/
    ├── CaptureTypes.h        FrameInfo (D3D11/COM thuần, không winrt)
    ├── WindowCapture.h/.cpp  module capture, winrt giấu trong .cpp (PIMPL)
    ├── WindowFinder.h/.cpp   tìm HWND theo tên exe
    └── BmpWriter.h/.cpp      công cụ debug: texture VRAM → BMP
```
Chạy: `client.exe [game.exe] [--save] [--frames N]`

## Giai đoạn 1 — Encode ✅ XONG (bản đầu, file-based)
- ✅ Định nghĩa interface `IVideoEncoder` + `EncoderConfig` (`IVideoEncoder.h`).
- ✅ Lớp chọn GPU theo chuỗi **NVIDIA → Intel → AMD → CPU (WARP)** (`GpuSelect.cpp`) — đúng
  yêu cầu không hard-code một loại GPU; capture và encoder **dùng chung 1 device** trên GPU đã chọn.
- ✅ Backend **Media Foundation** (`MfEncoder.cpp`): nhận thẳng texture D3D11 (VRAM) → H.264/MP4.
  MF tự dùng hardware encoder của device (NVENC trên NVIDIA / QSV trên Intel), tự rơi về software
  nếu không có HW → chuỗi ưu tiên được hiện thực "miễn phí" chỉ bằng việc chọn adapter.
- ✅ Kiểm chứng thật: bắt cửa sổ Notepad 2570×1018 trên RTX 5070 Ti → ghi 60 frame ra `output.mp4`
  (~1.1 MB, box `ftyp`/`mp42` hợp lệ).
- **Quyết định**: chọn MF thay vì NVENC trực tiếp vì `nvEncodeAPI.h` (Video Codec SDK) chưa có trên
  máy, trong khi MF có sẵn trong Windows SDK và đã cho hardware-encode trên chính NVENC qua MFT.
  Backend NVENC riêng (điều khiển low-latency mịn hơn) có thể thêm sau **sau cùng interface**.
- **Còn lại (đẩy sang GĐ3)**: xuất **NAL để streaming** thay vì file — cần `IMFByteStream` tùy biến
  hoặc chuyển sang NVENC/async-MFT. `forceKeyframe` hiện là no-op ở backend MF.

### Backend NVENC ✅ (đã thêm, là backend ưu tiên)
- ✅ `NvencEncoder.cpp`: nạp `nvEncodeAPI64.dll` động (không cần .lib), đăng ký texture D3D11
  zero-copy, preset `P4` + tuning `ULTRA_LOW_LATENCY`, CBR, GOP vô hạn + IDR theo yêu cầu,
  `forceKeyframe`=FORCEIDR chuẩn. Xuất **NAL Annex-B** ra `.h264` (sẵn sàng cho packetize GĐ3).
- ✅ `EncoderFactory.cpp`: thử **NVENC → Media Foundation** (khớp chuỗi NVIDIA→Intel→CPU).
- ✅ Kiểm chứng: RTX 5070 Ti → `output.h264` hợp lệ (start code Annex-B, SPS High profile,
  IDR 7784B + P-frame nhỏ dần). Có kiểm tra version: driver cũ hơn header → tự lùi về MF.
- ⚠️ **Ràng buộc phiên bản NVENC**: header phải ≤ API version driver hỗ trợ. Driver hiện tại
  hỗ trợ **API 13.0**; đang pin header **13.0** (`third_party/nvenc-13.0`, nhánh `sdk/13.0`
  của nv-codec-headers). Bản chính thức **13.1** ở `C:\Tools\Video_Codec_Interface_13.1.15`
  chỉ dùng được **sau khi update driver NVIDIA** — khi đó configure lại với
  `-DNVENC_INTERFACE_DIR=...` (biến cache trong `client/windows/CMakeLists.txt`).

**File thêm ở GĐ1:** `GpuSelect.h/.cpp`, `IVideoEncoder.h`, `MfEncoder.h/.cpp`,
`NvencEncoder.h/.cpp`, `EncoderFactory.cpp`.
Chạy: `client.exe game.exe --encode --out out.mp4 --bitrate 20 --fps 60 --frames 300`
(NVENC ghi ra `out.h264`; MF ghi ra `out.mp4`.)

## Giai đoạn 2 — Loopback nội máy (không mạng) ✅ XONG
- ✅ `IVideoDecoder` + `DecoderConfig`/`DecodedFrame` (`IVideoDecoder.h`) — đối xứng với
  encoder, GĐ3 chỉ đổi nguồn NAL từ loopback sang UDP, interface giữ nguyên.
- ✅ `MfDecoder.cpp`: H.264 decoder MFT **đồng bộ** (MFTEnumEx, SYNCMFT) + DXGI device
  manager → **D3D11VA hardware decode**, output **NV12 nằm trong VRAM** (IMFDXGIBuffer,
  texture pool + array slice, zero-copy). `MF_LOW_LATENCY` bật để MFT trả frame ngay.
  Xử lý `MF_E_TRANSFORM_STREAM_CHANGE` (renegotiate NV12) và `MF_E_NOTACCEPTING` (drain).
- ✅ `Renderer.cpp`: cửa sổ preview + swapchain **FLIP_DISCARD** + `Present(0)`;
  chuyển NV12→BGRA + scale bằng **D3D11 Video Processor** (không cần shader);
  input view cache theo (texture, slice); `--save` dump backbuffer ra `loopback.bmp`.
- ✅ Đường NAL trong process: thêm `PacketHandler onPacket` vào `EncoderConfig` —
  NVENC đẩy Annex-B ra callback (file `.h264` thành tùy chọn, `outputPath` rỗng = chỉ
  callback). MF encoder từ chối `onPacket` (SinkWriter chưa xuất NAL — việc của GĐ3).
- ✅ Chế độ `--loopback` trong main: capture → NVENC → MfDecoder → Renderer, cùng 1
  D3D11 device, timestamp QPC xuyên suốt để đo trễ end-to-end.
- ✅ **Kiểm chứng thật** (RTX 5070 Ti, Notepad 2570×1018, 60 frame): hình hiển thị lại
  đúng (so `window.bmp` vs `loopback.bmp` giống nhau, chữ rõ), trễ capture→hiển thị
  **~3.5 ms** ổn định (trung bình 8.2 ms tính cả frame đầu khởi tạo, max 53.9 ms).
- **Ghi chú luồng**: toàn chuỗi encode→decode→render chạy trên luồng FrameArrived của
  WGC; main chỉ tạo cửa sổ + bơm message. Device bật `SetMultithreadProtected`.

**File thêm ở GĐ2:** `IVideoDecoder.h`, `MfDecoder.h/.cpp`, `Renderer.h/.cpp`.
Chạy: `client.exe game.exe --loopback [--frames N] [--save]`
(không `--frames`: chạy tới khi đóng cửa sổ preview / nhấn ESC).

## Giai đoạn 3 — Transport + Protocol v1 ✅ XONG trên 1 máy (thiết kế: `06-phase3-transport.md`)
- ✅ **Thư viện chung `core/`** (static lib, namespace `deskhub`) — thuần C++20, **không
  Windows header**, dùng chung giữa các OS; không thread/socket/đồng hồ (thời gian
  bơm từ ngoài qua `nowUs` → test được offline). Cấu trúc repo: `core/` +
  `client/<os>/`; build **CMake + Ninja**. Đủ bộ: `ByteOrder` + `Wire` (header chung
  8 byte có sessionId) + `Packetizer` + `Reassembler` + `HostSession`/`ClientSession`.
- ✅ `UdpSocket` (winsock, mỏng, trong exe): tắt `SIO_UDP_CONNRESET`, SO_RCVBUF 4 MB,
  recvfrom timeout 100 ms. Chỉ lớp này là platform-specific.
- ✅ Packetize/reassemble frame: trả frame **theo thứ tự frameId**, giữ ≤4 frame đang
  ghép, bỏ frame khi quá 2 khoảng frame hoặc bị ≥2 frame mới hoàn chỉnh vượt mặt;
  sau loss nuốt non-IDR tới khi gặp IDR.
- ✅ Handshake HELLO/HELLO_ACK/START (retry 500 ms); PING mỗi 1 s đo RTT; BYE +
  timeout 5 s hai phía; host về IDLE chờ client mới sau khi client rời.
- ✅ **REQUEST_KEYFRAME khi mất gói** (retry 250 ms tới khi có IDR); `repeatSPSPPS=1`
  (có sẵn từ GĐ1); `forceIdr` là cờ atomic đặt từ thread Recv, tiêu thụ ở Encode kế tiếp.
- ✅ Mode `--serve` (AgentLoop) / `--connect ip[:port]` (ClientLoop, tái dùng
  MfDecoder/Renderer GĐ2) / `core_tests` (self-test M1). Client log mỗi 1 s:
  fps | kbps | frame bỏ | % gói mất | RTT | trễ e2e ước lượng.
- ✅ **UX kiểu AnyDesk**: chạy không tham số → menu chính hiện IP máy này theo từng
  card mạng (`NetInfo`, adapter ảo xếp cuối), `[s]` chia sẻ ứng dụng (picker cửa sổ
  như cũ), `[c]`/gõ thẳng `ip[:port]` để kết nối; xong phiên quay lại menu.
- ✅ **Phát sinh ngoài thiết kế**: WGC chỉ phát frame khi nội dung đổi → agent cache
  frame cuối (CopyResource) và encode lại từ thread Recv khi có yêu cầu IDR treo mà
  nguồn tĩnh >200 ms — không thì client join màn hình tĩnh sẽ đen vĩnh viễn.
- ✅ **Kiểm chứng** (2026-07-20): **M1** `core_tests` PASS (in-order/trộn/mất/trùng/join
  giữa chừng/timeout + mô phỏng handshake 2 session, bytes ra == vào). **M2** 2 process
  qua 127.0.0.1 PASS: handshake → hình hiển thị (cả nguồn tĩnh lẫn động ~13 fps),
  0% mất gói, RTT ~5–10 ms, trễ e2e ~4–7 ms; client thoát → agent về IDLE.
- ✅ **M3 hai máy LAN — ĐÃ CHẠY THẬT** (2026-07-22), và hơn thế: chạy tốt **qua Tailscale**
  (Internet/NAT), không chỉ LAN. (Host lần đầu nhớ mở firewall UDP 47777.)
- ⬜ **Còn lại**: **M4** giả lập drop 2–5% (tool clumsy) tự phục hồi qua IDR ≤ vài trăm ms.

**File thêm ở GĐ3:** core: `transport/Packetizer`, `transport/Reassembler`,
`session/HostSession`, `session/ClientSession` (+ `wire/ByteOrder.h`, `wire/Wire` từ trước),
`tests/CoreTests.cpp`; platform: `deskhubp/Clock.h`; client/windows: `net/UdpSocket`,
`net/NetInfo`, `AgentLoop`, `ClientLoop`.
Chạy: máy host `client.exe` → `[s]` (hoặc `client.exe game.exe --serve [--port N]`);
máy xem `client.exe` → gõ `ip[:port]` (hoặc `client.exe --connect ip[:port]`).

## Giai đoạn 4 — Input ✅ XONG phần code, CHỜ kiểm chứng 2 máy (thiết kế: `07-phase4-input.md`)
- ✅ **core**: `InputEvent` + build/parse trong `Wire`; `InputSender` (gom, đánh seq,
  gửi lặp) / `InputReceiver` (khử trùng, đếm mất) — thuần C++20, test offline được.
  `ClientSession::QueueInput` + `HostCallbacks::onInput` nối vào máy trạng thái sẵn có.
- ✅ **Tinh chỉnh giao thức**: `seq` gắn với **từng event** thay vì từng gói (layout wire
  không đổi). Không có nó thì bản gửi lặp bị hiểu thành thao tác mới — nhấn W một lần
  thành ba lần. Xem `04-protocol.md` §6 và `07-phase4-input.md` §2.
- ✅ **InputCapture** (client): Raw Input trên cửa sổ preview; bàn phím lấy **scancode**
  (`RAWKEYBOARD.MakeCode`, không phải WM_KEYDOWN) — game DirectInput đọc scancode, gửi
  vkCode thôi là game không nhận. Chuột 2 chế độ: tuyệt đối (mặc định) và tương đối
  (F9, khoá + ẩn con trỏ) cho game FPS. F10 tạm dừng gửi input.
- ✅ **InputInjector** (host): `SendInput` scancode + `KEYEVENTF_EXTENDEDKEY`; toạ độ
  chuẩn hoá → client rect cửa sổ đích → màn hình ảo (`MOUSEEVENTF_VIRTUALDESK`).
  Theo dõi phím/nút đang giữ → `ReleaseAll()` khi BYE/timeout/mất focus/thoát.
- ✅ **Chống kẹt phím** 3 lớp: redundancy trong gói + phát lại khi rảnh + `ReleaseAll`.
- ✅ **Phát sinh ngoài thiết kế — bẫy foreground**: `SendInput` bơm vào cửa sổ đang
  foreground của host, KHÔNG vào một HWND. Chủ máy bấm sang app khác là người điều khiển
  từ xa gõ thẳng vào trình duyệt/terminal của họ. Đã siết: chỉ bơm khi cửa sổ đang chia
  sẻ có focus, không thì bỏ qua + nhả phím. Vừa chống gõ nhầm vừa đúng ngữ nghĩa
  "chỉ chia sẻ cửa sổ này".
- ✅ **Kiểm chứng M1** `core_tests`: wire roundtrip (kể cả toạ độ âm), **bỏ 1/3 datagram
  mà mọi event vẫn áp dụng đúng một lần, đúng thứ tự**, gói đảo thứ tự không tua ngược.
- ✅ **Kiểm chứng M2** 2 process/1 máy: input đi trọn vòng client→host (`input 2 (mat 0)`),
  video không hồi quy (e2e ~2.3 ms, 0% mất gói).
- ✅ **M3 — ĐÃ CHẠY THẬT** (2026-07-22): 2 máy LAN + qua Tailscale điều khiển được **ứng
  dụng thường** (gõ phím, di chuột trọn vòng client→host).
- ⬜ **Còn lại**: **M4** điều khiển **game thật** (chuột nhìn + WASD), đo trễ input.
  Input **không test được trên 1 máy**: agent bơm vào foreground, nếu cửa sổ preview của
  client đang foreground thì phím vừa bơm bị chính client bắt lại → vòng lặp.
- ⬜ Nếu game bỏ qua input (anti-cheat lọc `LLMHF_INJECTED`) → ViGEm (tay cầm, tầng
  driver) hoặc Interception.
- ⚠️ Game/app chạy quyền admin ở host: phải chạy agent **as administrator** (UIPI).

**File thêm ở GĐ4:** core: `InputSender.h/.cpp`, `InputReceiver.h/.cpp` (+ `InputEvent`
trong `Wire`); client/windows: `InputCapture.h/.cpp`, `InputInjector.h/.cpp`.
Chạy: như GĐ3, input bật sẵn. `--noinput` = chỉ xem (đặt được ở cả hai vai trò).
`client.exe <app> --injecttest` = thử riêng đường bơm input, không cần mạng (dev).

## Giai đoạn 5 — Ổn định & chất lượng ✅ XONG phần code, CHỜ kiểm chứng 2 máy
- ✅ **RECONFIG khi cửa sổ resize**. Thread FrameArrived phát hiện đổi kích thước → vứt
  encoder + texture cache, dựng lại ngay ở frame đó; thread Recv gửi RECONFIG + IDR.
  Client không phải dựng lại gì: `MfDecoder` tự đàm phán lại qua
  `MF_E_TRANSFORM_STREAM_CHANGE`, `Renderer.EnsureVideoProcessor` tự theo kích thước
  frame giải mã. RECONFIG chỉ để cập nhật hiển thị + `HostSession::SetOffer` (client
  kết nối lại sau đó phải nhận số mới).
- ✅ **Kích thước nén luôn chẵn**: NV12 chroma 2×2, cửa sổ lẻ (1689×1392) làm
  `CreateTexture2D(NV12)` trả `E_INVALIDARG` → không có backend nào chạy được trên máy
  không-NVIDIA. `EncoderConfig` tách `width/height` (kích thước nén, chẵn) khỏi
  `srcWidth/srcHeight` (texture WGC thật, có thể lẻ) — video processor cần cả hai để
  khai báo content desc đúng, khai lệch thì `CreateVideoProcessorInputView` từ chối.
- ✅ **FEEDBACK → bitrate**. `IVideoEncoder::SetBitrate` (NVENC: `nvEncReconfigureEncoder`;
  MF: `CODECAPI_AVEncCommonMeanBitRate`) — không dựng lại encoder nên không cần IDR.
  Luật giảm-nhân/tăng-cộng, chi tiết ở `04-protocol.md` §7.
- ✅ **FEC parity XOR** theo nhóm 8 gói (`FEC_PACKET 0x11`): mất 1 gói/nhóm là dựng lại
  được, không phải bỏ frame + xin IDR (IDR to hơn P-frame nhiều lần — đáp mất gói bằng
  IDR đúng lúc đang nghẽn là đổ thêm dầu vào lửa). Bật/tắt động theo FEEDBACK để không
  trả 12.5% overhead khi đường sạch.
- ✅ **UILayer client** (đã làm cùng đợt GUI GĐ5): overlay số liệu trên cửa sổ preview,
  2 nút khóa chuột / tạm dừng đi cùng đường với F9/F10.
- ✅ **Kiểm chứng M1** `core_tests`: 6 ca FEC PASS (khôi phục gói giữa, khôi phục gói cuối
  ngắn, frame khôi phục giống hệt từng byte, 2 mất cùng nhóm thì rơi về chính sách cũ,
  frame 1 gói dựng lại từ parity, mặc định tắt). Toàn bộ suite GĐ3/GĐ4 không hồi quy dù
  `kMaxVideoPayload` đổi 1176→1174.
- ✅ **Chuỗi fix từ log chẩn đoán 2026-07-21** (host QSV → client Intel, LAN sạch —
  xem `09-diagnostics.md`): (1) **keepalive ~2fps khi nguồn tĩnh** — đo lần 2 xác
  nhận chạy (host `send 2 fps` đều khi capture 0); (2) **dựng decoder trên thread
  Decode** thay vì thread Recv — đo lần 2 xác nhận hết `recv_stall` phía client;
  (3) **PumpAsyncEvents cho MFT async** — QSV xếp sẵn nhiều NeedInput nên output bị
  giam sau hàng sự kiện, chỉ thoát ở lần Encode kế tiếp: input thưa (keepalive 2fps)
  đo được e2e ~3,4 s = ~7 frame × 500 ms; giờ vét sự kiện ngay sau ProcessInput
  (+`needInputCredit`), nhịp thưa chờ ≤30 ms cho frame vừa nén thoát ra.
- ⚠️ **VBV cho QSV chưa ăn**: `CODECAPI_AVEncCommonBufferSize` đặt rồi mà IDR vẫn
  195 KB (đo lần 2). Đã thêm log kết quả IsSupported/SetValue từng thuộc tính
  CodecAPI — lần chạy tới sẽ biết driver từ chối ở bước nào. Ghi chú thêm: trên QSV
  mỗi lần xin IDR = `ReinitTransform` (~200–265 ms, đo được `enc_ms_max=265` chặn
  thread Recv lúc START) — chi phí này là lý do nữa để ưu tiên NACK hơn xin IDR.
- ⬜ **Còn lại**: **M3** hai máy LAN — congestion control và FEC mới chỉ chạy đúng trên
  giấy + self-test, chưa lần nào gặp mất gói thật. **M4** giả lập drop 2–5% (clumsy) để
  chỉnh ngưỡng 2%/5% và nhóm FEC 8 cho khớp số đo thật.
- ⬜ Slicing (nhiều slice/frame) **chưa làm**: chỉ có ích nếu decoder chịu tiêu thụ frame
  thiếu mảnh, mà `MfDecoder` hiện đòi NAL trọn vẹn. Làm slicing mà không sửa đường decode
  thì không được gì — để lại tới khi có số đo M4 cho thấy FEC chưa đủ.

**File đổi ở GĐ5:** core: `wire/Wire` (FEC_PACKET, kMaxVideoPayload), `transport/Packetizer`
(sinh parity), `transport/Reassembler` (`PushFec`/`TryRecover`), `session/HostSession`
(`SetOffer`, `onFeedback`), `session/ClientSession` (`onReconfig`, `SendFeedback`),
`control/BitrateController` (policy siết/nới bitrate + trễ bật-tắt FEC),
`control/LinkStats` (gom thống kê 1s, dựng `Feedback` — dùng chung Windows/Android);
client/windows: `encode/IVideoEncoder.h` (`SetBitrate`, `srcWidth/srcHeight`),
`encode/MfEncoder`, `encode/NvencEncoder`, `AgentLoop`, `ClientLoop`.
Chạy self-test: `make test` (hoặc `out\build\x64-debug\core\core_tests.exe`).

## Giai đoạn 6 — Mở rộng (tùy nhu cầu)
- ✅ **Nhiều nguồn cùng lúc** (code xong, CHỜ kiểm chứng 2 máy). Host chia sẻ nhiều
  cửa sổ và/hoặc cả màn hình trên MỘT cổng; client hỏi `LIST_SOURCES`, tick chọn, mỗi
  nguồn mở một cửa sổ preview riêng.
  - **Mỗi cặp (client, nguồn) = một phiên độc lập** thay vì thêm streamId vào header
    video — xem `04-protocol.md` §3b về lý do. Kênh video/FEC/input/FEEDBACK không
    đổi một byte, `HostSession`/`ClientSession` vẫn 1:1.
  - Capture cả màn hình: `WindowCapture` nhận `CaptureTarget` (HWND **hoặc**
    HMONITOR) và gọi `CreateForMonitor`. `InputInjector` ánh xạ toạ độ theo rect
    monitor, và **bỏ chốt foreground** khi nguồn là cả màn hình — chốt đó có để input
    không rơi sang ứng dụng ngoài phạm vi chia sẻ, mà ở đây không có "ngoài phạm vi".
  - **Client dùng MỘT `InputCapture`**, gắn lại theo cửa sổ preview đang foreground.
    Raw Input đăng ký theo *process* chứ không theo cửa sổ: gọi `Attach` lần hai với
    HWND khác sẽ âm thầm hủy đăng ký của lần đầu.
  - `Renderer::Pump()` thành static: `PeekMessage` không lọc theo HWND nên một vòng
    bơm phục vụ hết mọi cửa sổ preview trên luồng đó.
  - ✅ **Kiểm chứng M1** `core_tests`: SOURCE_LIST round-trip (kể cả tên UTF-8), cắt
    tên đúng ranh giới UTF-8, HELLO mang sourceId và gói 13 byte kiểu cũ vẫn đọc được.
  - ⬜ **Còn lại**: M3 hai máy — chưa lần nào chạy thật với ≥2 nguồn.
- 📐 **Client Web** (WebTransport + WebCodecs) — **thiết kế xong, chưa code**
  (`10-web-client.md`). Chạy trong trình duyệt, chỉ xem + input (như Android v1). Trình
  duyệt không mở raw UDP → transport là **WebTransport (QUIC datagram)**, ánh xạ 1-1 với
  UDP nên `core/` biên dịch **WASM** dùng lại nguyên trạng; giải mã bằng **WebCodecs**.
  - **Thay đổi core duy nhất**: `kMaxVideoPayload` từ hằng biên dịch → tham số runtime
    theo `maxDatagramSize` (xem `04-protocol.md` §11).
  - **Mảnh native mới**: WebTransport server phía host (QUIC/HTTP3, đề xuất **msquic**),
    bơm datagram vào `HostSession` như UDP.
  - **Phần khó nhất**: chứng chỉ tự ký qua `serverCertificateHashes` (ECDSA P-256, hạn
    < 14 ngày, hash SHA-256) + phân phối hash cho người dùng — xem `10-web-client.md` §6.
  - **Transport hybrid** (native giữ UDP, chỉ web QUIC) + ma trận client/host + host
    desktop-only: `11-platform-transport.md`.
  - Mốc: M1 WASM+loopback trong tab → M2 WebTransport echo + chứng chỉ → M3 video e2e
    LAN → M4 input.
- ⬜ Mã hóa (DTLS/AEAD).
- ⬜ NAT traversal (ICE/STUN/TURN) để chạy qua Internet.
- ⬜ Audio (Opus + WASAPI loopback).
- ⬜ Adaptive resolution; multi-client.

## Bảng phụ thuộc

```
GĐ0 capture ─► GĐ1 encode ─► GĐ2 loopback ─► GĐ3 transport ─► GĐ4 input ─► GĐ5 ổn định ─► GĐ6 mở rộng
                                                   │
                                        (protocol v1 định nghĩa ở đây,
                                         nhưng struct nên viết sẵn từ GĐ1)
```

## Nguyên tắc xuyên suốt
1. **Xác nhận rủi ro sớm**: encode (GĐ1) và input (GĐ4) là hai chỗ dễ chết dự án — chạm sớm.
2. **Loopback trước mạng**: gỡ lỗi codec khi chưa có biến số mạng dễ hơn nhiều.
3. **Interface trước, backend sau**: `IVideoEncoder`/`IVideoDecoder` cho phép đổi GPU không sửa lõi.
4. **Đo, đừng đoán**: log fps, độ trễ, bitrate, mất gói ngay từ đầu để biết đang tối ưu đúng chỗ.
