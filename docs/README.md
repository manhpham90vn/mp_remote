# Deskhub — Tài liệu thiết kế

Tài liệu thiết kế của **Deskhub** — điều khiển & stream **bất kỳ ứng dụng nào trên PC** từ xa
(code, duyệt web, game…), **đa nền tảng**. Giới thiệu dự án + hướng dẫn tải/chạy ở **README
gốc repo**; đây là phần thiết kế chi tiết.

## 🎯 Mục tiêu

- **Agent (host): Windows · macOS · Ubuntu** — máy chạy ứng dụng cần điều khiển: bắt hình + nhận điều khiển.
- **Client: Windows · macOS · Ubuntu · iOS · Android · Web** — máy xem + điều khiển.

Kiểu **AnyDesk**: mỗi **desktop OS là MỘT app duy nhất** chứa **cả hai vai trò** (agent +
client), chọn vai lúc chạy — không tách thành hai app. **iOS / Android / Web là app
client-only** (không thể làm agent — inject input + listen bị nền tảng chặn, xem
`11-platform-transport.md` §3). Phần logic giao thức dùng chung mọi nền tảng nằm ở `core/`
(thuần C++20); mỗi app chỉ viết lớp mỏng riêng của OS (capture/encode/inject + decode/
render/input + transport). **Thêm một nền tảng = chỉ viết backend, không đụng lõi.**

## Ma trận nền tảng mục tiêu

| Nền tảng | Vai Agent (host) | Vai Client | App | Trạng thái |
|----------|:---------------:|:----------:|-----|-----------|
| Windows | ✅ | ✅ | một exe, cả hai vai | **Chạy thật 2 máy LAN + Tailscale** (Internet/NAT) |
| macOS | ✅ | ✅ | một app, cả hai vai | ⬜ chưa bắt đầu |
| Ubuntu/Linux | ✅ | ✅ | một app, cả hai vai | ⬜ chưa bắt đầu |
| Android | ❌ (không host được) | ✅ | app client-only | 🔶 stream video chạy (emulator ~33fps); chưa gửi input, số đo e2e còn sai |
| iOS | ❌ | ✅ | app client-only | 🔶 stream video chạy (SwiftUI + VideoToolbox); chưa gửi input |
| Web | ❌ | ✅ | trong trình duyệt | 📐 thiết kế xong, chưa code |

Vì sao chỉ desktop làm agent (inject input + listen socket): `11-platform-transport.md` §3.
Transport: **native (mọi client trừ web) dùng UDP; web dùng QUIC/WebTransport** — hybrid,
`11-platform-transport.md` §5.

## Vai trò (chung mọi nền tảng)

- **Agent** = capture màn hình/cửa sổ → **encode phần cứng** → gửi video; nhận input →
  **inject** vào ứng dụng đích. Backend theo OS (Win: WGC/NVENC/SendInput · mac: ScreenCaptureKit/
  VideoToolbox/CGEvent · Ubuntu: PipeWire/VAAPI/uinput). Chi tiết: `02-agent.md`.
- **Client** = nhận video → **decode phần cứng** → render; bắt chuột/phím → gửi. Backend
  theo nền tảng (Win: MF+D3D11 · mac: VideoToolbox+Metal · Ubuntu: VAAPI · Android:
  MediaCodec · iOS: VideoToolbox · Web: WebCodecs). Chi tiết: `03-client.md`.

## Cấu trúc & build

```
core/            phần dùng chung MỌI nền tảng (protocol, thuần C++20, KHÔNG header OS)
  include/deskhub/ + src/, chia theo tầng:
    wire/        khung byte trên dây (header chung, build/parse từng message)
    transport/   cắt/ghép frame ↔ datagram, FEC, chống trùng & mất gói
    session/     handshake + vòng đời phiên hai phía (host/client)
    input/       chuỗi sự kiện chuột/phím có thứ tự
    control/     policy điều tiết: bitrate/FEC (host), gom thống kê (client)
  tests/         core_tests — chạy offline, mọi toolchain (MSVC/NDK/Emscripten…) dựng được
platform/        lớp mỏng bọc header OS (Clock.h…) — cái core không được chạm
host-transport/  binding transport phía host, DÙNG CHUNG mọi desktop OS (UDP + WebTransport;
                 msquic) — không thuần như core; xem 11 §2. (Thư mục chốt khi code.)
client/windows/  app Windows — một exe, CẢ HAI vai (agent + client)   ✅ bản tham chiếu
                 net/ capture/ encode/ decode/ input/ ui/
client/macos/    app macOS — một app, cả hai vai                       ⬜
client/linux/    app Ubuntu — một app, cả hai vai                      ⬜
client/android/  app Android — client-only (UI Kotlin + core C++)      🔶
client/ios/      app iOS — client-only (SwiftUI + core C++)             🔶
client/web/      client trình duyệt (WebTransport + WebCodecs; core→WASM)  📐 thiết kế
docs/            tài liệu
```

