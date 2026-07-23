# 12 — Client iOS (SwiftUI + VideoToolbox)

Một trong **6 nền tảng client** (xem `03-client.md` §1b, `11-platform-transport.md`). iOS là
**client-only** — không làm agent được: inject input vào app khác và listen socket đều bị
sandbox chặn tuyệt đối (`11-platform-transport.md` §3). Đây là **doc thiết kế** (chưa code,
như `10-web-client.md`): chốt các quyết định trước khi bắt tay, để lúc port chỉ còn là chép.

iOS **làm client UDP bình thường** — rào cản host ở §3 không áp vào vai client
(`11-platform-transport.md` §5). Transport là **UDP POSIX**, dùng lại nguyên `UdpSocket` của
Android; không đụng gì tới QUIC (thứ chỉ web cần). Bản đầu **chỉ xem** — chưa gửi input, đúng
lộ trình của Android (§5, §7 dưới).

## 1. Phân chia Swift / C++

`core/` đã là C++20 không đụng header hệ điều hành (`core/CMakeLists.txt` ghi rõ nó phải
build được bằng mọi toolchain — MSVC, NDK, và giờ là Clang của Xcode). Nên toàn bộ
`ClientSession` / `Reassembler` / `Wire` / `LinkStats` dùng lại **y nguyên**. Phần phải viết
mới chỉ là bốn lớp mỏng platform-specific, đối ứng **1-1 với bản Android** — iOS gần Android
hơn gần Windows vì cả hai đều POSIX cho socket và đều "codec tự render, không qua CPU":

| Windows                   | Android                     | iOS                              | Vai trò                      |
|---------------------------|-----------------------------|----------------------------------|------------------------------|
| `UdpSocket.cpp` (winsock) | `UdpSocket.cpp` (BSD)       | **`UdpSocket.cpp` — chép từ Android** | datagram vào/ra         |
| `SourceQuery.cpp`         | `SourceQuery.cpp`           | **`SourceQuery.cpp` — chép từ Android** | LIST_SOURCES trước phiên |
| `MfDecoder` + `Renderer`  | `MediaCodecDecoder`         | **`VtDecoder`** (VideoToolbox)   | H.264 → màn hình             |
| `MainMenuWindow` (Win32)  | `MainActivity` (Compose)    | **`ConnectView`** (SwiftUI)      | nhập địa chỉ, chọn kết nối   |
| `SourcePickerDialog`      | `MainActivity` (một bước)   | **`SourcePickerView`** (SwiftUI) | chọn cửa sổ muốn xem         |
| cửa sổ preview            | `StreamActivity` (Compose)  | **`StreamView`** (SwiftUI)       | layer video + overlay số liệu |
| —                         | `JniBridge.cpp`             | **`DeskhubClient.mm`** (Obj-C++) | ranh giới UI ↔ C++           |
| —                         | `NativeClient.kt`           | **`DeskhubClient.swift`** (facade) | điểm gọi xuống C++ duy nhất |

`UdpSocket` và `SourceQuery` của Android là **POSIX thuần** (BSD socket, `arpa/inet.h`,
`sys/socket.h`) — chép sang iOS không sửa một dòng. Đây là lời hứa của
`11-platform-transport.md` §5 ("native mac/iOS/Linux dùng lại `UdpSocket` của Android") được
thu về tiền mặt: việc platform thật sự phải viết cho iOS là **decode + render + (sau) input**,
không phải socket.

Ranh giới Swift↔C++ cố ý mỏng, gói gọn trong `DeskhubClient.mm` + `DeskhubClient.swift`, đúng
như `JniBridge.cpp` + `NativeClient.kt` bên Android: **Swift chỉ làm phần người dùng nhìn thấy
và đưa layer xuống; không frame video nào đi qua Swift, và cũng không qua SwiftUI.** Video
sống trong một `AVSampleBufferDisplayLayer` bọc bằng `UIViewRepresentable`; VideoToolbox đẩy
`CMSampleBuffer` thẳng vào layer đó, nên đường nóng vẫn là bộ giải mã phần cứng → hardware
compositor. SwiftUI chỉ vẽ phần chrome (ô nhập địa chỉ, chữ trạng thái, overlay số liệu) và
cập nhật 500ms/lần.

Tỉ lệ khung hình do `.aspectRatio(w/h, contentMode: .fit)` lo — đối ứng
`Modifier.aspectRatio(w/h)` của Android, không phải tự tính frame.

