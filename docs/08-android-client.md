# GĐ3 — Client Android (view-only)

Client Android đầu tiên, **chỉ xem** — chưa gửi input.
Mạng + giải mã là C++ (dùng lại `core/`), UI là Kotlin + Jetpack Compose.

## 1. Phân chia Kotlin / C++

`core/` đã là C++20 không đụng header hệ điều hành (`core/CMakeLists.txt` ghi rõ nó
phải build được bằng toolchain NDK). Nên toàn bộ `ClientSession` / `Reassembler` /
`Wire` dùng lại y nguyên. Phần phải viết mới chỉ là ba lớp mỏng platform-specific,
đối ứng 1-1 với bên Windows:

| Windows                    | Android                     | Vai trò                       |
|----------------------------|-----------------------------|-------------------------------|
| `UdpSocket.cpp` (winsock)  | `UdpSocket.cpp` (BSD)       | datagram vào/ra               |
| `MfDecoder` + `Renderer`   | `MediaCodecDecoder`         | H.264 -> màn hình             |
| `MainMenuWindow` (Win32)   | `MainActivity` (Compose)    | nhập địa chỉ, chọn kết nối    |
| `SourcePickerDialog`       | `MainActivity` (một bước)   | chọn cửa sổ muốn xem          |
| cửa sổ preview             | `StreamActivity` (Compose)  | SurfaceView + overlay số liệu |

Ranh giới cố ý mỏng, gói gọn trong `JniBridge.cpp` + `NativeClient.kt`: Kotlin chỉ
làm phần người dùng nhìn thấy và bơm Surface xuống; **không frame video nào đi qua
JVM, và cũng không qua Compose**. Video nằm trong một `SurfaceView` bọc bằng
`AndroidView`; MediaCodec được configure thẳng với `ANativeWindow` lấy từ Surface
của nó, nên đường nóng vẫn là bộ giải mã phần cứng -> hardware composer. Compose chỉ
vẽ phần chrome (ô nhập địa chỉ, chữ trạng thái) và cập nhật 500ms/lần.

Tỉ lệ khung hình do `Modifier.aspectRatio(w/h)` lo — không phải tự tính layout params.

> Bản đầu tiên là NativeActivity thuần native, không một dòng Kotlin. Bỏ vì nó
> không có cách nào nhập địa chỉ host (phải truyền qua `adb --es addr`) và mọi
> trạng thái — thiếu địa chỉ, đang kết nối, host không trả lời — đều hiện ra một
> màn hình đen giống hệt nhau. Đổi sang Activity thường cũng sửa luôn được vụ
> video bị kéo giãn sai tỉ lệ, thứ mà NativeActivity không làm nổi.

`MediaCodecDecoder` gộp cả decode lẫn render vì AMediaCodec được configure thẳng
với `ANativeWindow`: `AMediaCodec_releaseOutputBuffer(..., true)` đẩy frame từ bộ
giải mã phần cứng lên hardware composer, không qua CPU. Bên Windows cần `Renderer`
riêng để chuyển NV12 -> BGRA; ở đây không cần.

## 2. Thread

Giữ nguyên bố cục của `client/windows/ClientLoop.cpp`:

- **UI** (main thread của Android): `SurfaceHolder.Callback` giao/thu hồi Surface,
  và hỏi trạng thái 500ms/lần để cập nhật overlay.
- **Net**: `recvfrom(10ms)` -> `ClientSession` + `Reassembler` -> đẩy frame vào hàng đợi.
- **Decode**: rút frame -> `MediaCodecDecoder`.

Tách Net và Decode vì lý do cũ: decode chặn thread Net thì `recvfrom` ngừng nghe,
buffer UDP của OS tràn, sinh mất gói THẬT. Hàng đợi giới hạn 3 frame; đầy thì bỏ
frame cũ nhất và xin IDR.

## 3. Điểm khác biệt so với client Windows

**Surface đến rồi đi.** Android thu hồi Surface mỗi lần app xuống nền
(`surfaceDestroyed`), và hủy nó ngay sau khi callback trả về. Codec còn render
vào đó = dùng-sau-giải-phóng. Nên `ClientLoop::SetWindow(nullptr)` **chặn** tới khi
thread Decode xác nhận đã buông codec, bắt tay qua cặp `winGen_` / `winAckGen_`.
Quay lại nền trước thì codec được dựng lại và một IDR được xin ngay.

**SPS/PPS.** NVENC bật `repeatSPSPPS` nên mỗi IDR mang sẵn tham số in-band. Đa số
bộ giải mã Android nuốt được, nhưng vài dòng máy đòi tham số tới trong buffer riêng
đánh cờ `CODEC_CONFIG`. `FirstVclOffset()` tách phần trước slice đầu tiên và nạp
riêng một lần, rồi vẫn gửi trọn frame — SPS/PPS trùng là hợp lệ.

