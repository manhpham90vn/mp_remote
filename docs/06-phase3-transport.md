# 06 — Thiết kế Giai đoạn 3: Transport + Protocol v1

Thiết kế chi tiết cho GĐ3 (xem tiêu chí ở `05-roadmap.md`), với một ràng buộc mới được
nâng lên hàng đầu: **client tương lai chạy trên macOS / Ubuntu / iOS / Android / Windows**.
Quyết định nào ở GĐ3 cũng phải trả lời được câu hỏi *"phần này có dùng lại được trên
nền tảng khác không, hay ít nhất có cản trở việc đó không?"*

## 0. Mục tiêu & phạm vi

**Trong phạm vi GĐ3:**
- Thư viện chung giữa các OS `core/`: wire format, packetize/reassemble, session.
- UDP hai phía trên Windows (winsock); Agent `--serve`, Client `--connect` (cùng exe).
- Handshake HELLO/HELLO_ACK/START; PING/PONG đo RTT; STOP/BYE + timeout.
- Packetize NAL → VIDEO_PACKET; reassemble + chính sách bỏ frame ở client.
- **REQUEST_KEYFRAME khi mất gói** — kéo từ GĐ5 về (lý do ở §6).
- Thống kê: fps, bitrate, % mất gói, RTT, ước lượng trễ end-to-end.

**Ngoài phạm vi (giữ nguyên roadmap):** FEC, mã hóa, NAT traversal, audio, FEEDBACK →
điều chỉnh bitrate (GĐ5), RECONFIG khi resize (GĐ5), input (GĐ4), MF encoder xuất NAL
(chỉ cần khi có agent không-NVIDIA — hoãn tới lúc đó; GĐ3 agent yêu cầu backend NVENC).

**Tiêu chí xong:** Agent và Client trên hai máy LAN, client thấy hình game realtime,
log được trễ và % mất gói; mất gói (giả lập) tự phục hồi bằng IDR theo yêu cầu.

## 1. Nguyên tắc đa nền tảng (định hình toàn bộ GĐ3)

1. **Wire format là nguồn chân lý, không phải code.** `04-protocol.md` đặc tả đủ chi tiết
   để một client viết bằng Swift/Kotlin bám theo được mà không cần đọc code C++. Mọi thứ
   trên wire trung lập nền tảng: big-endian, µs, H.264 **Annex-B**, tọa độ chuẩn hóa,
   không có type Windows nào (HWND, QPC tick, v.v.) lọt lên wire.
2. **Tách lõi mạng thuần** vào **thư viện chung `core/`** (static lib riêng,
   namespace `deskhub`) — C++20 chuẩn, **cấm include Windows header** (`windows.h`, winsock,
   winrt, D3D…), chỉ dùng `<cstdint> <vector> <span> <optional> <functional> <string>`.
   Toàn repo build bằng **CMake + Ninja** (`CMakePresets.json`, preset `x64-debug`/
   `x64-release`); `core/CMakeLists.txt` build được cả standalone cho nền tảng
   khác — macOS/Linux (clang/gcc), iOS (Xcode, bọc ObjC++), Android (NDK, bọc JNI).
3. **Chỉ socket là platform-specific.** `UdpSocket` là lớp mỏng (~100 dòng): Windows dùng
   winsock2, sau này `#ifdef` sang BSD sockets (mac/Linux/iOS/Android dùng chung API BSD).
   Lõi `core` không sở hữu socket — nó nhận byte vào và phát byte ra qua callback, nên
   client platform nào muốn dùng API mạng riêng (NWConnection trên Apple) vẫn ghép được.
4. **Endianness tự viết** (shift byte, không dùng `htons/htonl`) để `ByteOrder.h` không kéo
   theo header hệ điều hành nào.
5. **Phiên định danh bằng `sessionId`, không phải addr:port.** Client di động đổi mạng
   (Wi-Fi → LTE) là đổi IP; Agent nhận gói hợp lệ mang đúng sessionId từ địa chỉ mới thì
   cập nhật peer — roaming "miễn phí". (Kéo theo sửa đổi header, §3.)
6. **Đàm phán codec đã sẵn trong HELLO** (`codecMask`: H264/HEVC/AV1) — client Apple sau
   này thích HEVC chỉ là chuyện bật bit, không đổi giao thức.