## 1b. Seam Swift↔C++: vì sao Obj-C++ facade chứ không Swift/C++ interop trực tiếp

Swift 5.9+ gọi thẳng C++ được, nhưng **chọn một lớp Obj-C++ (`.mm`) làm mặt tiền** — đối ứng
đúng vai `JniBridge.cpp` bên Android — vì ba lý do:

1. **Vòng đời & thread rõ ràng.** `ClientLoop` chạy 2 thread nền và giữ con trỏ tài nguyên
   HĐH (layer). Một facade C giữ `std::unique_ptr<ClientLoop>` static (như biến toàn cục
   `g_client` bên Android) tránh được cả một lớp lỗi: Swift cầm con trỏ thô, giữ qua một lần
   xoay màn hình, rồi gọi vào đối tượng đã hủy. App chỉ xem **một nguồn** tại một thời điểm
   (view-only v1) nên một biến static là vừa đủ.
2. **`CMSampleBuffer`/`CVPixelBuffer`/`CAMetalLayer` là kiểu Core Foundation/Obj-C** — Obj-C++
   cầm chúng tự nhiên (bắc cầu ARC↔C++), Swift/C++ interop thuần thì vướng.
3. **Đối xứng với Android giúp port rẻ**: cùng một tập hàm mặt tiền (`start/stop/setLayer/
   phase/statusLine/listSources`), cùng một mô hình "một phiên toàn cục".

**Ánh xạ hàm mặt tiền** (khớp từng cái với `NativeClient.kt`):

| Android (`NativeClient`) | iOS (`DeskhubClient`) | Ghi chú |
|--------------------------|------------------------|---------|
| `nativeListSources`      | `listSources(addr:)`   | CHẶN ~3s → chạy trên `Task.detached`/`DispatchQueue` nền |
| `nativeStart`            | `start(addr:sourceId:)`| |
| `nativeStop`             | `stop()`               | |
| `nativeSetSurface`       | `setLayer(_:)`         | giao/thu hồi `AVSampleBufferDisplayLayer` |
| `nativePhase`            | `phase()`              | enum 4 giá trị Idle/Connecting/Streaming/Ended |
| `nativeStatusLine`       | `statusLine()`         | dòng fps/kbps/RTT/e2e cho overlay |
| `nativeEndReason`        | `endReason()`          | lý do phiên kết thúc |
| `nativeVideo{Width,Height}` | `videoSize()`       | đặt đúng `.aspectRatio` |

Khác JNI ở một điểm dễ chịu: **không có liên kết theo tên chuỗi**. Bẫy lớn nhất của
`JniBridge` (`Java_com_deskhub_app_...`, đổi tên gói mà quên sửa C++ → `UnsatisfiedLinkError`
lúc chạy, không lỗi biên dịch) **biến mất** — bridging header liên kết theo symbol lúc build,
sai tên là lỗi biên dịch ngay.

## 2. Decode + Render: VideoToolbox → `AVSampleBufferDisplayLayer`

Đây là mảnh iOS-specific lớn nhất. Đối ứng `MediaCodecDecoder` bên Android — **gộp cả decode
lẫn render**, không tách `Renderer` riêng như Windows.

### Vì sao `AVSampleBufferDisplayLayer` (ASBDL) chứ không `VTDecompressionSession` + Metal

Android chọn `MediaCodec` configure thẳng với `ANativeWindow`: `releaseOutputBuffer(..., true)`
đẩy frame từ bộ giải mã phần cứng lên hardware composer, **không qua CPU** — nên bên Windows
cần `Renderer` (NV12→BGRA) còn Android thì không (§1 của `08`). Analog **chính xác** trên iOS
là `AVSampleBufferDisplayLayer`: nạp `CMSampleBuffer` vào, layer tự **giải mã phần cứng +
hiển thị** qua compositor, ta không đụng vào pixel. Ít code hơn hẳn, đúng tinh thần "backend
mỏng".

Phương án kia — `VTDecompressionSession` → `CVPixelBuffer` → Metal (`CVMetalTextureCache`,
NV12 hai mặt phẳng, sample trong shader, present trên `CAMetalLayer`) — là bản sao của
`MfDecoder`+`Renderer` bên Windows: kiểm soát đầy đủ, tự compose overlay trong Metal. **Để
dành** cho lúc cần: (a) núm low-latency tường minh hơn ASBDL cho, hoặc (b) hiệu ứng/overlay vẽ
thẳng trong khung video. Bản đầu **không cần** — như Android không cần `Renderer`.

