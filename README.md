# 🖥️ Deskhub

> **Cả chiếc PC của bạn, trong tay bạn — ở bất cứ đâu.** Điều khiển bất kỳ ứng dụng nào trên
> máy từ xa — code với **Claude Code** hay **VS Code**, duyệt **Chrome**, chỉnh tài liệu, hay
> chơi **game nặng** — từ điện thoại, máy tính bảng, laptop khác, hay thẳng trong trình duyệt.
> Độ trễ **mili-giây**, hình **mã hóa/giải mã phần cứng** đầu-cuối, cài đặt gọn trong **một file**.

Remote desktop/app **độ trễ thấp, đa nền tảng**, kiến trúc kiểu **AnyDesk** — nhưng đủ nhanh và
đủ "thô" ở tầng input để **chơi được cả game** (chuột tương đối + scancode DirectInput), thứ mà
remote desktop thông thường không làm nổi. Điều khác biệt kỹ thuật: **một lõi C++20 duy nhất
chạy khắp mọi nơi** — từ Windows tới iPhone tới tab Chrome — không viết lại giao thức lần nào.

| ⚡ Độ trễ | 🖥️ Pipeline | 🌐 Nền tảng | 📦 Triển khai |
|-----------|-------------|-------------|---------------|
| **~3.5 ms** capture→hiển thị<br>(loopback, đo trên RTX 5070 Ti) | **Zero-copy VRAM**, HW encode+decode, 60 fps | **3 host + 6 client** từ một `core/` | **Một app / OS** — cắm là chạy |

> **Mới:** iOS client đã chạy — xem video từ host Windows trên iPhone/iPad qua LAN hoặc Tailscale.

## 💡 Dùng để làm gì

- **Làm việc từ xa** — mở Claude Code, VS Code, terminal trên PC ở nhà rồi code/chạy build
  từ laptop yếu hay iPad ngoài quán.
- **Duyệt & thao tác app** — điều khiển Chrome, Office, phần mềm chỉ có trên PC, từ mọi thiết bị.
- **Chơi game** — ca đòi hỏi nhất: 60 fps, độ trễ thấp, chuột tương đối + phím DirectInput.
- **Chia sẻ một cửa sổ, không phải cả màn hình** — chọn đúng ứng dụng muốn đưa đi; phần còn
  lại của máy vẫn riêng tư.

## 🚦 Trạng thái

