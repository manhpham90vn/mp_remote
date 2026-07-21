# Remote Game Control — Tài liệu thiết kế

Phần mềm điều khiển game từ xa. Mỗi OS **một app duy nhất** (kiểu AnyDesk) chứa cả hai
vai trò: `client.exe --serve` = host (máy chạy game), `client.exe --connect ip:port` = client.

## Cấu trúc & build

```
core/            phần dùng chung giữa các OS (protocol, thuần C++20, KHÔNG header OS)
  include/rgc/ + src/, chia theo tầng:
    wire/        khung byte trên dây (header chung, build/parse từng message)
    transport/   cắt/ghép frame ↔ datagram, FEC, chống trùng & mất gói
    session/     handshake + vòng đời phiên hai phía (host/client)
    input/       chuỗi sự kiện chuột/phím có thứ tự
    control/     policy điều tiết: bitrate/FEC (host), gom thống kê (client)
  tests/         core_tests — chạy offline, mọi toolchain dựng được core
platform/        lớp mỏng bọc header hệ điều hành (Clock.h) — cái core không được chạm
client/windows/  app Windows (một exe: host + client)
                 net/ capture/ encode/ decode/ input/ ui/; gốc giữ main + hai vai trò
client/android/  app Android (UI Kotlin + lõi C++ dùng chung; hiện chỉ vai trò client)
                 cpp/: net/ decode/; gốc giữ JniBridge + ClientLoop
docs/            tài liệu
```

Build (CMake + Ninja — hoặc mở folder bằng Visual Studio):

```
cmake --preset x64-debug && cmake --build --preset x64-debug
→ out/build/x64-debug/client/windows/client.exe
→ out/build/x64-debug/core/core_tests.exe   (hoặc: make test)
```

## Mục lục

| Tài liệu | Nội dung |
|----------|----------|
| [01-architecture.md](01-architecture.md) | Kiến trúc tổng thể, pipeline, mục tiêu hiệu năng |
| [02-agent.md](02-agent.md) | Vai trò Agent (host): capture → encode → gửi |
| [03-client.md](03-client.md) | Vai trò Client: nhận → decode → render → input |
| [04-protocol.md](04-protocol.md) | Giao thức mạng UDP |
| [05-roadmap.md](05-roadmap.md) | Lộ trình giai đoạn + trạng thái |
| [06-phase3-transport.md](06-phase3-transport.md) | Thiết kế chi tiết GĐ3 (transport) |
| [07-phase4-input.md](07-phase4-input.md) | Thiết kế chi tiết GĐ4 (input) |
| [08-android-client.md](08-android-client.md) | Client Android (NDK, view-only) |
| [09-diagnostics.md](09-diagnostics.md) | Log chẩn đoán điểm nghẽn (`[DIAG]`, luôn bật) |

## Trạng thái

GĐ0 capture ✅ · GĐ1 encode (NVENC/MF) ✅ · GĐ2 loopback (~3.5 ms) ✅ ·
GĐ3 transport ✅ (1 máy; chờ M3/M4 LAN) · GĐ4 input 🔶 (code + M1/M2 xong,
chờ kiểm chứng 2 máy — xem 07 §9) · Client Android 🔶 (build + chạy được trên
emulator, vòng đời vào/ra sạch ✅; **chưa từng nhận một frame video nào** —
xem 08 §4)
