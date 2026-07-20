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
make run                                  # không tham số: menu chính kiểu AnyDesk — hiện IP máy này
                                          #   theo từng card mạng, [s] chia sẻ ứng dụng (chọn cửa sổ),
                                          #   [c] / gõ thẳng ip[:port] để kết nối tới máy khác
make run ARGS="game.exe --serve"          # host: capture + NVENC → UDP (mặc định cổng 47777)
make run ARGS="--connect 192.168.1.10"    # client: nhận video, decode, hiển thị + log chỉ số mỗi 1s
make run ARGS="--nettest"                 # self-test offline packetize/reassemble/session/input (M1)
make run ARGS="game.exe --serve --noinput"   # host: chỉ cho xem, không cho điều khiển
make run ARGS="--connect 192.168.1.10 --noinput"  # client: chỉ xem, không gửi phím/chuột
make run ARGS="notepad.exe --injecttest"  # dev: thử riêng đường bơm input (không cần mạng)
make run ARGS="game.exe --save"           # capture + lưu frame BMP debug
make run ARGS="game.exe --encode --out out.mp4 --bitrate 20 --fps 60"
make run ARGS="game.exe --loopback"       # capture → encode → decode → render nội máy (đo trễ)
```

Giai đoạn 3 đã chạy được trên 1 máy (M1+M2); còn kiểm chứng 2 máy LAN (M3/M4) — xem
`docs/06-phase3-transport.md` §8 và tiến độ trong `docs/05-roadmap.md`. Máy host lần đầu
cần mở firewall: `netsh advfirewall firewall add rule name="RemoteGame" dir=in action=allow protocol=udp localport=47777`.

## Điều khiển từ xa (giai đoạn 4)

Input bật sẵn: gõ phím / di chuột trên cửa sổ preview của client là điều khiển máy host.

- `F9` khoá/thả chuột — **bắt buộc bật cho game FPS** (gửi chuột tương đối thay vì toạ độ).
- `F10` tạm dừng/tiếp tục gửi input.
- Ở máy host, **bấm vào cửa sổ đang chia sẻ một lần** để nó có focus: input chỉ được bơm
  khi cửa sổ đó đang foreground (cố ý — nếu không, người điều khiển từ xa sẽ gõ nhầm vào
  ứng dụng khác của bạn).
- Game/app chạy quyền admin → chạy host **as administrator**, không thì input bị UIPI chặn.
- **Input phải test bằng 2 máy**, không test được trên 1 máy (phím agent bơm ra sẽ bị chính
  cửa sổ client bắt lại → vòng lặp). Chi tiết: `docs/07-phase4-input.md` §7.
