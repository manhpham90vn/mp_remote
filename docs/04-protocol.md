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
| 0x05 | LIST_SOURCES | control | C→A | có (retry 500ms) |
| 0x06 | SOURCE_LIST | control | A→C | không |
| 0x10 | VIDEO_PACKET | video | A→C | không |
| 0x11 | FEC_PACKET | video | A→C | không (parity) |
| 0x20 | INPUT_EVENT | input | C→A | tin cậy nhẹ (retry key) |
| 0x30 | PING | control | cả hai | không |
| 0x31 | PONG | control | cả hai | không |
| 0x32 | FEEDBACK | control | C→A | không (định kỳ) |
| 0x33 | REQUEST_KEYFRAME | control | C→A | có (retry) |
| 0x34 | RECONFIG | control | A→C | có |
| 0x35 | SET_FOCUS | control | C→A | phát lặp 3× |
| 0x36 | NACK | control | C→A | không (best-effort) |
| 0x37 | INVALIDATE_REF | control | C→A | không (best-effort) |

## 3c. Gửi lại theo NACK và huỷ khung tham chiếu (GĐ7)

Hai cơ chế phục hồi mất gói bù cho FEC XOR (chỉ cứu 1 mảnh/nhóm):

**NACK (0x36).** Khi frame đầu hàng ở `Reassembler` còn thiếu mảnh sau một nhịp chờ
(cho gói đảo thứ tự về), client gửi NACK liệt kê các `pktIndex` thiếu; host tra
`RetransmitCache` (kho các datagram video vừa phát) và gửi lại đúng các mảnh đó. Cứu
được **mọi** kiểu mất — kể cả chùm mà FEC chịu chết — nếu RTT đủ nhỏ để gói gửi lại về
trước hạn ghép (2 khoảng frame). Chỉ tốn băng thông khi thật sự mất gói, khác FEC luôn
tốn 1/8. Client tự điều tiết: không xin lại cùng frame trong vòng ~max(RTT, 10ms).
Định dạng: `frameId(u32) count(u8)` rồi `count × pktIndex(u16)`.

**INVALIDATE_REF (0x37).** Client báo đã bỏ hẳn một `frameId(u32)` để host thôi tham
chiếu nó (phục hồi bằng P-frame rẻ thay vì IDR nặng). Giao thức và định tuyến đã có
trong core; phần encoder (NVENC reference-invalidation / intra-refresh) còn chờ nối và
kiểm chứng trên phần cứng.

## 3b. Nhiều nguồn trên một host (GĐ6)

Một host chia sẻ được nhiều **nguồn** cùng lúc — mỗi nguồn là một cửa sổ hoặc cả một
màn hình — trên **một cổng UDP duy nhất** (người dùng chỉ mở một cổng firewall và chỉ
phải nhớ một địa chỉ).

**Mỗi cặp (client, nguồn) là một PHIÊN ĐỘC LẬP**, có sessionId riêng. Đây là quyết
định thiết kế chính: phương án kia là nhét `streamId` vào header video/input, nhưng
như vậy phải sửa toàn bộ đường nóng (packetize, reassemble, FEC, input) và
`HostSession`/`ClientSession` phải thành 1:N. Với phiên-mỗi-nguồn thì:

- Kênh video, FEC, input, FEEDBACK, RECONFIG **không đổi một byte nào**.
- `HostSession`/`ClientSession` vẫn là máy trạng thái 1:1 như GĐ3.
- Mỗi nguồn tự có encoder, nên tự điều chỉnh bitrate và tự xin IDR theo tình trạng
  của riêng nó — vốn là hành vi đúng, không phải mẹo.

Cái giá: client mở N socket và N luồng PING. Không đáng kể.

**Định tuyến ở host:** HELLO chưa có sessionId nên định tuyến theo `hello.sourceId`;
mọi gói khác đã mang sessionId nên khớp thẳng với phiên tương ứng.

### LIST_SOURCES (0x05) / SOURCE_LIST (0x06)

LIST_SOURCES rỗng payload, sessionId = 0 (hỏi trước khi có phiên). Host trả
SOURCE_LIST:

```
count(u8), rồi count lần:
  sourceId(u8) | width(u16) | height(u16) | nameLen(u8) | name (UTF-8, ≤64 byte)
```

Tên bị cắt ở `kMaxSourceNameBytes` nhưng **lùi tới ranh giới ký tự UTF-8** — cắt giữa
một ký tự nhiều byte sẽ hiện ô vuông ở danh sách phía client. Trần `kMaxSources` = 8
nguồn để chắc chắn vừa một datagram (và vì mỗi nguồn là một pipeline capture+encode,
nhiều hơn thì GPU không kham nổi).