### Decode/render trên từng nền tảng client (tương lai — để kiểm tra thiết kế hôm nay)

| Nền tảng | Decode HW | Render | Ghi chú với Annex-B |
|----------|-----------|--------|---------------------|
| Windows | MF/D3D11VA (đã có GĐ2) | D3D11 | dùng lại nguyên `MfDecoder`+`Renderer` |
| macOS / iOS | VideoToolbox | Metal | VT cần AVCC: client tách SPS/PPS → `avcC`, đổi start-code → length-prefix. Việc của client, wire giữ Annex-B |
| Android | MediaCodec | Surface | nhận Annex-B trực tiếp (csd-0/csd-1 từ SPS/PPS) |
| Ubuntu | VAAPI (qua FFmpeg) | OpenGL/Vulkan | FFmpeg nhận Annex-B |

Kết luận: wire giữ **Annex-B** là đúng — 3/4 nền tảng ăn trực tiếp, Apple chuyển đổi nhẹ
phía client. Để chuyển đổi được, client cần SPS/PPS đi kèm mỗi IDR → NVENC bật
`repeatSPSPPS` (§4).

## 2. Cấu trúc module & luật phụ thuộc

```
core/                           ★ DÙNG CHUNG GIỮA CÁC OS — thuần C++20, namespace `deskhub`
├── include/deskhub/                public header (include qua "deskhub/<nhóm>/...")
│   ├── wire/ByteOrder.h ✅     đọc/ghi u16/u32/u64 big-endian (shift byte)
│   ├── wire/Wire.h ✅          hằng số Type/Chan/Flags, struct message,
│   │                           Build*/Parse* cho mọi loại gói v1
│   ├── transport/Packetizer.h ✅   NAL frame → N mảnh VIDEO_PACKET ≤ MTU
│   ├── transport/Reassembler.h ✅  mảnh → frame hoàn chỉnh; phát hiện mất; chính sách bỏ
│   ├── session/HostSession.h ✅    máy trạng thái phía Agent (IDLE→READY→STREAMING)
│   ├── session/ClientSession.h ✅  máy trạng thái phía Client (handshake, ping, timeout)
│   ├── input/InputSender.h ✅      gom + đánh seq + gửi lặp sự kiện chuột/phím
│   ├── input/InputReceiver.h ✅    khử trùng, đếm mất
│   ├── control/BitrateController.h ✅  policy bitrate/FEC phía host (GĐ5)
│   └── control/LinkStats.h ✅         gom thống kê 1s + dựng Feedback phía client
├── src/                        gương theo nhóm của include/deskhub/
├── tests/CoreTests.cpp ✅      test offline (không mạng/GPU) → target `core_tests`
└── CMakeLists.txt              static lib `core`; build được standalone
                                cho macOS/Ubuntu/iOS/Android

platform/                       ★ lớp MỎNG bọc header hệ điều hành — core không được chạm
└── include/deskhubp/Clock.h ✅     NowUs() đơn điệu (QPC trên Windows, CLOCK_MONOTONIC nơi khác)

client/
├── windows/                    app Windows — MỘT exe `client.exe` (kiểu AnyDesk), link `core`
│   ├── net/UdpSocket.h/.cpp ✅ platform: winsock2 (sau: #ifdef BSD sockets)
│   ├── net/NetInfo.h/.cpp ✅   liệt kê IPv4 theo card mạng (menu chính + agent in địa chỉ)
│   ├── capture/ encode/ ✅     WGC + D3D11; NVENC / Media Foundation
│   ├── decode/ input/ ui/ ✅   MfDecoder + Renderer; bơm/bắt input; dialog Win32
│   ├── AgentLoop.cpp ✅        ghép: capture+encode (GĐ2) + Packetizer + HostSession + socket
│   ├── ClientLoop.cpp ✅       ghép: socket + Reassembler + ClientSession + MfDecoder + Renderer
│   └── main.cpp ✅             vào thẳng GUI (MainMenuWindow); không còn CLI
└── android/                    app Android — UI Kotlin + lõi C++ (cpp/: net/, decode/)
(sau này: client/macos, client/linux, client/ios — mỗi OS một app)
```

