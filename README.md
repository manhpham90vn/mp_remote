# Deskhub

Stream + điều khiển ứng dụng từ xa. Mỗi OS **một exe duy nhất** (kiểu AnyDesk), điều
khiển hoàn toàn qua giao diện Win32 — không cần tham số dòng lệnh.

```
core/            phần dùng chung giữa các OS (protocol, C++20 thuần, không header OS)
platform/        lớp mỏng bọc header hệ điều hành (Clock.h) — cái core không được chạm
client/windows/  app Windows (một exe: host + client)
client/android/  app Android (UI Kotlin + lõi C++ dùng chung; hiện chỉ vai trò client)
docs/            tài liệu thiết kế (bắt đầu từ docs/README.md)
```

`core/` chia theo tầng: `wire/` (khung byte) → `transport/` (cắt/ghép gói, FEC) →
`session/` (handshake, vòng đời phiên), cộng `input/` và `control/` (bitrate, thống kê).
`client/windows/` chia theo chức năng: `net/ capture/ encode/ decode/ input/ ui/`.

## Test

```
make test              # core_tests — chạy offline, không cần mạng/GPU
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
make run       # chạy client.exe, mở thẳng man hình chính (GUI)
```

Cửa sổ chính (`MainMenuWindow`, xem `client/windows/ui/MainMenuWindow.h`) hiện địa chỉ
IP máy này theo từng card mạng, ô chỉnh **Port/FPS/Bitrate**, nút **"Chia sẻ ứng
dụng trên máy này"** (mở hộp thoại chọn cửa sổ nguồn — `WindowPickerDialog.h` — rồi
làm host) và ô nhập IP + nút **"Kết nối"** để làm client. Checkbox "Cho phép người
kia điều khiển" nằm ngay trong hộp thoại chọn cửa sổ; checkbox "Chỉ xem, không gửi
input" nằm cạnh nút Kết nối.

Giai đoạn 3 đã chạy được trên 1 máy (M1+M2); còn kiểm chứng 2 máy LAN (M3/M4) — xem
`docs/06-phase3-transport.md` §8 và tiến độ trong `docs/05-roadmap.md`. Máy host lần đầu
cần mở firewall: `netsh advfirewall firewall add rule name="Deskhub" dir=in action=allow protocol=udp localport=47777`.

## Điều khiển từ xa (giai đoạn 4)

Input bật sẵn: gõ phím / di chuột trên cửa sổ preview của client là điều khiển máy host.
Cửa sổ preview có sẵn 2 nút overlay góc trên-phải (khoá chuột / tạm dừng) đi cùng đường
với phím tắt bên dưới — bấm nút hoặc bấm phím đều được, trạng thái luôn khớp nhau.

- `F9` khoá/thả chuột — **bắt buộc bật cho game FPS** (gửi chuột tương đối thay vì toạ độ).
- `F10` tạm dừng/tiếp tục gửi input.
- Ở máy host, **bấm vào cửa sổ đang chia sẻ một lần** để nó có focus: input chỉ được bơm
  khi cửa sổ đó đang foreground (cố ý — nếu không, người điều khiển từ xa sẽ gõ nhầm vào
  ứng dụng khác của bạn).
- Game/app chạy quyền admin → chạy host **as administrator**, không thì input bị UIPI chặn.
- **Input phải test bằng 2 máy**, không test được trên 1 máy (phím agent bơm ra sẽ bị chính
  cửa sổ client bắt lại → vòng lặp). Chi tiết: `docs/07-phase4-input.md` §7.