Build hiện tại (Windows, CMake + Ninja — hoặc mở folder bằng Visual Studio):

```
cmake --preset x64-debug && cmake --build --preset x64-debug
→ out/build/x64-debug/client/windows/client.exe
→ out/build/x64-debug/core/core_tests.exe   (hoặc: make test)
```

Android build bằng Gradle (`client/android/`, xem `08-android-client.md`). iOS build bằng
Xcode project (`client/ios/`, xem `12-ios-client.md`). macOS/Linux/Web: toolchain riêng khi
bắt đầu (mac: Xcode/CMake · Linux: CMake/GCC · Web: Emscripten cho `core.wasm` + bundler
cho trang).

## Mục lục

| Tài liệu | Nội dung |
|----------|----------|
| [01-architecture.md](01-architecture.md) | Kiến trúc đa nền tảng: vai trò, pipeline, backend theo OS, hiệu năng |
| [02-agent.md](02-agent.md) | Vai Agent (host): capture → encode → gửi + inject; backend Win/mac/Ubuntu |
| [03-client.md](03-client.md) | Vai Client: nhận → decode → render → input; backend cả 6 nền tảng |
| [04-protocol.md](04-protocol.md) | Giao thức mạng (độc lập transport & OS) |
| [05-roadmap.md](05-roadmap.md) | Lộ trình: các giai đoạn (bản Windows) + rollout theo nền tảng |
| [06-phase3-transport.md](06-phase3-transport.md) | Thiết kế chi tiết GĐ3 (transport) |
| [07-phase4-input.md](07-phase4-input.md) | Thiết kế chi tiết GĐ4 (input) |
| [08-android-client.md](08-android-client.md) | Client Android (NDK + Kotlin) |
| [09-diagnostics.md](09-diagnostics.md) | Log chẩn đoán điểm nghẽn (`[DIAG]`, luôn bật) |
| [10-web-client.md](10-web-client.md) | Client Web (WebTransport + WebCodecs, core WASM) — thiết kế |
| [11-platform-transport.md](11-platform-transport.md) | **Nền tảng & transport: ma trận agent/client, backend theo OS, chiến lược UDP/QUIC** (cross-cutting) |
| [12-ios-client.md](12-ios-client.md) | Client iOS (SwiftUI + VideoToolbox, core C++) — **đã triển khai** (stream video) |
| [13-release-mobile.md](13-release-mobile.md) | Phát hành mobile: fastlane + GitHub Actions, hướng dẫn cấu hình secrets/store một lần |

## Trạng thái

**Bản tham chiếu (Windows)** — pipeline hoàn chỉnh, **đã chạy thật trên 2 máy LAN và qua
Tailscale** (dùng được qua Internet/NAT): GĐ0 capture ✅ · GĐ1 encode (NVENC/MF) ✅ ·
GĐ2 loopback (~3.5 ms) ✅ · GĐ3 transport ✅ (M3 2 máy LAN ✅) · GĐ4 input ✅ (điều khiển
app thật trên 2 máy) · GĐ5 ổn định (RECONFIG/FEC/bitrate) ✅ · GĐ6 nhiều nguồn 🔶.
Còn lại: giả lập mất gói (GĐ5 M4), đo trễ input game & kiểm chứng nhiều nguồn 2 máy (GĐ6).

**Các nền tảng khác:** Android 🔶 (**stream video chạy trên emulator ~33fps**; chưa gửi
input, số đo e2e còn sai — 08 §5/§6) · iOS 🔶 (**stream video chạy** — SwiftUI +
VideoToolbox; chưa gửi input — 12) · Web 📐 (thiết kế xong, chưa code — 10) ·
macOS / Ubuntu ⬜ (chưa bắt đầu).