**Một file duy nhất, kiểu AnyDesk (quyết định cố định):** người dùng chỉ tải **một exe**
chứa cả hai vai trò — chạy `--serve` là host (agent), `--connect ip:port` là client, không
tham số thì vào picker. **Không** tách thành hai app Agent/Client riêng. Trên nền tảng
khác sau này, mỗi OS cũng là một app duy nhất (tối thiểu vai trò client; macOS/Ubuntu có
thể thêm vai trò host khi làm capture cho OS đó).

Luật phụ thuộc (một chiều):

```
core:        ByteOrder ◄─ Wire ◄─ Packetizer / Reassembler / *Session   (thuần, test offline)
                                            ▲
client/<os>:     UdpSocket ◄─ AgentLoop / ClientLoop                    (platform, ghép nối)
```

`core` không gọi socket, không tạo thread, không đọc đồng hồ hệ thống — thời gian được
**bơm từ ngoài vào** (`nowUs` truyền vào `Tick()`/`Push()`), nên test đơn vị chạy được
không cần mạng, và client platform khác tự do chọn threading model của họ.

## 3. Sửa đổi protocol v1 (delta so với `04-protocol.md`, đã cập nhật vào đó)

1. **Header chung 4 → 8 byte**: thêm `sessionId (u32)` vào mọi gói.
   `HELLO`/`HELLO_ACK` dùng `sessionId=0` (chưa cấp); từ `START` trở đi dùng sessionId
   được cấp trong HELLO_ACK. Gói mang sessionId sai → bỏ. Đây là chốt cho roaming di động
   (§1.5) và chống gói lạc phiên cũ.
2. **Payload video tối đa 1176 byte** (= 1200 − 8 header chung − 16 header video).
3. **REQUEST_KEYFRAME thuộc phạm vi v1 bắt buộc** (không phải mở rộng) — vì GOP vô hạn.
4. Ghi chú hành vi: Agent gửi IDR (kèm SPS/PPS) ngay khi vào STREAMING.

## 4. Phía Agent

### Luồng dữ liệu & thread

```
Thread FrameArrived (WGC, đã có từ GĐ2):
    capture → NvencEncoder.Encode → onPacket(nal, idr, tsUs)
        → nếu session.state == STREAMING:
              Packetizer.Send(nal, frameId++, tsUs, idr,
                              [&](span pkt){ socket.SendTo(peer, pkt); })
        → nếu chưa STREAMING: bỏ NAL (không đệm)

Thread Recv (mới, blocking recvfrom + timeout 100ms):
    gói đến → HostSession.HandlePacket(data, len, fromAddr, nowUs)
    mỗi vòng → HostSession.Tick(nowUs)     // timeout phiên, v.v.
```

- `sendto` gọi thẳng trên thread FrameArrived: syscall rẻ (~µs), không cần queue riêng ở v1.
- `HostSession` callback `onStart` / `onKeyframeRequest` → gọi `encoder.ForceKeyframe()`.
  Gọi từ thread Recv trong khi Encode chạy ở thread khác → **`ForceKeyframe` phải là
  atomic flag**, áp vào lần Encode kế tiếp (kiểm tra/ sửa `NvencEncoder` nếu chưa).
- NVENC bật **`repeatSPSPPS=1`**: mọi IDR tự kèm SPS/PPS → client (mọi nền tảng) join/
  recovery giữa chừng đều decode được, không cần kênh riêng gửi extradata.
- Vào STREAMING (nhận START) → ForceKeyframe ngay để client có IDR mở màn.
- Agent chỉ phục vụ **một client** ở v1; HELLO mới khi đang STREAMING → từ chối
  (HELLO_ACK codec=0xFF).

### HostSession (core, thuần)

```cpp
struct HostCallbacks {
    std::function<void(std::span<const uint8_t>)> send;  // giao byte cho tầng socket
    std::function<void()> onStart;            // nhận START → force IDR, bắt đầu đẩy video
    std::function<void()> onKeyframeRequest;  // REQUEST_KEYFRAME
    std::function<void()> onDisconnect;       // BYE hoặc timeout ping
};
class HostSession {
public:
    HostSession(HostCallbacks cb, StreamParams offer); // width/height/fps/bitrate/codec
    void HandlePacket(std::span<const uint8_t> pkt, uint64_t nowUs);
    void Tick(uint64_t nowUs);                // phát hiện mất client (5s không gói)
    State state() const;                      // IDLE / READY / STREAMING
    uint32_t sessionId() const;
};
```

