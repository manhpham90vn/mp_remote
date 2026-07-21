# 09 — Log chẩn đoán điểm nghẽn

Hệ thống log `[DIAG]` trả lời câu hỏi: **khi giật/lag, khúc nào của đường ống đang
nghẽn** — encoder, đường gửi, mạng, hay đường nhận/decode. Thiết kế ra đời từ phép đo
`06-phase3-transport.md` §7b (chùm mất 384 gói, nghi vấn "mất thật vs tới muộn" chưa
được phân xử).

Nguyên tắc: **đường nóng chỉ cộng bộ đếm** (atomic/biến local, không I/O); mọi thứ in
ra ở nhịp thống kê 1 giây có sẵn hoặc khi có sự kiện hiếm. Core (`Reassembler`) phát
sự kiện qua callback `onFrameDrop`, không printf — đúng luật "core không I/O".

## 1. Cách thu log

Log `[DIAG]` **luôn bật, không có cờ**: diag chỉ thêm in log (bộ đếm luôn chạy, chi
phí không đáng kể), và khi sự cố xảy ra thì log đã-có-sẵn đáng giá hơn phải tái hiện
lại.

**Windows** (cả vai trò host lẫn client):

```
client.exe > diag-host.log 2>&1
```

**Android**:

```
adb logcat -s RemoteGame > diag-android.log
```

Khi cần chẩn đoán: chạy đúng kịch bản tái hiện, gom **cả hai file** (host + client)
quanh 1–2 phút lúc giật.

## 2. Định dạng

Một sự kiện một dòng: `[DIAG][<nguồn>] evt=<tên> k1=v1 k2=v2 ...`
Trường dạng `x=avg/max` là trung bình/đỉnh của cửa sổ 1 giây. Thời gian ms.

## 3. Sự kiện phía HOST (AgentLoop)

| Dòng | Khi nào | Ý nghĩa |
|---|---|---|
| `evt=idr bytes= pkts= burst_ms=` | mỗi IDR phát đi | Cỡ IDR — con số quyết định chẩn đoán chùm mất gói. `burst_ms` = thời gian bắn hết gói của frame đó. |
| `evt=sum enc_ms_avg/max idr= burst_ms_max= send_fail=` | 1s | `enc_ms` = thời gian encode; `send_fail` = sendto trả lỗi (buffer gửi đầy — mất gói ngay tại host). |
| `evt=sum loop_busy_ms_max=` | 1s | Thread Recv của host bận nhất bao lâu một vòng. |
| `evt=recv_stall busy_ms=` | busy >250 ms | Thread Recv nghẽn — buffer UDP kernel đang gánh. |

## 4. Sự kiện phía CLIENT (Windows + Android)

| Dòng | Khi nào | Ý nghĩa |
|---|---|---|
| `evt=frame_drop id= reason= miss=x/y pos= idr= waited_ms= got_bytes=` | mỗi frame bị khai tử | Khám nghiệm: `reason` = timeout / overtaken / evicted / pre_idr; `pos` = chùm thiếu nằm head/mid/tail/all — **tail là dấu vân tay của burst**. |
| `evt=kf_req reason=` | bắt đầu xin IDR | `reason` = loss / wait_idr / dec_fail / q_overflow. |
| `evt=idr_rx bytes= after_ms=` | IDR về sau khi xin | `after_ms` lớn + `kf_req` dồn dập = vòng xoáy IDR. |
| `evt=sum asm_ms= q_ms= dec_ms= dq_max= dq_drop= late= late_ms_avg/max= gap_ms_max= loop_busy_ms_max=` | 1s | Mổ xẻ trễ từng chặng — xem bảng dưới. (`q_ms`/`dq_max` chỉ có trên Windows.) |
| `evt=recv_stall busy_ms=` | busy >50 ms | Thread Recv/Net của client nghẽn. |

Các trường của `evt=sum` phía client:

- `asm_ms` — mảnh đầu tiên tới → frame ghép xong (mạng rải rác / chờ mảnh).
- `q_ms` — frame nằm chờ trong hàng đợi decode (decode không theo kịp).
- `dec_ms` — decode + render một frame.
- `dq_max` / `dq_drop` — độ sâu đỉnh của hàng đợi decode / số frame bị vứt vì đầy.
- `late` / `late_ms_*` — **gói VỀ MUỘN**: mảnh tới SAU khi frame của nó đã bị khai tử
  ("nghĩa địa" 16 frame trong `Reassembler`). Đây là phép đo phân xử §7b: `late`
  chiếm phần lớn loss → không phải mất gói, là deadline hết hạn trước khi đuôi kịp tới.
- `gap_ms_max` — khoảng lặng dài nhất giữa hai gói video liên tiếp (Wi-Fi
  nghẽn/power-save thì con số này nhảy lên hàng trăm).

## 5. Bảng tra: triệu chứng → điểm nghẽn

| Triệu chứng trong log | Điểm nghẽn | Hướng sửa |
|---|---|---|
| `evt=idr bytes=` hàng trăm KB | Encoder không chặn cỡ IDR | VBV cho MfEncoder (NVENC đã có, `NvencEncoder.cpp:148`) |
| `late=` ≈ phần lớn loss, `pos=tail` | Deadline Reassembler + đường truyền trễ, không phải mất thật | Nới deadline theo cỡ frame; giảm cỡ frame |
| `send_fail=` >0, `burst_ms_max=` lớn | Đường gửi/buffer host | SO_SNDBUF, xem lại burst |
| `gap_ms_max=` hàng trăm, `late=` cao | Wi-Fi nghẽn / power-save | DSCP, hạ bitrate, kiểm tra AP |
| `dec_ms` / `q_ms` / `dq_drop` cao | Client đuối | Giảm độ phân giải/fps; xem codec client |
| `kf_req` dồn dập + `idr_rx after_ms=` lớn | Vòng xoáy IDR | Chữa gốc loss trước, cân nhắc NACK |
| `recv_stall` / `loop_busy_ms_max=` lớn | Thread bị OS bỏ đói hoặc kẹt việc | Nâng ưu tiên thread; tìm việc chặn trong vòng |
| `enc_ms_max=` > khoảng frame (16 ms @60fps) | Encoder chậm | Preset/GPU load phía host |

## 6. Vị trí code

- Core: `rgc/transport/Reassembler.h` — `FrameDropInfo`/`onFrameDrop`, thống kê
  `latePackets`/nghĩa địa, `TakeMaxGapMs()`, `Frame::firstSeenUs`;
  `rgc/control/LinkStats.h` — `LinkWindow::latePackets/lateMsAvg/lateMsMax`.
- Windows: `client/windows/Diag.h` (helper `DiagAtomicMax`), `AgentLoop.cpp` (H1–H3),
  `ClientLoop.cpp` (K1–K4 + nối core).
- Android: `ClientLoop.cpp/.h` (bản rút gọn, không có `q_ms`/`dq_max`).
