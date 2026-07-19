# 04 — Giao thức mạng

Đặc tả giao thức giữa Agent và Client. Thiết kế cho độ trễ thấp: UDP, tách kênh theo đặc
tính tin cậy. Phiên bản này là **v1** — cố ý đơn giản, đủ chạy LAN; các phần nâng cao
(mã hóa, FEC, NAT traversal) đánh dấu là mở rộng.

## 1. Tổng quan

- **Transport**: UDP. Một cổng, phân kênh bằng byte đầu; hoặc hai cổng (video/control) —
  v1 dùng **một cổng, phân kênh trong header** cho đơn giản.
- **Endianness**: mọi trường số nguyên **big-endian (network byte order)**.
- **MTU**: payload tối đa **1200 byte** (an toàn qua Internet, tránh phân mảnh IP).
- **Đơn vị thời gian**: micro giây (µs), kiểu `uint64`, gốc thời gian thỏa thuận lúc handshake.

## 2. Header chung (mọi gói)

Mỗi datagram UDP bắt đầu bằng header 8 byte:

```
 0        1        2        3        4        5        6        7
+--------+--------+--------+--------+--------+--------+--------+--------+
| Ver    | Type   | Flags  | Chan   | sessionId (u32)                   |
+--------+--------+--------+--------+--------+--------+--------+--------+
```

| Trường | Kích thước | Ý nghĩa |
|--------|-----------|---------|
| Ver | u8 | Phiên bản giao thức = `1`. |
| Type | u8 | Loại gói (bảng §3). |
| Flags | u8 | Cờ tùy loại (vd. bit 0 = frame IDR ở kênh video). |
| Chan | u8 | Kênh logic: `0=control`, `1=video`, `2=input`, `3=audio(dự phòng)`. |
| sessionId | u32 | ID phiên do Agent cấp trong HELLO_ACK. `HELLO`/`HELLO_ACK` dùng `0`. Gói mang sessionId sai → bỏ. |

Phiên được định danh bằng **sessionId, không phải addr:port**: gói hợp lệ mang đúng
sessionId đến từ địa chỉ mới → Agent cập nhật peer (client di động đổi mạng Wi-Fi↔LTE
vẫn giữ phiên).

Sau header 8 byte là payload tùy `Type`.

## 3. Bảng loại gói (Type)

| Type | Tên | Kênh | Hướng | Tin cậy |
|------|-----|------|-------|---------|
| 0x01 | HELLO | control | C→A | có (retry) |
| 0x02 | HELLO_ACK | control | A→C | có (retry) |
| 0x03 | START | control | C→A | có |
| 0x04 | STOP / BYE | control | cả hai | có |
| 0x10 | VIDEO_PACKET | video | A→C | không |
| 0x20 | INPUT_EVENT | input | C→A | tin cậy nhẹ (retry key) |
| 0x30 | PING | control | cả hai | không |
| 0x31 | PONG | control | cả hai | không |
| 0x32 | FEEDBACK | control | C→A | không (định kỳ) |
| 0x33 | REQUEST_KEYFRAME | control | C→A | có (retry) |
| 0x34 | RECONFIG | control | A→C | có |

## 4. Handshake (thiết lập phiên)

```
Client                                   Agent
  │ ── HELLO ─────────────────────────────► │  (khả năng client)
  │ ◄──────────────────────── HELLO_ACK ─── │  (khả năng agent + tham số chọn)
  │ ── START ─────────────────────────────► │  (xác nhận, bắt đầu stream)
  │ ◄═══════════ VIDEO_PACKET stream ══════ │
  │ ══════════════ INPUT_EVENT ═══════════► │
```

### HELLO (0x01) — payload
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| clientId | u32 | ID phiên do client sinh (chống nhầm gói cũ). |
| codecMask | u16 | Bitmask codec **decode** được: bit0=H264, bit1=HEVC, bit2=AV1. |
| maxWidth | u16 | Độ phân giải tối đa client render. |
| maxHeight | u16 | |
| desiredFps | u8 | FPS mong muốn. |
| features | u16 | Bitmask: bit0=FEC, bit1=encryption, bit2=relative-mouse... |