**RECONFIG.** `MfDecoder` tự đàm phán lại kích thước qua
`MF_E_TRANSFORM_STREAM_CHANGE`. MediaCodec đã configure cứng kích thước, nên
`onReconfig` đặt cờ dựng lại codec. Host gửi kèm IDR nên không mất gì.

**Chọn nguồn.** `QuerySources()` (`SourceQuery.cpp`) là bản port của
`QueryHostSources()` bên Windows: mở socket riêng, phát `LIST_SOURCES` mỗi 500ms trong
3 giây, chờ `SOURCE_LIST`. Đứng NGOÀI `ClientLoop` vì nó chạy trước khi có phiên —
không sessionId, không thread. Nó chặn tới 3 giây nên `NativeClient.listSources()` bọc
lại bằng `withContext(Dispatchers.IO)`.

JNI trả về `Array<String>` mỗi dòng `"id\twidth\theight\tname"` chứ không dựng object
Kotlin từ C++: bên đó chỉ cần `NewStringUTF`, khỏi phải tra `FindClass`/`GetMethodID`
cho một kiểu chỉ dùng đúng một chỗ. Kotlin `split('\t', limit = 4)` — `limit` là bắt
buộc, tiêu đề cửa sổ có thể chứa tab.

Danh sách rỗng gộp hai trường hợp — host im lặng (bản cũ, hoặc mất gói) và host không
chia sẻ gì — thành một: cứ vào nguồn 0 và để `ClientSession` báo lỗi thật. Một nguồn
thì bỏ qua luôn màn chọn. Khác `SourcePickerDialog` bên Windows ở chỗ chỉ chọn được
MỘT nguồn: bên đó mỗi nguồn là một cửa sổ preview riêng, ở đây chỉ có một Activity.

**Độ trễ.** Đặt khóa `"low-latency"` (đối ứng `MF_LOW_LATENCY`) để codec không giữ
frame sắp xếp lại thứ tự hiển thị — chuỗi của ta không có B-frame nên không mất gì.
Số e2e đo trên frame VỪA LÊN MÀN HÌNH (`lastRenderedPtsUs`), không phải lúc nạp vào
codec, để so sánh được với con số của client Windows.

## 4. Build và chạy

Đã build thành công lần đầu 2026-07-20: `app-debug.apk` 1.7 MB, chứa
`lib/arm64-v8a/libdeskhub.so`.

**Ràng buộc phiên bản — mỗi dòng dưới đây đều từng làm build chết thật:**

| Thành phần | Bản | Vì sao |
|---|---|---|
| AGP        | 9.3.0 | Đòi Gradle ≥ 9.5.0. |
| Gradle     | 9.6.1 | Dưới 9.5.0 thì AGP 9.3 từ chối ngay ở `version-check`. |
| Kotlin     | *tích hợp trong AGP* | **Không** khai `org.jetbrains.kotlin.android`: từ AGP 9, Kotlin nằm sẵn trong plugin Android, khai thêm là lỗi. Cũng vì thế không còn khối `kotlinOptions`/`kotlin { compilerOptions }`. |
| Compose compiler | plugin `org.jetbrains.kotlin.plugin.compose` 2.4.10 | Từ Kotlin 2.0, bật `buildFeatures { compose = true }` mà thiếu plugin này là lỗi cấu hình. Nó **độc lập** với `kotlin.android` nên vẫn phải khai tay dù AGP 9 đã có Kotlin. |
| Compose libs | BOM `2026.06.01` | BOM giữ mọi artifact Compose cùng thế hệ; các dòng `implementation` khác không ghi phiên bản. |
| JDK        | 21 (JBR) | Ghim ở `~/.gradle/gradle.properties`, xem dưới. |
| compileSdk | **37** | `androidx.core:core-ktx:1.19.0` đòi ≥ 37. Phải cài `platforms;android-37.0` bằng sdkmanager. |
| targetSdk  | 36 | Cố tình thấp hơn compileSdk: biên dịch với API mới nhất nhưng chỉ cam kết hành vi ở mức đã kiểm chứng. |
| NDK        | 26.1.10909125 | Pin trong `app/build.gradle.kts`. |
| CMake      | 3.22.1 | Bản SDK cung cấp. |

Lịch sử để khỏi đi lại vết xe đổ: bộ ban đầu là AGP 8.5.2 + Gradle 8.9. Không lên
Gradle 9 được vì Gradle 9 bỏ hẳn `Project.exec()` mà AGP 8.5.2 còn gọi
(`NoSuchMethodError`). Lối thoát là nâng AGP lên 9.x chứ không phải ghìm Gradle lại.