Đàm phán ở v1 tối giản: Agent áp kích thước cửa sổ game + fps/bitrate từ CLI; kiểm tra
client decode được H.264 (`codecMask bit0`), không thì từ chối.

### CLI

```
client.exe game.exe --serve [--port 47777] [--bitrate 20] [--fps 60]
```

**UX kiểu AnyDesk (thêm 2026-07-20):** chạy `client.exe` KHÔNG tham số → màn hình
chính: liệt kê địa chỉ IP máy này theo từng card mạng (adapter ảo vEthernet xếp
cuối), `[s]` chia sẻ ứng dụng (vào picker cửa sổ như cũ rồi `--serve`), `[c]` hoặc
gõ thẳng `ip[:port]` để kết nối. Xong phiên (host lẫn client) quay lại menu.
Agent khi bắt đầu nghe cũng in sẵn danh sách `ip:port` để đọc cho máy kia.

## 5. Phía Client (client Windows đầu tiên, tái dùng GĐ2)

### Luồng dữ liệu & thread

```
Thread Main: tạo cửa sổ preview + bơm message (như GĐ2).

Thread Recv (vòng chính của client):
    recvfrom (timeout 100ms)
    ├─ gói control → ClientSession.HandlePacket → (READY → tạo decoder/renderer theo
    │                 width/height trong HELLO_ACK → gửi START)
    ├─ gói video   → Reassembler.Push(pkt, nowUs)
    │                while (frame = Reassembler.PopReady(nowUs)):
    │                    tex = MfDecoder.Decode(frame.nal, frame.tsUs)
    │                    Renderer.Present(tex)
    │                if (Reassembler.TakeLossEvent()):
    │                    ClientSession.RequestKeyframe()      // có retry
    └─ mỗi vòng    → ClientSession.Tick(nowUs)   // retry HELLO/START, PING mỗi 1s,
                                                  // timeout 5s → onDisconnect
```

Decode+render nằm luôn trên thread Recv — đúng mô hình GĐ2 (cả chuỗi trên một thread,
~3.5 ms), tránh thêm queue/latency. Chỉ tách thread decode nếu đo thấy recvfrom bị đói
(để ngỏ, chưa làm).

### Reassembler (core, thuần) — chính sách v1

```cpp
class Reassembler {
public:
    struct Frame { uint32_t frameId; uint64_t timestampUs; bool idr;
                   std::vector<uint8_t> nal; };
    void Push(const VideoPacketView& pkt, uint64_t nowUs);
    std::optional<Frame> PopReady(uint64_t nowUs);  // trả frame theo thứ tự frameId
    bool TakeLossEvent();                           // true 1 lần khi vừa bỏ frame
};
```

- Giữ tối đa **4 frame đang ghép** (map theo `frameId`); gói trùng/cũ hơn frame đã phát → bỏ.
- `PopReady` chỉ trả **theo thứ tự frameId** (H.264 inter-frame cần thứ tự).
- Frame đầu hàng đợi chưa đủ mảnh mà (a) đã quá **2 khoảng frame** (~33ms@60fps) hoặc
  (b) đã có ≥2 frame mới hơn hoàn chỉnh → **bỏ frame đó**, đánh dấu loss.
- Sau loss: **nuốt mọi frame không-IDR** cho tới khi gặp IDR (decode tiếp chỉ sinh vỡ hình),
  đồng thời `TakeLossEvent()` → client xin keyframe. GOP vô hạn nên bước này bắt buộc.
- Trước khi có IDR đầu tiên của phiên: bỏ mọi frame non-IDR (join giữa chừng).

### ClientSession (core, thuần)

```cpp
struct ClientCallbacks {
    std::function<void(std::span<const uint8_t>)> send;
    std::function<void(const NegotiatedParams&)> onReady;   // codec/w/h/fps/timebase
    std::function<void(uint32_t rttUs)> onRtt;              // mỗi PONG
    std::function<void()> onDisconnect;
};
class ClientSession {
public:
    void Start(const ClientHello& hello, uint64_t nowUs);   // phát HELLO (retry 500ms)
    void HandlePacket(std::span<const uint8_t> pkt, uint64_t nowUs);
    void Tick(uint64_t nowUs);
    void RequestKeyframe();                                  // đặt cờ, Tick gửi + retry
};
```

### CLI

```
client.exe --connect 192.168.1.10[:47777] [--save]
```

