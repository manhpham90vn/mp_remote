# Deskhub Privacy Policy

_Effective date: July 24, 2026 — Version 1.0_

_(Bản tiếng Việt đầy đủ ở nửa sau tài liệu — [nhảy tới](#chính-sách-quyền-riêng-tư-tiếng-việt).)_

## 1. Introduction

This Privacy Policy describes how **Deskhub** ("the app", "we") handles
information when you use the Deskhub mobile applications (iOS, Android) and the
Deskhub host application for Windows (together, "the Software").

Deskhub is a remote desktop application: it streams the screen of your Windows
PC to your phone and lets you control the PC with touch and keyboard input.

The Software is developed and published by an individual developer:

- **Developer:** Manh Pham
- **Contact:** manhpv151090@gmail.com
- **Project page:** https://github.com/manhpham90vn/Deskhub

## 2. The short version

**Deskhub does not collect, store, sell, or share any personal data. We do not
operate any servers, and no data about you or your usage ever reaches us or any
third party through the Software.** There are no user accounts, no analytics,
no crash reporting, no advertising, and no third-party SDKs embedded in the
Software.

## 3. Information the Software processes

To function, the Software must process certain data **entirely on and between
your own devices**. None of it is transmitted to the developer or to any third
party.

| Data | Purpose | Where it goes | Retention |
|---|---|---|---|
| Screen content of your PC (video frames) | Displaying your PC screen on your phone | Sent directly from your PC to your phone, encrypted in transit only by your own network/VPN layer | Never stored; exists only in memory during the session |
| Mouse, keyboard, and touch input | Controlling your PC from your phone | Sent directly from your phone to your PC | Never stored; discarded after injection |
| Clipboard content | Copy/paste between phone and PC when you use the clipboard feature | Sent directly between your devices | Held only in the system clipboard of the receiving device |
| The PC address (IP/hostname) you type | Connecting to your PC | Stays on your phone | Kept locally until you change it |
| Connection statistics (bitrate, packet loss, latency) | Adapting stream quality; shown in the status bar | Exchanged only between your phone and your PC | Never stored; discarded when the session ends |

### 3.1 Peer-to-peer by design

All communication happens **directly between your phone and your PC** over:

- your local network (Wi-Fi/LAN), or
- a VPN that **you** operate or subscribe to (for example Tailscale), if you
  choose to use one for access over the Internet.

We do not operate relay servers, signaling servers, or any other backend. The
Software has no technical means to send data to the developer.

### 3.2 Data we do NOT process

The Software does not access or process: your name, email address, phone
number, contacts, location, photos, files (other than what is visible on the
PC screen you choose to stream), microphone, camera, advertising identifiers,
or any device identifiers beyond what the operating system needs to run the
app.

## 4. Permissions the apps request

| Platform | Permission | Why |
|---|---|---|
| iOS | Local Network | Required by iOS to send/receive traffic to your PC on the same network. Used only for the streaming session. |
| Android | `INTERNET`, network state | Required to open the UDP connection to your PC. Used only for the streaming session. |

The apps request no other permissions. If a future version needs a new
permission, it will be requested in-context and this policy will be updated.

## 5. Analytics, advertising, and third parties

- **Analytics / telemetry:** none.
- **Crash reporting:** none. Diagnostic logs (`[DIAG]`) exist only in the
  app's local console output and never leave your device unless you copy and
  send them yourself.
- **Advertising:** none.
- **Third-party SDKs:** none. The Software is built only from its own source
  code (available at the project page) and operating-system frameworks.
- **App stores:** the apps are distributed through Apple App Store and Google
  Play. Apple and Google may collect installation/usage statistics under their
  own privacy policies; that collection is outside our control and we receive
  only the aggregated, anonymous statistics those platforms show to every
  developer.
- **Tailscale or other VPNs:** if you choose to connect through a VPN, your
  traffic is handled under that provider's privacy policy. Deskhub neither
  requires nor bundles any VPN.

## 6. Security

- Streaming traffic stays inside your own network or your own VPN tunnel.
  When you use a VPN such as Tailscale, traffic between devices is end-to-end
  encrypted by that VPN (WireGuard).
- On a plain local network, traffic is not additionally encrypted by Deskhub;
  use it only on networks you trust, or through a VPN.
- Because we hold no data about you, there is no developer-side database that
  could be breached.

## 7. Data retention and deletion

We retain nothing, so there is nothing for us to delete. All session data
disappears when the session ends. The PC address saved in the app is removed
by clearing the field or uninstalling the app.

## 8. Your rights (GDPR, CCPA, and similar laws)

Laws such as the EU General Data Protection Regulation (GDPR) and the
California Consumer Privacy Act (CCPA) grant rights over personal data —
access, correction, deletion, portability, objection, and non-discrimination.

Because Deskhub does not collect or hold any personal data, there is no data
on which to exercise these rights. If you believe we do hold data about you,
contact us at the address below and we will respond within 30 days.

We do not "sell" or "share" personal information as defined by the CCPA.

## 9. Children's privacy

The Software is not directed at children and, as described above, collects no
data from anyone, including children under 13 (COPPA) or under 16 (GDPR).

## 10. International data transfers

None. Your data never leaves your own devices and networks through the
Software.

## 11. Changes to this policy

If the Software's data practices ever change (for example, if a future
version adds optional crash reporting), this policy will be updated **before**
the change ships, with a new effective date and a changelog entry below. The
current version is always published at:
https://github.com/manhpham90vn/Deskhub/blob/main/PRIVACY.md

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-07-24 | First publication. |

## 12. Contact

For any question about this policy or about privacy in Deskhub:

- **Email:** manhpv151090@gmail.com
- **Issues:** https://github.com/manhpham90vn/Deskhub/issues

---

# Chính sách quyền riêng tư (tiếng Việt)

_Hiệu lực: 24/07/2026 — Phiên bản 1.0_

## 1. Giới thiệu

Chính sách này mô tả cách **Deskhub** xử lý thông tin khi bạn dùng ứng dụng
Deskhub trên iOS/Android và ứng dụng host Deskhub trên Windows (gọi chung là
"Phần mềm").

Deskhub là ứng dụng điều khiển máy tính từ xa: stream màn hình PC Windows sang
điện thoại và cho bạn điều khiển PC bằng cảm ứng + bàn phím.

- **Nhà phát triển:** Manh Pham (nhà phát triển cá nhân)
- **Liên hệ:** manhpv151090@gmail.com
- **Trang dự án:** https://github.com/manhpham90vn/Deskhub

## 2. Tóm tắt

**Deskhub không thu thập, không lưu trữ, không bán, không chia sẻ bất kỳ dữ
liệu cá nhân nào. Chúng tôi không vận hành máy chủ nào, nên không dữ liệu nào
về bạn hay việc bạn dùng app đến được chúng tôi hay bên thứ ba.** Không tài
khoản người dùng, không analytics, không báo cáo crash, không quảng cáo, không
SDK bên thứ ba.

## 3. Dữ liệu Phần mềm xử lý

Để hoạt động, Phần mềm phải xử lý một số dữ liệu — **hoàn toàn trên và giữa
các thiết bị của chính bạn**, không gửi cho nhà phát triển hay bên thứ ba:

| Dữ liệu | Mục đích | Đi đâu | Lưu bao lâu |
|---|---|---|---|
| Nội dung màn hình PC (khung hình video) | Hiển thị màn hình PC trên điện thoại | Truyền thẳng PC → điện thoại | Không lưu; chỉ tồn tại trong bộ nhớ lúc đang stream |
| Thao tác chuột, phím, cảm ứng | Điều khiển PC từ điện thoại | Truyền thẳng điện thoại → PC | Không lưu; bỏ ngay sau khi thực thi |
| Nội dung clipboard | Copy/paste giữa hai thiết bị khi bạn dùng tính năng này | Truyền thẳng giữa hai thiết bị | Chỉ nằm trong clipboard hệ thống của máy nhận |
| Địa chỉ PC bạn nhập (IP/hostname) | Kết nối tới PC | Ở lại trên điện thoại của bạn | Lưu cục bộ tới khi bạn đổi |
| Số liệu kết nối (bitrate, mất gói, độ trễ) | Tự điều chỉnh chất lượng stream; hiện trên thanh trạng thái | Chỉ trao đổi giữa điện thoại và PC | Không lưu; xoá khi hết phiên |

**Peer-to-peer từ thiết kế:** mọi giao tiếp đi trực tiếp giữa điện thoại và PC
của bạn — qua mạng nội bộ (Wi-Fi/LAN) hoặc qua VPN do **bạn** vận hành (ví dụ
Tailscale) nếu muốn dùng qua Internet. Chúng tôi không có relay server,
signaling server hay backend nào; Phần mềm không có đường kỹ thuật nào để gửi
dữ liệu về nhà phát triển.

**Dữ liệu KHÔNG xử lý:** họ tên, email, số điện thoại, danh bạ, vị trí, ảnh,
tệp tin (ngoài những gì hiển thị trên màn hình PC bạn chọn stream), micro,
camera, mã định danh quảng cáo.

## 4. Quyền hệ thống app xin

| Nền tảng | Quyền | Lý do |
|---|---|---|
| iOS | Local Network (Mạng cục bộ) | iOS yêu cầu để gửi/nhận dữ liệu tới PC cùng mạng. Chỉ dùng cho phiên stream. |
| Android | `INTERNET`, trạng thái mạng | Mở kết nối UDP tới PC. Chỉ dùng cho phiên stream. |

Không xin quyền nào khác.

## 5. Analytics, quảng cáo, bên thứ ba

Không analytics/telemetry, không báo cáo crash (log chẩn đoán `[DIAG]` chỉ nằm
trong console cục bộ của máy bạn), không quảng cáo, không SDK bên thứ ba.
Apple App Store / Google Play có thể tự thu thập số liệu cài đặt theo chính
sách riêng của họ — việc đó ngoài kiểm soát của chúng tôi. Nếu bạn dùng VPN
(vd Tailscale), lưu lượng do chính sách của nhà cung cấp VPN đó điều chỉnh.

## 6. Bảo mật

Lưu lượng stream chỉ chạy trong mạng của bạn hoặc trong đường hầm VPN của bạn
(Tailscale mã hoá đầu-cuối bằng WireGuard). Trên mạng LAN thường, Deskhub
không mã hoá thêm — chỉ dùng trên mạng bạn tin cậy, hoặc qua VPN. Vì chúng tôi
không giữ dữ liệu nào của bạn nên không tồn tại cơ sở dữ liệu phía nhà phát
triển để có thể bị lộ.

## 7. Lưu trữ & xoá dữ liệu

Chúng tôi không giữ gì nên không có gì để xoá. Dữ liệu phiên biến mất khi phiên
kết thúc; địa chỉ PC lưu trong app xoá bằng cách sửa ô nhập hoặc gỡ app.

## 8. Quyền của bạn (GDPR, CCPA…)

Các luật như GDPR/CCPA cho bạn quyền truy cập, sửa, xoá, di chuyển dữ liệu…
Vì Deskhub không thu thập dữ liệu cá nhân nên không có dữ liệu nào để thực thi
các quyền đó. Nếu bạn cho rằng chúng tôi đang giữ dữ liệu của bạn, liên hệ
email bên dưới — chúng tôi phản hồi trong 30 ngày. Chúng tôi không "bán" hay
"chia sẻ" thông tin cá nhân theo định nghĩa của CCPA.

## 9. Trẻ em

Phần mềm không nhắm tới trẻ em và không thu thập dữ liệu từ bất kỳ ai, kể cả
trẻ dưới 13 tuổi (COPPA) hay dưới 16 tuổi (GDPR).

## 10. Chuyển dữ liệu xuyên biên giới

Không có — dữ liệu không bao giờ rời thiết bị và mạng của bạn thông qua Phần mềm.

## 11. Thay đổi chính sách

Nếu cách xử lý dữ liệu thay đổi (ví dụ bản sau thêm báo cáo crash tuỳ chọn),
chính sách sẽ được cập nhật **trước khi** thay đổi được phát hành, kèm ngày
hiệu lực mới và dòng changelog. Bản mới nhất luôn ở:
https://github.com/manhpham90vn/Deskhub/blob/main/PRIVACY.md

## 12. Liên hệ

- **Email:** manhpv151090@gmail.com
- **Issues:** https://github.com/manhpham90vn/Deskhub/issues
