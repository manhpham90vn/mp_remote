# 07 — Giai đoạn 4: Input (bắt ở client → bơm ở host)

Đặc tả wire nằm ở `04-protocol.md` §6. Tài liệu này ghi lại **quyết định thiết kế**
và những chỗ dễ sai — phần lớn nằm ở Windows chứ không ở giao thức.

## 1. Phân chia core / platform

```
core/  (thuần C++20, dùng lại cho macOS/Linux/iOS/Android)
  Wire            InputEvent + BuildInputEvents/ParseInputEvents
  InputSender     gom event, đánh seq, gửi lặp chống mất gói      (client)
  InputReceiver   khử trùng theo seq, phát ra đúng thứ tự          (host)
  ClientSession   QueueInput() → Tick() gửi
  HostSession     nhận INPUT_EVENT → callback onInput

client/windows/input/
  InputCapture    Raw Input trên cửa sổ preview → InputEvent       (client)
  InputInjector   SendInput vào cửa sổ đang chia sẻ                (host)
```

Ràng buộc giữ nguyên từ GĐ3: core không có socket/thread/đồng hồ, thời gian bơm
từ ngoài → test được offline (`make test`).

## 2. seq theo TỪNG EVENT, không theo gói

`04-protocol.md` §6 để `seq` là số thứ tự **gói**. Khi hiện thực thì thấy như vậy
không đủ: chính sách chống kẹt phím là "client lặp lại event trạng thái vài lần",
mà nếu seq gắn với gói thì host **không phân biệt được** bản lặp với thao tác mới
— người dùng nhấn W một lần sẽ thành nhấn ba lần.

Tinh chỉnh (vẫn giữ nguyên layout wire): **`seq` là số thứ tự của event ĐẦU TIÊN
trong gói**, event thứ `i` mang seq `= seq + i`. Được cả hai việc bằng một trường:

- **khử trùng**: host chỉ áp dụng event có `seq > lastApplied` → gửi lặp vô hại;
- **phát hiện mất**: nhảy seq = số event mất thật sự, đếm được chính xác;
- **chống đảo thứ tự**: gói cũ về muộn mang toàn seq cũ → bỏ sạch, không "tua
  ngược" thao tác (không có chuyện phím đã nhả bị nhấn lại).

## 3. Chống kẹt phím — vì sao phải gửi lặp

Kênh input không có ACK. Mất gói chứa event **nhả phím** là lỗi tệ nhất: nhân vật
chạy mãi, chuột kẹt ở trạng thái giữ. Ba lớp bảo vệ:

1. **Redundancy trong gói**: mỗi datagram kèm `kInputRedundancy`=8 event đã gửi
   gần nhất. Mất một gói lẻ → gói kế tiếp bù ngay.
2. **Phát lại khi rảnh**: hết event mới, `InputSender` phát lại đuôi thêm 2 lần
   cách nhau 25 ms. Cần thiết vì gói **cuối cùng** (thường chính là event nhả phím)
   không có gói nào sau nó để bù.
3. **`ReleaseAll()` ở host**: BYE/timeout/mất focus/tắt input/agent thoát → nhả hết
   phím và nút đang giữ. Đây là lưới an toàn cuối; hai lớp trên chỉ giảm xác suất.

Chi phí: mỗi event 19 byte, đi ~3 lần → vài chục kbps lúc gõ liên tục. Không đáng
kể so với 20 Mbps video.

`core_tests` kiểm chứng đúng tính chất này: **bỏ 1/3 số datagram, mọi event vẫn được
áp dụng đúng một lần, đúng thứ tự, không mất, không lặp.**

## 4. Chuột: tuyệt đối vs tương đối

| | Tuyệt đối (mặc định) | Tương đối (F9) |
|---|---|---|
| Nguồn | `WM_MOUSEMOVE`, toạ độ client chuẩn hoá 0..65535 | Raw Input `lLastX/lLastY` |
| Con trỏ | tự do, hai máy trùng nhau | khoá trong cửa sổ preview + ẩn |
| Dùng cho | ứng dụng cửa sổ, menu, game chiến thuật | **game FPS** |

Vì sao phải có chế độ tương đối: game FPS đọc chuột thô rồi tự kéo con trỏ về
giữa màn hình. Gửi toạ độ tuyệt đối vào loại game đó thì camera giật liên tục
không chơi được. Ngược lại chế độ tương đối lại không dùng được cho menu.

Chuẩn hoá dùng mẫu số `extent-1` ở cả hai đầu để cạnh phải/dưới đạt đúng 65535,
không hụt một pixel. Client thu nhỏ cửa sổ preview vẫn trỏ đúng chỗ vì host quy
đổi ngược theo **đúng vùng WGC capture tại thời điểm nhận**: với nguồn cửa sổ đó
là cả khung cửa sổ kể cả thanh tiêu đề (DWM extended frame bounds, không phải
client rect — nhờ vậy bấm được cả nút đóng/thu nhỏ của cửa sổ được share), với
nguồn màn hình là rect của monitor.

## 5. Bàn phím: bơm SCANCODE, không bơm mã phím ảo

`SendInput` với `KEYEVENTF_SCANCODE`, cờ `KEYEVENTF_EXTENDEDKEY` khi bit E0 bật.

