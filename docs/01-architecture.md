# 01 — Kiến trúc tổng thể

## 1. Mô hình hệ thống

Hai thành phần, giao tiếp qua mạng IP:

- **Agent** (host): chạy trên máy đang chạy game. Bắt hình, nén, gửi video; nhận input và bơm vào game.
- **Client** (controller): chạy trên máy người dùng. Nhận video, hiển thị; bắt chuột/phím và gửi đi.

```
┌─────────────────────────── AGENT (máy chạy game) ───────────────────────────┐
│                                                                              │
│  Game window ──► WGC Capture ──► [Texture trong VRAM] ──► HW Encoder         │
│                  (đã có)                                    (NVENC/AMF/QSV)   │
│                                                                 │            │
│                                                          H.264/HEVC NAL      │
│                                                                 │            │
│  Input Injector ◄── Input decode ◄── Transport RX      Transport TX          │
│  (SendInput/                          (UDP)              (UDP, packetizer)    │
│   ViGEm)                                 ▲                     │              │
└──────────────────────────────────────────┼─────────────────────┼────────────┘
                                            │  input packets      │  video packets
                                            │                     ▼
┌─────────────────────────── CLIENT (máy điều khiển) ─────────────────────────┐
│                                            ▲                     │           │
│  Input Capture ──► Input encode ──► Transport TX      Transport RX           │
│  (Raw Input)                          (UDP)            (UDP, de-packetizer)   │
│                                                                 │            │
│                                                          H.264/HEVC NAL      │
│                                                                 │            │
│  Display  ◄────────────  Renderer  ◄──────────────────  HW Decoder           │
│  (swapchain)             (D3D11)                        (D3D11VA/DXVA2)       │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 2. Pipeline dữ liệu chi tiết (đường video)

| Bước | Vị trí | Dữ liệu vào | Dữ liệu ra | Ghi chú |
|------|--------|-------------|------------|---------|
| 1. Capture | Agent | Game window | `ID3D11Texture2D` (BGRA, VRAM) | Đã có. Dùng event `FrameArrived`, không polling. |
| 2. Encode | Agent | Texture VRAM | NAL units nén | Zero-copy. `nvEncRegisterResource` cho NVENC. |
| 3. Packetize | Agent | NAL units | Gói UDP ≤ MTU | Cắt NAL lớn thành nhiều gói, đánh số. |
| 4. Transmit | Agent→Client | Gói UDP | — | UDP, không chờ ACK cho video. |
| 5. Reassemble | Client | Gói UDP | NAL units | Ghép lại; bỏ frame thiếu gói (nếu không FEC). |
| 6. Decode | Client | NAL units | Texture VRAM | Hardware decode. |
| 7. Render | Client | Texture VRAM | Khung hình màn hình | Present qua swapchain. |

## 3. Pipeline dữ liệu (đường input — chiều ngược)

| Bước | Vị trí | Ghi chú |
|------|--------|---------|
| 1. Capture input | Client | Raw Input API bắt chuột/phím tương đối. |
| 2. Encode | Client | Message nhỏ, cố định; timestamp + sequence. |
| 3. Transmit | Client→Agent | UDP, kênh riêng; cần tin cậy nhẹ (retransmit key events). |
| 4. Inject | Agent | `SendInput` (mặc định) hoặc driver ảo (game khó). |

## 4. Mục tiêu hiệu năng

| Chỉ số | Mục tiêu LAN | Mục tiêu Internet |
|--------|--------------|-------------------|
| Độ trễ glass-to-glass | < 30–50 ms | < 80–120 ms |
| FPS | 60 (tùy chọn 120) | 30–60 |
| Bitrate 1080p60 | 15–30 Mbps | 8–20 Mbps |
| Frame drop | < 0.1% | < 1% (có FEC) |

**Ngân sách độ trễ (LAN, 60fps, mục tiêu ~40ms):**

```
Capture   ~2 ms  │ Encode   ~5–8 ms │ Network ~1–5 ms
Decode    ~5 ms  │ Render+present ~2ms + tối đa 1 frame chờ vsync (~16ms)
```

Kết luận: encode và vsync là hai khoản lớn nhất. Ưu tiên encoder low-latency preset
và cân nhắc tắt vsync ở client khi cần độ trễ tối thiểu.

## 5. Ràng buộc & giả định

- **Nền tảng Agent**: Windows 10 build 1903+ (yêu cầu của WGC). x64.
- **GPU Agent**: có hardware encoder. NVIDIA (NVENC), AMD (AMF), Intel (QSV/MF).
- **Client**: Windows trước (dùng lại D3D11). Agent và Client là hai **vai trò trong cùng
  một app** (`client.exe`, kiểu AnyDesk); phần dùng chung giữa các OS nằm ở `core/`,
  sau này thêm app cho macOS/Ubuntu/iOS/Android (`client/<os>/`).
- **Mạng**: UDP thông được giữa hai máy. NAT traversal để giai đoạn sau (xem protocol).
- **Bảo mật**: giai đoạn đầu chạy trong LAN tin cậy; mã hóa (DTLS) thêm sau.

## 6. Vì sao chọn các quyết định này

### Zero-copy VRAM → Encoder
Frame 1080p BGRA thô = ~8 MB. Kéo về CPU rồi đẩy lại GPU cho encoder tốn băng thông
PCIe và độ trễ. Encoder phần cứng nhận thẳng texture VRAM. Hàm `CopyToCpu` trong code
hiện tại chỉ để debug/lưu BMP, **không** nằm trong đường streaming.

### UDP thay vì TCP
TCP đảm bảo thứ tự và tin cậy bằng cách chặn (head-of-line blocking): một gói mất làm
nghẽn mọi gói sau. Với video realtime, một frame cũ đến muộn là vô dụng — thà bỏ và
chờ frame mới. UDP cho phép "bỏ qua và tiến tới".

### Tách kênh video / input
Video: dữ liệu lớn, mất gói lẻ chấp nhận được. Input: dữ liệu nhỏ, mất một sự kiện phím
là lỗi cảm nhận được (nhân vật chạy mãi vì mất event key-up). Hai đặc tính trái ngược →
hai kênh với chính sách tin cậy khác nhau.

## 7. Rủi ro kiến trúc (xếp theo mức độ)

1. **Input injection bị game bỏ qua** (cao): nhiều game dùng Raw/DirectInput hoặc có
   anti-cheat, không nhận `SendInput`. Phải thử sớm với game thật. Dự phòng: ViGEm (gamepad ảo),
   Interception driver.
2. **Encoder không zero-copy được** (trung bình): tùy GPU/driver. Dự phòng: copy VRAM→VRAM
   sang định dạng encoder chấp nhận, vẫn nhanh hơn qua CPU.
3. **Độ trễ mạng biến động (jitter)** (trung bình): cần jitter buffer nhỏ ở client, đánh đổi
   độ trễ lấy mượt.
4. **Mất gói làm hỏng frame kéo dài** (trung bình): decode lỗi lan tới frame sau vì
   inter-frame. Dự phòng: yêu cầu IDR frame khi phát hiện mất, hoặc FEC.
