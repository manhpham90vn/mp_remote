# 03 — Thiết kế Client (controller)

Client chạy trên máy điều khiển — vai **client**, trên **cả 6 nền tảng**: Windows · macOS ·
Ubuntu · iOS · Android · Web. Trách nhiệm: nhận video, giải nén, hiển thị; bắt input và gửi
đi. Đơn giản hơn Agent nhưng quyết định trực tiếp cảm nhận độ trễ và độ mượt.

Vai trò và điều phối (§1, §7, §8) **giống nhau mọi nền tảng**; chỉ ba backend — **decode ·
render · input** — và transport đổi theo nền tảng (§1b). Phần dưới mô tả **bản tham chiếu
Windows** (đã hiện thực GĐ2–GĐ5); Android ở `08-android-client.md`, iOS ở `12-ios-client.md`,
Web ở `10-web-client.md`, còn mac/Ubuntu chưa bắt đầu.

## 1. Các module

```
Client
├── TransportModule    core reassemble + per-platform transport  (UDP; QUIC ở web — 11 §2)
├── JitterBuffer       core/  (đệm nhỏ, sắp xếp gói, phát hiện mất gói)
├── DecoderModule      per-platform backend  (NAL → frame GPU)
├── Renderer           per-platform backend  (frame GPU → màn hình, present)
├── InputCapture       per-platform backend  (chuột/phím → message)
├── SessionManager     core/  (handshake, thương lượng, ping/RTT — chung mọi nền tảng)
└── UILayer            per-platform  (kết nối, chọn nguồn, thống kê độ trễ/fps/bitrate)
```

Ba module `per-platform backend` (Decoder/Renderer/InputCapture) + lớp transport là toàn bộ
việc phải viết cho một client mới; `JitterBuffer`/`Reassembler`/`SessionManager` nằm trong
`core/` dùng lại (biên dịch native, hoặc **WASM** cho web).

### 1b. Backend theo nền tảng

| | Windows (tham chiếu ✅) | macOS | Ubuntu | Android (🔶) | iOS (🔶) | Web (📐) |
|--|-------------------------|-------|--------|-------------|-----|---------|
| Decode | MF (D3D11VA) | VideoToolbox | VAAPI | MediaCodec | VideoToolbox | WebCodecs |
| Render | D3D11 | Metal | OpenGL/Vulkan | Surface (Compose) | Metal | canvas/WebGL |
| Input | Raw Input | — | — | touch | touch | Pointer Lock + KeyboardEvent |
| Transport | UDP (winsock) | UDP (BSD) | UDP (BSD) | UDP (BSD) | UDP (BSD) | **QUIC/WebTransport** |
| Lõi | `core/` native | ← | ← | ← native | ← | ← **WASM** |

WebCodecs gộp cả decode + render (§4/§5 không tách như Windows). Input trên mobile là chạm;
scancode bàn phím chỉ cần trên nền có bàn phím vật lý (desktop + web — `07-phase4-input.md` §5).

## 2. TransportModule (phía Client)

- **Nhận video**: gom gói UDP, dùng `frameId` + `packetIndex` để ghép lại một frame NAL.
- **Phát hiện thiếu gói**: nếu một frame thiếu gói sau timeout ngắn → bỏ frame đó và (nếu là
  frame quan trọng) gửi control **RequestKeyframe** để Agent phát IDR mới, cắt lỗi lan.
- **Gửi input**: đóng gói sự kiện từ InputCapture, gửi UDP kênh input.
- **Gửi control**: ping, báo cáo mất gói/RTT, xin đổi bitrate.

## 3. JitterBuffer

- Đệm rất nhỏ (1–2 frame hoặc theo ms cấu hình được). Đánh đổi: đệm lớn = mượt hơn nhưng trễ hơn.
- Sắp lại thứ tự gói đến sai thứ tự (UDP không đảm bảo thứ tự).
- Chính sách "thà bỏ hơn chờ": frame quá hạn hiển thị thì bỏ, không giữ pipeline.
- Cho phép tắt hoàn toàn ở chế độ "độ trễ tối thiểu" trong LAN.

## 4. DecoderModule

### Trừu tượng hóa

> **Trạng thái (GĐ2 ✅):** interface thực tế ở `client/windows/decode/IVideoDecoder.h`
> (`DecoderConfig`/`DecodedFrame`); backend `MfDecoder.cpp` — H.264 MFT đồng bộ +
> D3D11VA, output NV12 nằm trong VRAM, `MF_LOW_LATENCY`. Renderer dùng chung device.