Đây là chỗ **quyết định sống chết của giai đoạn này**: game dùng DirectInput /
Raw Input đọc thẳng scancode từ driver, không đọc mã phím ảo. Gửi mỗi vkCode thì
gõ vào Notepad chạy tốt nhưng vào game **không có gì xảy ra** — đúng cái bẫy làm
phần lớn công cụ remote không điều khiển được game. Vì vậy `InputCapture` lấy
scancode từ `RAWKEYBOARD.MakeCode` (không phải từ `WM_KEYDOWN`) và mang nguyên
qua wire ở trường `b`.

Còn lại `a` = vkCode: dùng để đối chiếu/log và làm đường lùi khi client không có
scancode (client trên OS khác sau này).

## 6. Bơm vào đâu — bẫy foreground

`SendInput` bơm vào **cửa sổ đang foreground của máy host**, không bơm vào một
HWND cụ thể. Hệ quả: nếu người ở máy host bấm sang ứng dụng khác, mọi phím từ
client sẽ rơi vào ứng dụng đó — người điều khiển từ xa vô tình gõ vào trình
duyệt, terminal, chat của chủ máy.

Vì vậy `InputInjector::Apply` **kiểm tra foreground trước mỗi event**: chỉ bơm khi
cửa sổ đang chia sẻ (hoặc popup con của nó) đang foreground; không thì bỏ qua,
nhả hết phím đang giữ, và log một lần. Đây vừa là chống gõ nhầm vừa là ranh giới
an toàn đúng với ngữ nghĩa "tôi chỉ chia sẻ cửa sổ này".

`Init()` cố `SetForegroundWindow` một lần, nhưng Windows chặn process không có
focus gọi hàm này → **thường thất bại**, và khi đó agent in hướng dẫn: bấm vào
cửa sổ đó một lần ở máy host.

`--injecttest` (dev, không cần mạng) cũng dừng luôn nếu cửa sổ đích chưa
foreground, thay vì bắn phím mù vào ứng dụng đang mở.

## 7. Những giới hạn đã biết

- **Không chạy được trên 1 máy**: agent bơm phím vào foreground; nếu cửa sổ preview
  của client đang foreground thì phím vừa bơm lại bị chính client bắt và gửi đi →
  vòng lặp. Kiểm chứng input **phải dùng 2 máy** (video thì 1 máy vẫn test được).
- **Phím hệ thống không bắt được**: Alt+Tab, phím Windows, Ctrl+Alt+Del bị OS nuốt
  trước khi tới ứng dụng. Muốn có phải dùng low-level hook / driver (ngoài phạm vi v1).
- **UIPI**: app chạy quyền admin ở host chỉ nhận được input nếu **agent cũng chạy
  admin**. Game chạy admin (hoặc có anti-cheat) → chạy agent bằng "Run as administrator".
- **Anti-cheat**: một số game chặn input tổng hợp bằng `SendInput` (kiểm tra cờ
  `LLMHF_INJECTED`). Nếu gặp: thử ViGEm (giả lập tay cầm ở tầng driver) hoặc
  Interception — đã ghi sẵn trong roadmap GĐ4 làm phương án dự phòng.
- **Trễ input**: gộp và gửi trong `Tick` của vòng Recv client → trần trễ = timeout
  `recvfrom`, đã hạ **100 ms → 10 ms** ở GĐ4 vì lý do này (lúc màn hình tĩnh không
  có gói video nào đánh thức vòng lặp).

## 8. Phím tắt ở client

| Phím | Tác dụng |
|------|----------|
| `F9` | khoá/thả chuột (chuyển tuyệt đối ↔ tương đối) |
| `F10` | tạm dừng/tiếp tục gửi input |

Hai phím này xử lý **cục bộ**, không gửi đi. Khi đang gửi input, client nuốt luôn
phím thường (kể cả ESC) để người dùng gõ vào máy kia chứ không đóng cửa sổ preview
— đóng bằng nút X hoặc F10 rồi ESC. Mất focus khi đang khoá chuột → tự thả, không
để người dùng kẹt con trỏ.

**Tổ hợp hệ thống (GĐ8).** `Ctrl+Shift+Esc` bấm thật thì Windows máy *client* chặn
lấy (mở Task Manager của chính nó) trước khi tới ta — nên cửa sổ preview có nút
`⌨ Ctrl+Shift+Esc`: bấm nút là tổ hợp được xếp vào kênh input như chuỗi phím
thường (nhấn theo thứ tự, nhả ngược lại) và host bơm bằng scancode như mọi phím
khác. `Ctrl+Alt+Del` thì **không làm được** kể cả cách này: đó là Secure Attention
Sequence, `SendInput` ở host không giả được.

## 9. Mốc kiểm chứng

| Mốc | Nội dung | Trạng thái |
|-----|----------|-----------|
| M1 | `core_tests`: wire roundtrip, khử trùng khi mất 1/3 gói, chống đảo thứ tự | ✅ PASS |
| M2 | 2 process 1 máy: input đi trọn vòng client→host (`input N (mat 0)`), video không hồi quy | ✅ PASS |
| M3 | 2 máy LAN: gõ phím + chuột điều khiển được **ứng dụng thường** (Notepad/trình duyệt) | ⬜ |
| M4 | 2 máy LAN: điều khiển được **game thật** (chuột nhìn + WASD), đo trễ input | ⬜ |

M3/M4 bắt buộc 2 máy (xem §7). Nếu M4 trượt vì anti-cheat → chuyển hướng ViGEm.
