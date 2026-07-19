# 03 — Thiết kế Client (controller)

Client chạy trên máy điều khiển. Trách nhiệm: nhận video, giải nén, hiển thị; bắt input và gửi đi.
Đơn giản hơn Agent nhưng quyết định trực tiếp cảm nhận độ trễ và độ mượt.

## 1. Các module

```
Client
├── TransportModule    (UDP send/recv, reassemble video, gửi input/control)
├── JitterBuffer       (đệm nhỏ, sắp xếp gói, phát hiện mất gói)
├── DecoderModule      (NAL → texture VRAM)
├── Renderer           (texture VRAM → swapchain, present)
├── InputCapture       (Raw Input → message chuột/phím)
├── SessionManager     (handshake, thương lượng, ping/RTT)
└── UILayer            (kết nối, chọn cửa sổ, thống kê độ trễ/fps/bitrate)
```

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

> **Trạng thái (GĐ2 ✅):** interface thực tế ở `client/windows/IVideoDecoder.h`
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

## 10. Đa nền tảng (tương lai)
Cấu trúc repo đã tách sẵn: `core/` (protocol thuần C++20, dùng chung giữa các OS) +
`client/<os>/` (mỗi OS một app duy nhất, tối thiểu vai trò client). Client mới chỉ cần
viết decode/render/input theo API nền tảng đó và link `core` — xem bảng nền tảng trong
`06-phase3-transport.md` §1.