**Windows là bản tham chiếu — pipeline hoàn chỉnh, đã kiểm chứng chạy thật trên 2 máy LAN
và qua Tailscale**, tức là dùng được **qua Internet/NAT**, không chỉ trong mạng nội bộ. Tải
bản cài sẵn ở [Releases](https://github.com/manhpham90vn/Deskhub/releases).

| Nền tảng | Host | Client | Tình trạng |
|----------|:----:|:------:|-----------|
| **Windows** | ✅ | ✅ | **Chạy thật 2 máy LAN + qua Tailscale** (Internet/NAT) — video + điều khiển |
| **Android** | — | 🔶 | Stream video chạy (emulator ~33 fps); chưa gửi input |
| **iOS** | — | 🔶 | Stream video chạy (SwiftUI + VideoToolbox); chưa gửi input |
| **Web** | — | 📐 | Thiết kế xong, chưa code |
| **macOS · Ubuntu** | ⬜ | ⬜ | Chưa bắt đầu |

Chi tiết giai đoạn + lộ trình từng nền tảng: [`docs/05-roadmap.md`](docs/05-roadmap.md).

## 🎯 Mục tiêu nền tảng

- **Agent (host): Windows · macOS · Ubuntu** — máy chạy game: bắt hình + nhận điều khiển.
- **Client: Windows · macOS · Ubuntu · iOS · Android · Web** — máy xem + điều khiển.

Kiểu **AnyDesk**: mỗi desktop OS là **một app duy nhất** chứa cả hai vai (agent + client);
iOS/Android/Web là app **client-only**. Thêm một nền tảng = chỉ viết lớp backend mỏng, lõi
giao thức dùng lại nguyên vẹn. Ma trận + lý do: [`docs/11-platform-transport.md`](docs/11-platform-transport.md).

## ✨ Bên dưới lớp vỏ

- **Zero-copy toàn tuyến** — Windows Graphics Capture đẩy frame thẳng vào VRAM → NVENC mã
  hóa ngay trên GPU → giải mã phần cứng → render. Đường nóng **không chạm CPU**.
- **Giao thức realtime tự thiết kế** — UDP, GOP vô hạn + **IDR theo yêu cầu** (không phát
  keyframe thừa), **FEC XOR** vá mất gói, **tự chỉnh bitrate** theo phản hồi mạng.
- **Transport lai** — native dùng UDP; web dùng **QUIC/WebTransport** (cùng mô hình datagram,
  cùng `core/` biên dịch **WASM**). Không WebRTC, không WebSocket.
- **Chọn GPU tự động** — NVIDIA (NVENC) → Intel/AMD → Media Foundation fallback.
- **Điều khiển từ xa thật** — chuột tương đối (Pointer Lock/Raw Input) + **scancode** cho
  game DirectInput, chống kẹt phím ba lớp.

## 🏗️ Cấu trúc

```
core/            lõi dùng chung MỌI nền tảng (protocol, C++20 thuần, không header OS)
                 wire/ → transport/ (FEC) → session/ + input/ + control/
platform/        lớp mỏng bọc header OS (Clock.h) — cái core không được chạm
client/windows/  app Windows — một exe, cả hai vai (host + client)   ✅ bản tham chiếu
client/android/  app Android — client-only (Kotlin + core C++)        🔶
client/ios/      app iOS — client-only (SwiftUI + core C++)          🔶
client/<macos|linux|web>/   các nền tảng còn lại                     ⬜ / 📐 thiết kế
docs/            tài liệu thiết kế (bắt đầu từ docs/README.md)
```

## Yêu cầu

- Windows 10 1903+ x64, Visual Studio 2022+ (workload C++, kèm sẵn CMake + Ninja).
- GPU NVIDIA khuyến nghị (NVENC); không có thì tự rơi về Media Foundation.

Cài toàn bộ dependency tự động (idempotent), mọi OS: `make bootstrap` — gồm cả Android
SDK/NDK và tool format (ktlint/swiftformat ghim version về `tools/`). Makefile chạy được
trên cả Windows/macOS/Ubuntu; trên macOS/Ubuntu hiện build được `core` + `make test`/
`make lint`/`make coverage` (client desktop chưa có).

## 🚀 Tải & chạy

**Cách nhanh nhất — tải bản cài sẵn** (Windows):

➡️ **[github.com/manhpham90vn/Deskhub/releases](https://github.com/manhpham90vn/Deskhub/releases)**

Tải bản mới nhất, chạy `client.exe` — mở thẳng màn hình chính, không cần cài đặt. Máy host
lần đầu cần mở firewall (lệnh ở dưới). Muốn dùng **qua Internet** thì bật
[Tailscale](https://tailscale.com) trên cả hai máy rồi kết nối bằng IP Tailscale (100.x.y.z).

**Hoặc build từ nguồn** (cần [Yêu cầu](#yêu-cầu) ở trên):

```
git clone https://github.com/manhpham90vn/Deskhub && cd Deskhub
make            # build debug → out\build\x64-debug\client\windows\client.exe
make run        # chạy, mở thẳng màn hình chính (GUI)
make build-android # build APK debug (Gradle + NDK, không cần máy/emulator)
make run-android # build + cài + mở app Android (máy/emulator trong `adb devices`)
make build-ios  # build app iOS cho Simulator (cần macOS + Xcode)
make run-ios    # build + cài + mở app iOS trên Simulator (cần macOS + Xcode)
make release-windows  # bản release theo nền tảng; tương tự: release-android (APK
                      # chưa ký), release-ios (Simulator)
```

Lệnh khác: `make release` · `make test` (core_tests — offline, không cần mạng/GPU). Cách
khác: mở repo bằng Visual Studio (Open Folder), hoặc
`cmake --preset x64-debug && cmake --build --preset x64-debug` trong Developer prompt.

Cửa sổ chính (`MainMenuWindow`) hiện IP máy này theo từng card mạng, ô chỉnh **Port/FPS/
Bitrate**, nút **"Chia sẻ ứng dụng trên máy này"** (chọn cửa sổ nguồn → làm host) và ô nhập
IP + **"Kết nối"** để làm client. Máy host lần đầu cần mở firewall:

```
netsh advfirewall firewall add rule name="Deskhub" dir=in action=allow protocol=udp localport=47777
```

## 🎮 Điều khiển từ xa

Input bật sẵn: gõ phím / di chuột trên cửa sổ preview của client là điều khiển máy host.

- `F9` khoá/thả chuột — **bắt buộc bật cho game FPS** (gửi chuột tương đối thay vì toạ độ).
- `F10` tạm dừng/tiếp tục gửi input.
- Ở máy host, **bấm vào cửa sổ đang chia sẻ một lần** để nó có focus: input chỉ được bơm
  khi cửa sổ đó đang foreground (cố ý — nếu không, người điều khiển từ xa sẽ gõ nhầm vào
  ứng dụng khác của bạn).
- Game/app chạy quyền admin → chạy host **as administrator**, không thì input bị UIPI chặn.

## 📚 Tài liệu

Thiết kế đầy đủ ở [`docs/`](docs/README.md) — kiến trúc đa nền tảng, giao thức, ma trận
nền tảng & transport, lộ trình. Bắt đầu đọc từ [`docs/README.md`](docs/README.md).
