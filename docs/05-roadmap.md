# 05 — Lộ trình triển khai

Thứ tự ưu tiên theo **rủi ro giảm dần**: làm phần dễ hỏng nhất trước để xác nhận dự án
khả thi sớm, tránh xây nhiều rồi mới phát hiện tắc.

## Giai đoạn 0 — Nền tảng ✅ XONG
- ✅ WGC capture cửa sổ game theo tên process.
- ✅ Refactor: tách thành module — `WindowCapture` (PIMPL, giấu winrt), `WindowFinder`, `BmpWriter` (debug).
- ✅ Chuyển polling → event `FrameArrived` (callback trên luồng thread-pool WGC).
- ✅ Chia sẻ D3D11 device qua `Device()`/`Context()` (COM thuần, không rò winrt) để encoder dùng chung.
- ✅ `CopyToCpu`/`WriteBmp` tách ra `BmpWriter`, chỉ chạy khi có cờ `--save`, ngoài đường nóng.
- **Tiêu chí xong**: ✅ build sạch (0 warning), capture chạy bằng event, đo fps; đường nóng không đụng CPU.

**Cấu trúc file sau GĐ0** (đường dẫn theo cấu trúc hiện tại `client/windows/`):
```
client/windows/
├── main.cpp                  main: tìm cửa sổ → capture theo event → đếm frame/đo fps
├── CaptureTypes.h            FrameInfo (D3D11/COM thuần, không winrt)
├── WindowCapture.h/.cpp      module capture, winrt giấu trong .cpp (PIMPL)
├── WindowFinder.h/.cpp       tìm HWND theo tên exe
└── BmpWriter.h/.cpp          công cụ debug: texture VRAM → BMP
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
- ✅ **Thư viện chung `core/`** (static lib, namespace `rgc`) — thuần C++20, **không
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
  MfDecoder/Renderer GĐ2) / `--nettest` (self-test M1). Client log mỗi 1 s:
  fps | kbps | frame bỏ | % gói mất | RTT | trễ e2e ước lượng.
- ✅ **UX kiểu AnyDesk**: chạy không tham số → menu chính hiện IP máy này theo từng
  card mạng (`NetInfo`, adapter ảo xếp cuối), `[s]` chia sẻ ứng dụng (picker cửa sổ
  như cũ), `[c]`/gõ thẳng `ip[:port]` để kết nối; xong phiên quay lại menu.
- ✅ **Phát sinh ngoài thiết kế**: WGC chỉ phát frame khi nội dung đổi → agent cache
  frame cuối (CopyResource) và encode lại từ thread Recv khi có yêu cầu IDR treo mà
  nguồn tĩnh >200 ms — không thì client join màn hình tĩnh sẽ đen vĩnh viễn.
- ✅ **Kiểm chứng** (2026-07-20): **M1** `--nettest` PASS (in-order/trộn/mất/trùng/join
  giữa chừng/timeout + mô phỏng handshake 2 session, bytes ra == vào). **M2** 2 process
  qua 127.0.0.1 PASS: handshake → hình hiển thị (cả nguồn tĩnh lẫn động ~13 fps),
  0% mất gói, RTT ~5–10 ms, trễ e2e ~4–7 ms; client thoát → agent về IDLE.
- ⬜ **Còn lại — tiêu chí xong đầy đủ**: **M3** hai máy LAN (nhớ mở firewall UDP 47777
  trên host), **M4** giả lập drop 2–5% (tool clumsy) tự phục hồi qua IDR ≤ vài trăm ms.

**File thêm ở GĐ3:** core: `Packetizer.h/.cpp`, `Reassembler.h/.cpp`,
`HostSession.h/.cpp`, `ClientSession.h/.cpp` (+ `ByteOrder.h`, `Wire.h/.cpp` từ trước);
client/windows: `UdpSocket.h/.cpp`, `NetInfo.h/.cpp`, `AgentLoop.h/.cpp`,
`ClientLoop.h/.cpp`, `NetTest.h/.cpp`, `TimeUs.h`.
Chạy: máy host `client.exe` → `[s]` (hoặc `client.exe game.exe --serve [--port N]`);
máy xem `client.exe` → gõ `ip[:port]` (hoặc `client.exe --connect ip[:port]`).

## Giai đoạn 4 — Input (RỦI RO CAO thứ hai)
- ⬜ InputCapture ở client (Raw Input) → INPUT_EVENT.
- ⬜ InputInjector ở Agent: `SendInput` trước; ánh xạ tọa độ chuẩn hóa → pixel cửa sổ.
- ⬜ Thử với **game thật**; nếu bị bỏ qua → thử ViGEm (gamepad) / Interception.
- **Tiêu chí xong**: điều khiển được ít nhất một game mục tiêu bằng chuột/phím từ client.

## Giai đoạn 5 — Ổn định & chất lượng
- ⬜ RECONFIG khi cửa sổ resize (REQUEST_KEYFRAME đã làm ở GĐ3).
- ⬜ FEEDBACK → điều chỉnh bitrate (congestion control đơn giản).
- ⬜ Slicing + (tùy chọn) FEC để mất gói không hỏng cả frame.
- ⬜ UILayer client: kết nối, thống kê, phím tắt khóa chuột.

## Giai đoạn 6 — Mở rộng (tùy nhu cầu)
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