### Định dạng bitstream: Annex-B → AVCC

Stream của Deskhub là **H.264 Annex-B** (start code `00 00 00 01`), IDR mang sẵn SPS/PPS
in-band (NVENC bật `repeatSPSPPS` — xem `08` §3). VideoToolbox lại đòi **AVCC**:

- **Tham số:** `CMVideoFormatDescriptionCreateFromH264ParameterSets(...)` nhận SPS/PPS dạng
  NAL trần (không start code). Tách phần trước slice VCL đầu tiên bằng **`FirstVclOffset()`**
  — dùng lại nguyên logic của Android (`08` §3, chỗ xử lý máy Android đòi `CODEC_CONFIG`
  riêng). Chỉ dựng lại `formatDescription` khi SPS/PPS đổi (so byte).
- **Frame:** `CMSampleBuffer` cần NAL **length-prefixed 4 byte** (AVCC), không phải start
  code. Chuyển Annex-B→AVCC = quét start code, thay bằng độ dài big-endian. Rẻ, làm trên
  thread Decode.

Đặt attachment `kCMSampleAttachmentKey_DisplayImmediately = true` cho mỗi sample — chuỗi của
ta **không có B-frame** nên không cần layer giữ frame sắp xếp lại thứ tự hiển thị; đây là núm
low-latency đối ứng `MF_LOW_LATENCY` (Windows) và khóa `"low-latency"` của MediaCodec
(`08` §3). Không đặt timestamp lịch trình (để layer hiện ngay), hoặc dùng
`sampleBufferRenderer` với timebase chạy nhanh.

### Đo e2e

Số e2e đo trên frame **VỪA LÊN MÀN HÌNH**, không phải lúc nạp vào decoder — để so được với
Windows/Android (`08` §3, `09-diagnostics.md`). Với ASBDL, mốc "đã hiển thị" lấy từ
`sampleBufferRenderer`/callback trình bày; nếu ASBDL không lộ mốc đủ chính xác thì đây là một
lý do chính đáng để chuyển sang nhánh `VTDecompressionSession` (có callback
`decompressionOutputCallback` với thời điểm rõ ràng). Ghi chú lại: Android hiện **số đo e2e
còn sai** (`08` §5/§6) — đừng lặp lại, chốt cách đo ngay từ M1.

## 3. Thread

Giữ nguyên bố cục của `client/android/.../ClientLoop.cpp` (bản thân nó là port của
`client/windows/ClientLoop.cpp`):

- **UI** (main thread / `@MainActor`): giao/thu hồi layer, hỏi trạng thái 500ms/lần
  (`Timer`/`.task`) để cập nhật overlay.
- **Net:** `RecvFrom(10ms)` → `ClientSession` + `Reassembler` → đẩy frame vào hàng đợi.
- **Decode:** rút frame → `VtDecoder` → `CMSampleBuffer` → enqueue vào layer.

Tách Net và Decode vì lý do cũ, **không đổi trên iOS**: decode chặn thread Net thì `RecvFrom`
ngừng nghe, buffer UDP của HĐH tràn, sinh mất gói THẬT — loại mất mát mà cả FEC lẫn xin IDR
đều không cứu được. Hàng đợi giới hạn **3 frame** (`kMaxQueuedFrames`); đầy thì bỏ frame cũ
nhất và xin IDR. `ClientLoop` port gần như trọn vẹn — chỉ thay hai điểm chạm HW (decoder,
layer) và cơ chế bắt tay layer (§4).

## 4. Điểm khác biệt so với Android

**Layer đến rồi đi (lifecycle nền).** Như Android thu hồi Surface mỗi lần app xuống nền
(`surfaceDestroyed`), iOS cũng cần buông layer khi app vào nền / view biến mất — codec còn
enqueue vào một layer đã rời cây render là lãng phí hoặc lỗi. Nên `SetWindow(nullptr)` của
Android trở thành **`setLayer(nil)`**, và giữ nguyên cơ chế **bắt tay theo thế hệ**
(`winGen_`/`winAckGen_` trong `ClientLoop.h`): main tăng gen, Decode ack — số đếm thay cho cờ
bool để nhiều lần đổi liên tiếp không nuốt mất lần nào. Móc vào
`scenePhase == .background`/`onDisappear`. Quay lại foreground → dựng lại `VtDecoder` và xin
một IDR ngay (`ClientSession::RequestKeyframe`), y như Android.