### Trang nhớ 16 KB (Android 15+)

Máy ảo `sdk_gphone16k_*` và thiết bị Android 15+ dùng trang nhớ **16 KB**. NDK 26
căn ELF theo 4 KB, nạp lên đó là chết ngay khi mở app:

```
dlopen failed: ... program alignment (4096) cannot be smaller than system page size (16384)
```

Hai thứ phải làm cùng lúc, thiếu một cái là vẫn hỏng:

1. `target_link_options(deskhub PRIVATE -Wl,-z,max-page-size=16384)` trong
   `cpp/CMakeLists.txt`. (NDK r27+ mặc định làm, r26 phải tự khai.)
2. `-DANDROID_STL=c++_static` trong `app/build.gradle.kts`. `libc++_shared.so` là
   **prebuilt** của NDK 26 căn 4 KB — ta không re-link được nó, nên chỉ sửa cờ ở (1)
   thì `libdeskhub.so` nạp được rồi chết ở `libc++_shared.so`. Nhúng STL tĩnh thì
   app chỉ còn một `.so` do ta hoàn toàn kiểm soát alignment.

Kiểm tra lại bằng: `llvm-readelf -l libdeskhub.so` — cột cuối của các dòng `LOAD`
phải là `0x4000`, không phải `0x1000`.

### ABI

`abiFilters = ["arm64-v8a", "x86_64"]`. x86_64 là để máy ảo trên PC chạy **native**;
thiếu nó thì emulator phải dịch ARM — chậm và không đáng tin với app giải mã video.

JDK đã được ghim một lần cho mọi build ở **`~/.gradle/gradle.properties`** (file cấp
user, không nằm trong repo):

```properties
org.gradle.java.home=C:/Program Files/Android/Android Studio/jbr
```

Nên không cần đặt `JAVA_HOME` nữa — build được cả trong Android Studio lẫn dòng lệnh:

```powershell
cd client/android
.\gradlew.bat assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Hai điều cần biết về cách ghim này: nó áp cho **mọi** project Gradle trên máy (chấp
nhận được — JDK còn lại là bản 11 quá cũ), và vì file nằm ngoài repo nên máy khác
clone về sẽ phải tự ghim lại hoặc tự đặt `JAVA_HOME`. Cố tình không nhét đường dẫn
này vào `client/android/gradle.properties`: đó là file có commit, hardcode đường dẫn
riêng của một máy vào đấy là hỏng cho mọi máy khác.

Lưu ý cú pháp: trong file `.properties` thì `\` là ký tự escape, phải dùng `/` hoặc `\\`.

`local.properties` phải trỏ tới SDK (`sdk.dir=...`) — Android Studio tự ghi, và file
này KHÔNG commit.

Cảnh báo `[CXX5304] SDK XML version 4` khi configure CMake là vô hại: NDK 26.1 cũ hơn
SDK tools đang cài. Không ảnh hưởng kết quả biên dịch.

Mở app, gõ địa chỉ host, bấm **Connect** — địa chỉ được nhớ lại cho lần sau. Host chia
sẻ nhiều cửa sổ thì hiện màn chọn nguồn (Back quay lại ô địa chỉ). Vẫn chạy thẳng được
từ adb khi cần test nhanh — đường này bỏ qua màn chọn, luôn vào nguồn 0:

```
adb shell am start -n com.deskhub.app/.MainActivity --es addr 192.168.1.10:47777
adb logcat -s Deskhub
```

Máy Windows chạy `client.exe`, chọn **[s]** để chia sẻ một cửa sổ. Cả hai máy phải
cùng LAN và tường lửa Windows phải cho UDP 47777 vào.

## 4b. Quy ước ngôn ngữ

Theo yêu cầu của dự án: **mọi chuỗi người dùng hoặc console thấy đều bằng tiếng
Anh**, **mọi comment trong code bằng tiếng Việt có dấu**.

Cụ thể ở client Android: chuỗi UI nằm trong `res/values/strings.xml` (tiếng Anh,
không hard-code trong layout/Kotlin), log `LOGI/LOGW/LOGE` và dòng overlay dựng
trong `ClientLoop.cpp` cũng tiếng Anh.

Một ngoại lệ có lý do: `~/.gradle/gradle.properties` để comment tiếng Anh, vì Gradle
đọc file `.properties` theo ISO-8859-1 nên không mang được dấu tiếng Việt an toàn.

## 5. Hạn chế đã biết (chưa làm, không phải bug)

- **Một nguồn tại một thời điểm**: chọn được cửa sổ nào để xem, nhưng không xem được
  nhiều cửa sổ cùng lúc như client Windows.
- **Số đo e2e sai** (xem §6).
- **Chưa gửi input** (GĐ4 cho Android).