### HELLO_ACK (0x02) — payload
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| sessionId | u32 | ID phiên do Agent cấp; dùng trong mọi gói sau. |
| codec | u8 | Codec đã chọn (giao của khả năng hai bên). |
| width | u16 | Độ phân giải stream đã chọn (kích thước cửa sổ game). |
| height | u16 | |
| fps | u8 | FPS đã chọn. |
| bitrateBps | u32 | Bitrate khởi đầu. |
| timebaseUs | u64 | Gốc thời gian Agent (để quy đổi timestamp). |

Nếu không giao được codec → HELLO_ACK với `codec=0xFF` (từ chối).

## 5. Kênh video (VIDEO_PACKET, 0x10)

Một frame nén có thể lớn hơn MTU → cắt thành nhiều gói. Header video sau header chung:

```
+--------+--------+--------+--------+--------+--------+--------+--------+
| frameId (u32)                     | timestampUs (u64) ...           |
+--------+--------+--------+--------+--------+--------+--------+--------+
| ...timestampUs                    | pktIndex(u16)  | pktCount(u16)  |
+--------+--------+--------+--------+--------+--------+--------+--------+
| payload (mảnh NAL, ≤ 1176 byte)  ...                                |
```

| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| frameId | u32 | Số thứ tự frame, tăng dần. Client dùng để ghép và phát hiện mất. |
| timestampUs | u64 | Thời điểm capture (theo timebase). Dùng đồng bộ/đo độ trễ. |
| pktIndex | u16 | Thứ tự mảnh trong frame (0-based). |
| pktCount | u16 | Tổng số mảnh của frame này. |
| payload | bytes | Một phần dữ liệu NAL đã nén. |

**Flags (byte Flags của header chung) cho video:**
- bit0 `IDR`: frame này là keyframe (giải mã độc lập).
- bit1 `FRAME_END`: mảnh cuối của frame (dư thừa với pktIndex==pktCount-1, để chắc chắn).

**Ghép frame ở client:**
1. Gom các gói cùng `frameId` cho tới khi đủ `pktCount` mảnh.
2. Nếu đủ → nối theo `pktIndex` → NAL hoàn chỉnh → decode.
3. Nếu sau timeout (vd. 1–2 khoảng frame) vẫn thiếu → bỏ frame; nếu frame bị bỏ khiến
   decode lỗi lan → gửi **REQUEST_KEYFRAME**.

**Không có ACK cho video.** Độ tin cậy đến từ IDR-on-demand + (tùy chọn) FEC, không từ retransmit.

## 6. Kênh input (INPUT_EVENT, 0x20)

Gói input nhỏ, có thể gộp nhiều sự kiện trong một datagram để giảm overhead.

```
+--------+--------+ ---- lặp cho từng event ----
| seq (u32)       | count(u8) | event[0] | event[1] | ...
+--------+--------+
```

| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| seq | u32 | Số thứ tự gói input, tăng dần. Agent phát hiện mất/đảo. |
| count | u8 | Số event trong gói. |

**Cấu trúc một event:**
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| evType | u8 | `1=key`, `2=mouse_move`, `3=mouse_button`, `4=mouse_wheel`. |
| timestampUs | u64 | Thời điểm phát sinh ở client. |
| a | i32 | Tùy loại: key→vkCode; mouse_move→dx (hoặc x chuẩn hóa*65535); button→buttonId. |
| b | i32 | Tùy loại: mouse_move→dy; wheel→delta; key/button→0. |
| state | u8 | `1=down/pressed`, `0=up/released`; mouse_move bỏ qua. |
| absolute | u8 | 1 nếu a/b là tọa độ tuyệt đối chuẩn hóa; 0 nếu delta tương đối. |