> ASBDL còn một nếp riêng: sau khi app nền quay lại, layer có thể ở trạng thái
> `.failed`/cần flush (`requiresFlushToResumeDecoding`). Kiểm tra `status` trước khi enqueue;
> nếu cần thì `flush()` rồi bơm lại từ một IDR. Đây là chỗ iOS phát sinh so với Android, ghi
> vào code chỗ enqueue.

**SPS/PPS.** Android: đa số bộ giải mã nuốt được SPS/PPS in-band, vài dòng máy đòi
`CODEC_CONFIG` riêng nên tách bằng `FirstVclOffset()`. iOS **luôn** phải tách (VideoToolbox
đòi tham số dựng `CMVideoFormatDescription` tách khỏi frame) — nên `FirstVclOffset()` từ hàng
"tùy máy" bên Android thành **bắt buộc** bên iOS. Cùng một hàm, dùng lại.

**RECONFIG.** `MfDecoder` (Windows) tự đàm phán lại kích thước qua
`MF_E_TRANSFORM_STREAM_CHANGE`; MediaCodec (Android) đã configure cứng nên `onReconfig` đặt cờ
dựng lại codec. iOS giống Android: `formatDescription` gắn với SPS cũ, gặp SPS mới thì **dựng
lại** nó (và session nếu dùng nhánh VT). Host gửi RECONFIG **kèm IDR** (`04-protocol.md` §7)
nên không mất gì.

**Chọn nguồn.** `QuerySources()` (`SourceQuery.cpp`) chép từ Android không sửa: mở socket
riêng, phát `LIST_SOURCES` mỗi 500ms trong 3 giây, chờ `SOURCE_LIST`. Đứng NGOÀI `ClientLoop`
vì chạy trước khi có phiên — không sessionId, không thread. Nó chặn tới 3s nên
`DeskhubClient.listSources()` bọc bằng `Task.detached` (đối ứng `withContext(Dispatchers.IO)`
của Android). Facade trả về mảng chuỗi `"id\twidth\theight\tname"` cho Swift `split` — **không
dựng struct từ C++**, đúng như Android trả `Array<String>` (bên C chỉ cần bơm chuỗi, khỏi tra
kiểu Swift cho thứ dùng đúng một chỗ). `split(separator: "\t", maxSplits: 3)` — `maxSplits` là
**bắt buộc**, tiêu đề cửa sổ có thể chứa tab.

Danh sách rỗng gộp hai trường hợp — host im lặng (bản cũ / mất gói) và host không chia sẻ gì —
thành một: cứ vào **nguồn 0** và để `ClientSession` báo lỗi thật. Một nguồn thì bỏ qua luôn
`SourcePickerView`.

## 5. UI SwiftUI

Ba màn, đối ứng `MainActivity`/`SourcePickerView`/`StreamActivity` của Android:

- **`ConnectView`** — ô nhập `ip[:port]`, nút **Connect**. Địa chỉ nhớ lại bằng `@AppStorage`
  (đối ứng "địa chỉ được nhớ lại cho lần sau" của Android). Port mặc định **47777**
  (`kDefaultPort`, trùng `client/windows/MainMenuWindow.cpp`). Bấm Connect → `listSources` →
  nếu >1 nguồn đẩy sang `SourcePickerView`, nếu ≤1 vào thẳng `StreamView` nguồn 0.
- **`SourcePickerView`** — `List` các nguồn từ `SOURCE_LIST`; chọn → `StreamView`. Back về
  `ConnectView`.
- **`StreamView`** — `UIViewRepresentable` bọc `UIView` có
  `layerClass = AVSampleBufferDisplayLayer`, đặt `.aspectRatio(w/h, contentMode: .fit)` theo
  `videoSize()`. Overlay `Text` (fps/kbps/RTT/e2e) trên `ZStack`, cập nhật 500ms/lần từ
  `statusLine()`. `onAppear` → `setLayer` + `start`; `onDisappear`/nền → `setLayer(nil)`;
  phiên `Ended` → hiện `endReason()` rồi về `ConnectView`.