### Backend
| API | Ghi chú |
|-----|---------|
| **D3D11VA / DXVA2** | Giải mã phần cứng, xuất thẳng D3D11 texture → render zero-copy. Ưu tiên. |
| **Media Foundation** (`IMFTransform` H.264/HEVC) | Bọc DXVA, dễ dùng, tích hợp `IMFDXGIDeviceManager`. |
| **NVDEC / cuvid** | Nếu client là NVIDIA và muốn tối ưu riêng. |
| **FFmpeg (sw fallback)** | Khi không có HW decode; chỉ dùng dự phòng, tốn CPU. |

Điểm mấu chốt: decoder và renderer **chia sẻ cùng D3D11 device** để texture giải mã ra
render thẳng, không copy.

## 5. Renderer

- Tạo swapchain (`IDXGISwapChain1`) trên cửa sổ client.
- Nhận texture từ decoder, vẽ full-screen quad (hoặc `CopyResource`/blit vào back buffer).
- **Vsync**: cho phép bật/tắt.
  - Bật (`Present(1,...)`): mượt, không tearing, thêm tới ~1 frame độ trễ.
  - Tắt (`Present(0,...)` + flip model + `ALLOW_TEARING`): độ trễ thấp nhất, có thể tearing.
- Overlay thống kê tùy chọn: fps, độ trễ, bitrate, % mất gói.

## 6. InputCapture — bắt input độ trễ thấp

- Dùng **Raw Input API** (`WM_INPUT`) để lấy chuyển động chuột **tương đối** thô (không bị
  tăng tốc/ghim bởi con trỏ hệ thống) — quan trọng cho game FPS.
- Bắt phím qua Raw Input hoặc `WM_KEYDOWN/UP`; lưu ý phân biệt key-down/up rõ ràng.
- **Con trỏ**: ở chế độ điều khiển, thường ẩn con trỏ hệ thống và khóa vào cửa sổ client
  (`ClipCursor`), gửi delta tương đối. Có chế độ "tuyệt đối" cho game giao diện chuột.
- Đóng gói mỗi sự kiện: loại, mã phím/nút, delta hoặc tọa độ chuẩn hóa, timestamp, sequence.

### Ánh xạ tọa độ (khớp với Agent §5)
Client biết kích thước frame nó đang render. Với input tuyệt đối, gửi tọa độ **chuẩn hóa 0..1**
theo vùng render; Agent quy đổi về pixel cửa sổ game. Với input tương đối, gửi delta thô +
độ nhạy, Agent áp vào vị trí chuột hiện tại trong game.

## 7. SessionManager (phía Client)

- Khởi tạo **handshake**: gửi khả năng client (codec hỗ trợ decode, độ phân giải màn hình, fps mong muốn).
- Đo **RTT** bằng ping định kỳ; báo cáo cho Agent để điều chỉnh bitrate.
- Xử lý teardown, reconnect.

## 8. Luồng điều phối Client

```
Luồng nhận mạng:
    gói video → jitterBuffer.Push(pkt)
    gói control → sessionManager.Handle(pkt)

Luồng render (theo nhịp hiển thị):
    nal = jitterBuffer.PopReadyFrame()
    if nal: tex = decoder.Decode(nal); renderer.Present(tex)
    else:   renderer.RepeatLastFrame()   # tránh nháy khi thiếu frame

Luồng input (message loop cửa sổ):
    khi WM_INPUT: evt = parse(); transport.SendInput(encode(evt))
```

## 9. UILayer (giai đoạn sau)
- Màn kết nối: nhập IP/port Agent, chọn game/cửa sổ.
- Bảng thống kê realtime để chỉnh bitrate/độ trễ.
- Phím tắt: bật/tắt khóa chuột, thoát điều khiển, chụp màn hình.

## 10. Đa nền tảng — mục tiêu, không phải "tương lai"

Client phải chạy trên **cả 6 nền tảng** (§1b). Cấu trúc repo đã tách sẵn cho việc này:
`core/` (protocol thuần C++20, dùng chung — biên dịch native hoặc WASM) + `client/<os>/`
(mỗi nền tảng một app; desktop kèm luôn vai agent, mobile/web client-only). Thêm một client
mới = viết **decode + render + input** theo API nền tảng đó + lớp transport, rồi link `core`
(mac/iOS/Linux dùng lại socket BSD của Android — `11-platform-transport.md` §5).

Trạng thái & thiết kế từng nền tảng:

- **Windows** ✅ — bản tham chiếu (doc này, §2–§9).
- **Android** 🔶 — `08-android-client.md` (Kotlin + core C++ qua JNI).
- **Web** 📐 — `10-web-client.md` (WebTransport + WebCodecs, core WASM).
- **macOS / Ubuntu / iOS** ⬜ — chưa bắt đầu; backend ở §1b, giao thức + core dùng lại.

Ma trận đầy đủ (ai làm client/agent, vì sao) + chiến lược transport: `11-platform-transport.md`.