## 6. Vì sao REQUEST_KEYFRAME phải nằm trong GĐ3

Encoder GĐ1 cấu hình **GOP vô hạn + IDR theo yêu cầu** (tiết kiệm bitrate). Hệ quả:
mất một gói của một frame ⇒ mọi frame sau tham chiếu frame hỏng ⇒ **vỡ hình vĩnh viễn**,
không tự lành. Trên LAN thật, mất gói lẻ tẻ chắc chắn xảy ra. Vậy vòng
`mất gói → bỏ frame → xin IDR → phục hồi` là **điều kiện sống** của GĐ3, không phải
tính năng ổn định hóa của GĐ5. (GĐ5 vẫn giữ phần còn lại: FEEDBACK→bitrate, RECONFIG, FEC.)

## 7. Đo đạc & nghiệm thu

Log mỗi 1s ở client (console): `fps nhận | kbps | frame bỏ | %gói mất | RTT | trễ e2e ước lượng`.
- % gói mất: đếm lỗ hổng `frameId`/`pktIndex` ở Reassembler.
- Trễ e2e ước lượng: `nowClient − (timestampUs + offset)`, với `offset ≈ (timebase chênh) − RTT/2`
  cập nhật theo PONG — đủ cho quan sát, không cần NTP.

### Milestone

| # | Nội dung | Kiểm chứng |
|---|----------|-----------|
| M1 | `core` + self-test `core_tests`: packetize → trộn thứ tự/bỏ gói giả lập → reassemble | bytes ra == vào; loss event đúng lúc; không cần mạng |
| M2 | Agent + Client 2 process **cùng máy** qua 127.0.0.1 | hình như `--loopback` GĐ2, qua UDP thật |
| M3 | **Hai máy LAN** | realtime, log đủ chỉ số — tiêu chí xong GĐ3 |
| M4 | Giả lập mạng xấu (tool clumsy: drop 2–5%) | vỡ hình ≤ vài trăm ms rồi tự phục hồi qua IDR |

## 7b. Mất gói theo CHÙM — đo được gì, và một giả thuyết đã bị bác bỏ

Đo ngày 2026-07-21, host Windows (Intel Quick Sync) → Pixel 4 qua Wi-Fi 5 GHz,
nguồn Screen 1 3440×1440 @60fps 20 Mbps.

**Mất gói ở đây gần như 100% là mất theo chùm.** `Reassembler::Stats::lossRuns` (thêm
để đo đúng việc này) cho phân bố dứt khoát: cả phiên chỉ có **một** chùm dài 1 gói,
còn lại nằm hết ở nhóm ≥32 gói, dài nhất **384 gói liên tiếp (~450 KB)**.

Hệ quả trực tiếp: **`fec+0` không phải bug.** `Packetizer` nhóm `kFecGroupSize` gói
LIÊN TIẾP và `TryRecover` bỏ cuộc ngay khi `missing != 1`, nên một chùm ≥2 gói đã đủ
vô hiệu hoá parity. Interleave cũng không cứu được: muốn gỡ chùm 384 gói thì cần 384
gói parity, tức 100% overhead. Đừng nới FEC để chữa loss ở đây.

### Giả thuyết đã thử và ĐÃ BỊ BÁC BỎ: pacing phía gửi

Suy luận ban đầu: `Packetizer::SendFrame` bắn cả frame vào `send` trong một vòng `for`
không nghỉ, nên một IDR 450 KB rời NIC 1 Gbps thành một chùm liền; xuống Wi-Fi
~300 Mbps thì hàng đợi chỗ thắt tràn và tail-drop nguyên dải. Đã cài `Pacer` (token
bucket + `CreateWaitableTimerExW` độ phân giải cao) rải gói ở 3× bitrate.

**Kết quả đo: không sửa được loss, mà làm mọi thứ khác xấu đi.**

| | trước pacing | sau pacing |
|---|---|---|
| chùm dài nhất | 384 gói | 352 gói — gần như không đổi |
| fps client | 11–38 | 3–37 (tụt còn 3–6 lúc xấu) |
| e2e | 141–397 ms | 249–785 ms |

Ở những giây tốt, pacing rải ở ~9 Mbps tức ngủ trước **từng gói một** (1 gói = 1,06 ms
> ngưỡng 500 µs) — mượt tối đa có thể — mà chùm 352 gói vẫn mất. Nếu burst phía gửi là
nguyên nhân thì mức rải đó phải xoá được nó.