Quản trạng thái bằng một `@Observable`/`ObservableObject` (`SessionModel`) gọi vào
`DeskhubClient` — không View nào tự gọi C++, mọi lối đi qua facade (đối ứng "không Activity nào
tự khai external fun" của Android).

## 6. Build & chạy

**Không dùng CMake cho app** (khác Android dùng Gradle+CMake). App là **Xcode project** thêm
thẳng nguồn C++ vào target — `core/` không có header OS nên Clang của Xcode dựng sạch:

- Thêm compile sources: `core/src/**/*.cpp` + `client/ios/net/{UdpSocket,SourceQuery}.cpp` +
  `client/ios/*.{mm,cpp}` (ClientLoop, VtDecoder, DeskhubClient).
- **C++ Language Dialect = C++20** (`-std=c++20`); header search path thêm `core/include`.
- `DeskhubClient.h` vào **bridging header** để Swift thấy facade.
- Framework cần link: **VideoToolbox, CoreMedia, AVFoundation, CoreVideo** (+ Metal nếu chọn
  nhánh VT+Metal).
- Ký & chạy trên **thiết bị thật** để test LAN (Simulator không cùng mạng với host tiện lợi,
  và HW decode trên Simulator khác thiết bị). `Info.plist`: `NSLocalNetworkUsageDescription`
  (iOS đòi quyền LAN cho UDP nội mạng) và **Local Network** entitlement — thiếu là gói UDP ra
  vào mạng nội bộ bị chặn im lặng, một bẫy iOS không có ở Android.

Chạy: mở app, gõ IP host, **Connect**. Máy Windows chạy `client.exe`, chọn **[s]** chia sẻ một
cửa sổ. Cả hai cùng LAN, tường lửa Windows cho UDP 47777 vào (`netsh ... localport=47777`).
Qua Internet: bật Tailscale hai đầu, nhập IP `100.x.y.z`.

**Ràng buộc phiên bản** (điền khi build thật lần đầu, theo mẫu bảng của `08` §4): Xcode /
iOS Deployment Target / Swift version. Ghi lại mọi dòng từng làm build chết để máy khác khỏi
đi lại vết xe đổ — như Android đã làm.

## 6b. Quy ước ngôn ngữ

Theo yêu cầu dự án (`08` §4b): **mọi chuỗi người dùng hoặc console thấy đều bằng tiếng Anh**,
**mọi comment trong code bằng tiếng Việt có dấu**. Cụ thể: chuỗi UI trong `Localizable.strings`
(tiếng Anh, không hard-code trong View), log và dòng overlay dựng trong `ClientLoop.cpp` cũng
tiếng Anh.

## 7. Lộ trình (milestone)

Rủi ro giảm dần, đúng triết lý `05-roadmap.md` — làm phần dễ hỏng nhất trước:

- **M0 — Khung build & seam** (rủi ro cao nhất: toolchain + Swift↔C++). Xcode project, thêm
  `core/` + net glue làm sources, `DeskhubClient.mm` + bridging header. **Xong khi:** app rỗng
  gọi `listSources()` in ra console danh sách nguồn từ host Windows thật.
- **M1 — Stream video (view-only), mục tiêu chính.** Port `ClientLoop`; `VtDecoder`
  (VideoToolbox → ASBDL); 3 màn SwiftUI; lifecycle nền (§4); đo e2e đúng ngay từ đầu.
  **Xong khi:** xem được cửa sổ host trên iPhone/iPad thật qua LAN, đúng tỉ lệ, overlay chạy.
- **M2 — Input (GĐ4 cho iOS).** Chạm/kéo → `INPUT_EVENT` qua `core/InputSender`; bàn phím ảo
  (`UIKeyInput`) → vkCode + **scancode** (bắt buộc cho game DirectInput — `07-phase4-input.md`
  §5); chuột tương đối cho iPad (`GCMouse`/trackpad). iOS **đi trước Android** ở mảng này (Android
  chưa gửi input — `08` §5). **Xong khi:** điều khiển được host từ iPad.
- **M3 — Hoàn thiện.** FEC (đã ở core, chỉ nối dây), FEEDBACK/điều tiết bitrate (đã ở core),
  đa nguồn cùng lúc (nếu muốn), chẩn đoán `[DIAG]` (`09`).

## 8. Hạn chế đã biết (thiết kế, chưa làm)

- **Một nguồn tại một thời điểm** (view-only v1) — như Android: chọn cửa sổ nào để xem, chưa
  xem nhiều cửa sổ song song như client Windows.
- **Chưa gửi input** ở M0–M1 (để M2).
- **Cần thiết bị thật + quyền Local Network** — không test trọn trên Simulator được.
- **Chốt nhánh render** (ASBDL vs VTDecompressionSession+Metal) có thể phải xét lại nếu ASBDL
  không cho mốc "đã hiển thị" đủ chính xác để đo e2e (§2) — đó là tiêu chí quyết định, không
  phải sở thích.