Client phát lại LIST_SOURCES mỗi 500ms trong ~3s. Host không trả lời (sai IP, firewall,
hoặc bản cũ không biết message này) thì client cứ thử nguồn 0 — lỗi kết nối cụ thể từ
`ClientSession` hữu ích hơn nhiều so với một hộp thoại "không thấy host".

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
| sourceId | u8 | Nguồn muốn xem (lấy từ SOURCE_LIST). Thêm ở GĐ6; gói 13 byte kiểu cũ vẫn đọc được và hiểu là nguồn 0. |

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
| payload (mảnh NAL, ≤ 1174 byte)  ...                                |
```

> Trần payload là **1174** chứ không phải 1176 (= 1200 − 8 − 16): gói FEC dưới đây có
> header 16 byte **cộng** 2 byte `lenXor`, nên nó mới là ràng buộc chặt nhất. Lấy chung
> một trần cho cả hai để parity luôn phủ trọn được mảnh dữ liệu lớn nhất.

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

## 5b. Kênh FEC (FEC_PACKET, 0x11) — GĐ5

**Nhóm XEN KẼ (interleaved).** Frame `N` gói được chia thành `numGroups = ceil(N/8)`
nhóm (`kFecGroupSize = 8`); gói thứ `i` thuộc nhóm `i % numGroups` — **không** phải các
gói liên tiếp. Mỗi nhóm kèm MỘT gói parity = XOR của cả nhóm. Mất đúng 1 gói trong một
nhóm → client dựng lại được, không phải bỏ frame và xin IDR. Vì hai gói cùng nhóm cách
nhau `numGroups` vị trí, một **chùm mất tới `numGroups` gói liên tiếp** chỉ đụng mỗi nhóm
một gói nên vẫn cứu được trọn — đây là điểm hơn hẳn cách gom liên tiếp trước đây (chùm ≥2
là chịu), mà chi phí băng thông vẫn = 1/8. Mất ≥2 gói **cùng một nhóm** → parity vô dụng,
quay về chính sách §5.

```
+--------+--------+--------+--------+--------+--------+--------+--------+
| frameId (u32)                     | timestampUs (u64) ...           |
+--------+--------+--------+--------+--------+--------+--------+--------+
| ...timestampUs                    | pktCount(u16)  | grpIdx | rsv    |
+--------+--------+--------+--------+--------+--------+--------+--------+
| lenXor(u16)     | dữ liệu XOR (đệm 0 tới độ dài lớn nhất trong nhóm) |
```

| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| frameId / timestampUs / pktCount | | Như VIDEO_PACKET — đủ để dựng lại frame chỉ có 1 gói. |
| grpIdx | u8 | Nhóm xen kẽ: phủ các mảnh `{ i : i % numGroups == grpIdx }` với `numGroups = ceil(pktCount/8)` → `grpIdx, grpIdx+numGroups, grpIdx+2·numGroups, …`. Tối đa 256 nhóm (grpIdx là u8). |
| rsv | u8 | Dự trữ, phải bằng 0. |
| lenXor | u16 | XOR **độ dài** các mảnh trong nhóm — mảnh cuối frame ngắn hơn, không có trường này thì không biết cắt ở đâu. |

Cờ `IDR` ở header chung mang cùng giá trị với các gói dữ liệu của frame.

**Bật/tắt động.** FEC tốn 1/8 băng thông nên Agent chỉ bật khi FEEDBACK báo mất gói
≥1%, và tắt sau 5 giây liên tiếp sạch (tắt chậm hơn bật vì mất gói hay đến theo cụm).

## 6. Kênh input (INPUT_EVENT, 0x20)

Gói input nhỏ, có thể gộp nhiều sự kiện trong một datagram để giảm overhead.

```
+--------+--------+ ---- lặp cho từng event ----
| seq (u32)       | count(u8) | event[0] | event[1] | ...
+--------+--------+
```

| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| seq | u32 | Số thứ tự của **event đầu tiên** trong gói; event thứ `i` mang seq `= seq + i`. |
| count | u8 | Số event trong gói (≤ 62 để vừa một datagram). |

> **seq gắn với EVENT, không gắn với gói.** Bản nháp v1 để seq là số thứ tự gói,
> nhưng như vậy Agent không phân biệt được bản **gửi lặp** với thao tác mới (xem
> "tin cậy nhẹ" bên dưới) — nhấn W một lần sẽ thành ba lần. Đánh seq theo từng
> event thì cùng một trường lo được cả ba việc: khử trùng (`seq ≤ lastApplied` →
> bỏ), đếm mất (nhảy seq), và chống đảo thứ tự (gói cũ về muộn → toàn seq cũ → bỏ
> sạch, không tua ngược thao tác). Layout wire không đổi. Chi tiết: `07-phase4-input.md` §2.

**Cấu trúc một event:**
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| evType | u8 | `1=key`, `2=mouse_move`, `3=mouse_button`, `4=mouse_wheel`. |
| timestampUs | u64 | Thời điểm phát sinh ở client. |
| a | i32 | Tùy loại: key→vkCode; mouse_move→dx (hoặc x chuẩn hóa*65535); button→buttonId. |
| b | i32 | Tùy loại: key→**scancode** (bit8 = cờ E0, phím mở rộng); mouse_move→dy; wheel→delta; button→0. |
| state | u8 | `1=down/pressed`, `0=up/released`; mouse_move bỏ qua. |
| absolute | u8 | 1 nếu a/b là tọa độ tuyệt đối chuẩn hóa; 0 nếu delta tương đối. |

> `b` = scancode là **bắt buộc**, không phải tùy chọn: game dùng DirectInput/Raw Input
> đọc scancode chứ không đọc vkCode. Chỉ gửi vkCode thì gõ vào Notepad chạy tốt nhưng
> vào game không có gì xảy ra (`07-phase4-input.md` §5).

**Tin cậy nhẹ:** sự kiện **chuyển trạng thái** (key/button down/up) là quan trọng — mất
event key-up gây "kẹt phím". Chính sách v1 (đã hiện thực):
- Client **gửi lặp**: mỗi datagram kèm 8 event đã gửi gần nhất; khi hết event mới thì
  phát lại đuôi thêm 2 lần cách nhau 25 ms (gói cuối cùng — thường chính là event nhả
  phím — không có gói nào sau nó để bù). Agent khử trùng bằng seq nên lặp là vô hại.
- Agent **nhả hết phím đang giữ** khi BYE/timeout/mất focus. Đây là lưới an toàn cuối:
  gửi lặp chỉ giảm xác suất, không loại trừ được mất gói.
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

Agent dùng để **điều chỉnh bitrate encoder**. Luật hiện tại (GĐ5, `AgentLoop`): mất ≥5%
→ ×0.75; ≥2% → ×0.90; ≤1% và đã 2 giây không giảm → +5% trần mỗi giây. Kẹp trong
[1 Mbps, bitrate người dùng đặt]. Giảm nhân / tăng cộng là có chủ ý — mất gói UDP gần
như luôn là hàng đợi router đầy, nới nhanh ngay sau đó chỉ làm nghẽn lại theo chu kỳ.

Client gửi FEEDBACK **kể cả khi 0% mất gói**: im lặng bị Agent hiểu là mất kết nối chứ
không phải đường thông, và Agent cần tín hiệu sạch mới dám nới bitrate lên lại.

### REQUEST_KEYFRAME (0x33)
Rỗng payload (hoặc kèm `frameId` cuối nhận tốt). Agent gọi `encoder.RequestKeyframe()`.

### RECONFIG (0x34) — Agent thông báo đổi tham số
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| width | u16 | Độ phân giải mới (khi cửa sổ game resize). |
| height | u16 | |
| bitrateBps | u32 | Bitrate mới. |

Khi cửa sổ đang chia sẻ đổi kích thước, WGC tạo lại frame pool và Agent **bắt buộc phải
dựng lại encoder** — encoder gắn chặt với kích thước cũ. Agent gửi RECONFIG **kèm IDR**:
stream đổi SPS giữa chừng, không có IDR thì client chỉ có rác tới keyframe kế tiếp.

Phía client, RECONFIG chỉ để cập nhật hiển thị: `MfDecoder` tự đàm phán lại kích thước
khi gặp SPS mới (`MF_E_TRANSFORM_STREAM_CHANGE`) và `Renderer` tự dựng lại video
processor theo kích thước frame giải mã — không bên nào cần dựng lại từ đầu.

**Kích thước nén luôn là số chẵn.** NV12 lấy mẫu chroma 2×2; cửa sổ rộng/cao lẻ được cắt
xuống số chẵn gần nhất, không thì `CreateTexture2D(NV12)` trả `E_INVALIDARG`. Số trong
RECONFIG/HELLO_ACK là kích thước **đã cắt**, tức là cái thật sự nằm trong stream.

### SET_FOCUS (0x35) — client chuyển sang điều khiển nguồn này
| Trường | Kiểu | Ý nghĩa |
|--------|------|---------|
| focused | u8 | 1 = cửa sổ preview của nguồn này vừa nhận focus; 0 = vừa mất. |

Chia sẻ nhiều nguồn thì host chỉ để **một** cửa sổ ở foreground được, mà `SendInput` bơm
vào cửa sổ foreground chứ không vào một HWND cụ thể (`07-phase4-input.md` §5) — nên nếu
không có message này, client mở N cửa sổ preview nhưng chỉ điều khiển được đúng cái mà
người ngồi ở máy host tình cờ bấm vào. Client đổi cửa sổ preview → gửi SET_FOCUS(1) →
host gọi `InputInjector::FocusTarget()` (chính là `ForceForeground` mà `Init` đang dùng).
SET_FOCUS(0) → host nhả hết phím đang giữ của phiên đó.

Host **chỉ nghe SET_FOCUS khi đã bật cho phép điều khiển** (`--input`): không cho điều
khiển thì cũng không cho giành foreground của máy chủ.

Gửi **theo biến cố, không định kỳ**. Phát lại đều đặn thì người ngồi ở máy host không
bao giờ bấm sang được ứng dụng của chính mình — đổi lại phải chịu mất gói, nên mỗi lần
đổi phát 3 lần cách nhau 50 ms. Host xử lý idempotent (đã foreground thì không làm gì).
SET_FOCUS đi **trước** INPUT_EVENT trong cùng chu kỳ Tick, không thì mấy phím đầu tiên
sau khi đổi cửa sổ bị host bỏ vì cửa sổ chưa kịp lên trước.

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

## 11. Ràng buộc transport (transport bindings)

Giao thức trên đây **độc lập với transport**: nó chỉ giả định một kênh **datagram không
tin cậy, giữ ranh giới gói** (mất/đảo/trùng đều xử lý ở tầng này). Nhờ vậy cùng một
định dạng wire chạy trên nhiều transport mà không đổi một byte:

| Binding | Client | Datagram bằng | Ghi chú |
|---------|--------|---------------|---------|
| UDP native | Windows, Android | `sendto`/`recvfrom` (raw UDP) | Mặc định. `client/windows/net/UdpSocket`. |
| WebTransport | Web (trình duyệt) | QUIC DATAGRAM (RFC 9221) | Trình duyệt không mở được raw UDP. Xem `10-web-client.md`. |

**QUIC datagram ánh xạ 1-1 với UDP datagram** — đó là lý do client web chọn WebTransport
chứ không phải WebRTC/WebSocket: `Packetizer`/`Reassembler`/FEC/máy trạng thái phiên
dùng lại nguyên trạng. QUIC datagram cũng **không** retransmit và **có** chịu điều tiết
tắc nghẽn, đúng giả định của §5/§7 nên FEC + FEEDBACK vẫn cần.

Hai binding **cố ý cùng tồn tại** (hybrid), không thống nhất về một: native (Windows/
Android, và macOS/iOS/Linux sau) giữ UDP thô — nhẹ, không cert, đã kiểm chứng; chỉ web dùng
QUIC vì nó bắt buộc. Vì `core/` transport-agnostic (byte ra/vào qua callback, không biết
socket), thống nhất QUIC mọi client là việc **hoãn được**, gắn với lúc làm mã hóa (GĐ6).
Chiến lược đầy đủ + ma trận nền tảng (ai làm host/client): `11-platform-transport.md`.

**Trần payload thành tham số runtime.** §5 lấy trần **1174** cố định (MTU 1200 − header).
QUIC bọc thêm header gói + tag AEAD nên payload dùng được nhỏ hơn; trình duyệt báo trị
thật qua `datagrams.maxDatagramSize`. Do đó `kMaxVideoPayload` chuyển từ hằng biên dịch
sang **tham số đặt lúc handshake theo transport**: binding UDP truyền 1174 (hành vi cũ
không đổi), binding WebTransport truyền trần QUIC báo về. Packetizer nhận trần qua tham
số; reassembler không cần biết. Đây là **thay đổi core duy nhất** mà client web đòi hỏi.

**Định danh phiên.** Phiên định danh bằng `sessionId` chứ không phải addr:port (§2) —
giữ nguyên cho mọi binding. Với WebTransport, bản thân kết nối QUIC đã sống sót khi đổi IP
(connection migration), nhưng `sessionId` vẫn giữ để một host phục vụ chung cả client UDP
lẫn client web trên cùng một giao thức.