Hai bài học giữ lại:

1. **Pacing không được bám theo bitrate đã bị congestion control siết.** Bitrate tụt →
   tốc độ rải tụt xuống dưới mức encoder đang thực sự sinh ra → hàng đợi gửi phình
   (đo được `q` tới 426 gói, pacing tự thêm 91 ms) → trễ tăng → loss → lại siết
   bitrate. Vòng xoáy tự tạo.
2. **Không được ngủ trong đường `onPacket`.** Encoder gọi `onPacket` ngay trong
   `Encode()` (`MfEncoder.cpp:443`, `NvencEncoder.cpp:237`), mà `Encode()` chạy dưới
   `encMutex` trong callback FrameArrived của WGC. Bản pacing đầu ngủ ở đó và kéo
   stream về ~0 kbps, e2e 13,8 giây. Phải xếp hàng rồi rải ở thread riêng.

`client/windows/net/Pacer.{h,cpp}` còn trong repo nhưng **không nối vào build** — giữ làm
nguyên liệu nếu quay lại, với chính sách chọn tốc độ khác. (CMakeLists có dòng comment
đúng chỗ nó sẽ nằm, để lần sau khỏi phải đi tìm.)

### Hướng chưa loại trừ

`lossRuns` đếm mảnh thiếu **trong một frame**, và chùm luôn nằm ở **đuôi** frame. Có
khả năng một phần con số "mất" thật ra là **tới muộn**: Reassembler hết hạn chờ frame
trước khi đuôi kịp tới rồi tính phần đuôi là mất. Cần tách bạch hai thứ này bằng đo
riêng trước khi thiết kế lại bất cứ thứ gì — chưa đo thì chưa biết loss thật bao nhiêu.

**Phép đo đã được cài** (2026-07-21): `Reassembler` giữ "nghĩa địa" các frame vừa khai
tử và đếm gói của chúng còn lết về sau đó — trường `late=` trong log `[DIAG]`, kèm
khám nghiệm từng frame bị bỏ (`evt=frame_drop pos=tail/...`) và cỡ từng IDR phía host
(`evt=idr bytes=`). Cách thu log và bảng tra triệu chứng: `09-diagnostics.md`.

## 8. Thứ tự triển khai

1. ✅ Lập thư viện `core/` (CMake target `core`, app link vào; toàn repo build
   CMake + Ninja, cấu trúc `core/` + `client/<os>/`) với `deskhub/ByteOrder.h` +
   `deskhub/Wire.h` + `Wire.cpp` (+ `04-protocol.md` đã cập nhật header 8 byte).
2. ✅ `Packetizer` + `Reassembler` (trong core) + target `core_tests` (M1 PASS 2026-07-20:
   in-order / trộn thứ tự / mất gói / trùng gói / join giữa chừng / timeout đầu hàng
   / mô phỏng handshake 2 session — bytes ra == vào, loss event đúng lúc).
3. ✅ `UdpSocket` (winsock; tắt `SIO_UDP_CONNRESET`, SO_RCVBUF 4MB) +
   `HostSession`/`ClientSession` (trong core).
4. ✅ `AgentLoop` (`--serve`): `repeatSPSPPS=1` đã sẵn từ GĐ1; `forceIdr` là atomic
   flag đặt từ thread Recv, tiêu thụ ở Encode kế tiếp. **Phát sinh ngoài thiết kế:**
   WGC chỉ phát frame khi nội dung ĐỔI → agent cache bản sao frame cuối
   (CopyResource, device bật multithread-protected); khi có yêu cầu IDR treo mà
   nguồn tĩnh >200ms, thread Recv encode lại frame cache — không thì client join
   màn hình tĩnh sẽ đen vĩnh viễn.
5. ✅ `ClientLoop` (`--connect`): ghép MfDecoder/Renderer GĐ2. M2 PASS 2026-07-20
   (2 process cùng máy, 127.0.0.1): handshake → hình hiển thị, nguồn tĩnh nhận IDR
   từ cache, nguồn động ~13fps end-to-end, 0% mất gói, e2e ~4–7 ms; client thoát →
   agent về IDLE chờ client mới.
6. ⬜ Chạy 2 máy LAN, đo, tinh chỉnh timeout/buffer (M3, M4) — cần máy thứ hai.
