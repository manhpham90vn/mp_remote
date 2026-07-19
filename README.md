# Remote Game Control

Stream + điều khiển game từ xa. Mỗi OS **một exe duy nhất** (kiểu AnyDesk):
`--serve` = host (máy chạy game), `--connect` = client (máy điều khiển).

```
core/            phần dùng chung giữa các OS (protocol, C++20 thuần)
client/windows/  app Windows (một exe: host + client)
docs/            tài liệu thiết kế (bắt đầu từ docs/README.md)
```

## Yêu cầu

- Windows 10 1903+ x64, Visual Studio 2022+ (workload C++, kèm sẵn CMake + Ninja).
- GPU NVIDIA khuyến nghị (NVENC); không có thì tự rơi về Media Foundation.

## Build

```
make                   # debug  → out\build\x64-debug\client\windows\client.exe
make release           # release
```

(Cách khác: mở thư mục repo bằng Visual Studio — Open Folder; hoặc tự chạy
`cmake --preset x64-debug && cmake --build --preset x64-debug` trong Developer prompt.)

## Run

```
make run                                  # không tham số: hiện danh sách cửa sổ để chọn, đo fps capture
make run ARGS="game.exe --save"           # capture + lưu frame BMP debug
make run ARGS="game.exe --encode --out out.mp4 --bitrate 20 --fps 60"
make run ARGS="game.exe --loopback"       # capture → encode → decode → render nội máy (đo trễ)
```

Giai đoạn 3 (đang làm) sẽ thêm: `--serve` / `--connect ip:port` — xem `docs/06-phase3-transport.md`.