**Tin cậy nhẹ:** sự kiện **chuyển trạng thái** (key/button down/up) là quan trọng — mất
event key-up gây "kẹt phím". Chính sách:
- Agent theo dõi `seq`; nếu phát hiện lỗ hổng chuỗi ở event trạng thái → có thể xin gửi lại,
  hoặc client **lặp lại các event trạng thái gần nhất** vài lần (đơn giản hơn, đủ tốt cho v1).
- `mouse_move` tương đối thì mất một gói không nghiêm trọng (chỉ hụt chút chuyển động) →
  không cần tin cậy.

## 7. Kênh control

### PING (0x30) / PONG (0x31)
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| pingId | u32 | Client sinh; Agent phản chiếu trong PONG. |
| sendTimeUs | u64 | Thời điểm gửi (client). RTT = nay − sendTimeUs khi nhận PONG. |

Ping định kỳ (vd. mỗi 1s) để đo RTT, phát hiện mất kết nối.

### FEEDBACK (0x32) — client báo tình trạng đường truyền
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| lostFrames | u16 | Số frame bỏ trong cửa sổ gần đây. |
| lossPct | u8 | % gói mất ước lượng. |
| rttMs | u16 | RTT hiện tại. |
| recvBitrateKbps | u32 | Bitrate thực nhận. |

Agent dùng để **điều chỉnh bitrate encoder** (congestion control đơn giản: mất nhiều → giảm bitrate).

### REQUEST_KEYFRAME (0x33)
Rỗng payload (hoặc kèm `frameId` cuối nhận tốt). Agent gọi `encoder.RequestKeyframe()`.

### RECONFIG (0x34) — Agent thông báo đổi tham số
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| width | u16 | Độ phân giải mới (khi cửa sổ game resize — code hiện đã bắt sự kiện này). |
| height | u16 | |
| bitrateBps | u32 | Bitrate mới. |

Khi cửa sổ game đổi kích thước, Agent (đã phát hiện trong `TryGetFrame`) gửi RECONFIG để
client tái tạo decoder/renderer đúng kích thước.

## 8. Máy trạng thái phiên

```
        HELLO/HELLO_ACK ok        START
IDLE ─────────────────────► READY ──────► STREAMING
  ▲                                          │
  │              STOP/BYE hoặc timeout        │
  └──────────────────────────────────────────┘
```

- **IDLE**: chờ HELLO.
- **READY**: đã thỏa thuận tham số, chờ START.
- **STREAMING**: video chảy A→C, input chảy C→A, control hai chiều.
- Mất PING quá N lần (vd. 5s không PONG) → quay về IDLE (mất kết nối).

## 9. Mở rộng (ngoài v1)

| Tính năng | Ghi chú |
|-----------|---------|
| **Mã hóa** | DTLS hoặc lớp AEAD (vd. libsodium) bọc payload. Bật qua `features` bit1. |
| **FEC** | Reed-Solomon/XOR trên nhóm gói video để phục hồi mất gói không cần retransmit. |
| **NAT traversal** | ICE/STUN/TURN để chạy qua Internet không cần forward port. |
| **Audio** | Kênh 3: Opus qua WASAPI loopback capture ở Agent. |
| **Adaptive resolution** | Giảm độ phân giải khi mạng yếu, không chỉ bitrate. |
| **Multi-client / relay** | Nhiều client xem, một client điều khiển. |

## 10. Vì sao thiết kế thế này

- **Một cổng, phân kênh trong header**: giảm rắc rối firewall/NAT so với nhiều cổng; đủ cho v1.
- **frameId + pktIndex/pktCount**: đủ tối thiểu để ghép frame và phát hiện mất mà không cần
  giao thức nặng như RTP. Có thể nâng lên RTP/RTCP sau nếu cần tương thích chuẩn.
- **IDR-on-demand thay vì GOP cố định**: tiết kiệm bitrate (không phát keyframe thừa), đổi lại
  cần kênh feedback — đã có sẵn ở control.
- **Input gộp nhiều event + tương đối cho chuột**: giảm overhead, hợp với game FPS cần chuột thô.
