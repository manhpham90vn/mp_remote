# 13 — Phát hành mobile qua fastlane + GitHub Actions

Hướng dẫn cấu hình MỘT LẦN để pipeline phát hành chạy được. Sau khi xong phần
này, quy trình hằng ngày chỉ còn: sửa code → push tag `v*` (hoặc bấm Run
workflow) → bản mới tự lên TestFlight + Play internal.

## Bức tranh chung

| Thứ | Nơi quản lý |
|---|---|
| Bundle ID iOS + Team ID | `client/ios/fastlane/Appfile` |
| Package Android | `client/android/fastlane/Appfile` |
| Version hiển thị (x.y.z, chung 2 nền tảng) | file `VERSION` ở gốc repo |
| Build number / versionCode | tự tăng theo `github.run_number` |
| Store listing (description, keywords…) | file txt trong `client/<os>/fastlane/metadata/` |
| Khoá/chứng chỉ/API key | GitHub Secrets (bảng dưới) |

Workflow:
- `deploy.yml` — build + đẩy bản (tag `v*` hoặc bấm tay).
- `metadata.yml` — đẩy store listing (chỉ bấm tay, không build app).

## Bước 1 — iOS: App Store Connect API key

1. Vào [App Store Connect](https://appstoreconnect.apple.com) → **Users and
   Access** → tab **Integrations** → **App Store Connect API** → **Team Keys**
   → dấu **+**.
2. Name tuỳ ý (vd `deskhub-ci`), Access chọn **App Manager** → Generate.
3. Ghi lại **Key ID** và **Issuer ID** (hiện đầu trang). Tải file
   `AuthKey_XXXX.p8` về (chỉ tải được MỘT lần — cất kỹ).
4. Encode base64 một dòng:

   ```sh
   base64 -i AuthKey_XXXX.p8 | tr -d '\n'
   ```

Lưu ý: bundle ID app tạo trên App Store Connect phải khớp `app_identifier`
trong `client/ios/fastlane/Appfile` (hiện là `com.ios.deskhub`) — lệch thì sửa
Appfile theo cái đã đăng ký.

Ký app: pipeline dùng **cloud signing** (`-allowProvisioningUpdates` + API
key) — xcodebuild tự tạo/tải certificate + provisioning profile, KHÔNG phải
import cert vào runner. Không cần làm gì thêm ở bước này.

## Bước 2 — Android: keystore ký release

Tạo keystore (một lần, giữ vĩnh viễn — mất là không update app được nữa):

```sh
keytool -genkeypair -v -keystore release.keystore -alias deskhub \
    -keyalg RSA -keysize 2048 -validity 10000
```

Nhớ mật khẩu store + key. Encode base64:

```sh
base64 -i release.keystore | tr -d '\n'
```

Khuyến nghị: khi tạo app trên Play Console, bật **Play App Signing** (mặc
định) — Google giữ khoá ký cuối, keystore này chỉ là upload key, lỡ mất còn
xin cấp lại được.

## Bước 3 — Android: service account cho Play Console

(Trang "Setup → API access" cũ đã bị Google gỡ khỏi Play Console cuối 2024 —
không còn bước link Google Cloud project nữa; giờ service account được mời
thẳng như một user thường.)

1. Vào [Google Cloud Console](https://console.cloud.google.com): tạo (hoặc
   chọn) một project bất kỳ.
2. **APIs & Services** → **Library** → tìm **Google Play Android Developer
   API** → **Enable** cho project đó.
3. **IAM & Admin** → **Service Accounts** → **Create service account** (vd
   `deskhub-publisher`; KHÔNG cần cấp role IAM nào của Google Cloud) → vào tab
   **Keys** của account vừa tạo → **Add key** → **Create new key** → **JSON**
   → tải file về. Ghi lại email của service account
   (dạng `deskhub-publisher@<project>.iam.gserviceaccount.com`).
4. Sang [Play Console](https://play.google.com/console) → **Users and
   permissions** → **Invite new users** → dán email service account ở trên →
   tab **App permissions** → chọn app `com.manhpham.deskhub` → cấp các quyền:
   - **Releases**: "Release apps to testing tracks" + "Manage testing tracks
     and edit tester lists" (đủ cho track internal; muốn CI phát hành thẳng
     production thì thêm "Release to production")
   - **Store presence**: "Edit store listing, pricing & distribution" (cho
     lane metadata)
   rồi **Invite user** / **Send invite** (service account nhận quyền ngay,
   không cần bấm chấp nhận lời mời).
5. Secret `PLAY_JSON_KEY` = NGUYÊN VĂN nội dung file JSON (không base64).

Nếu lần chạy đầu supply báo lỗi 401/403 "The current user has insufficient
permissions": kiểm tra đã Enable đúng API ở bước 2 và đợi vài phút cho quyền
lan truyền — quyền mới cấp trên Play đôi khi trễ tới 24h.

Nếu bước tạo key JSON báo **"An Organization Policy that blocks service
accounts key creation..."**: tài khoản GCP đang nằm trong organization
(Google Workspace) có policy `iam.disableServiceAccountKeyCreation` bật mặc
định. Lối thoát nhanh nhất: tạo project bằng **Gmail cá nhân** (No
organization) — project GCP không cần cùng tài khoản với Play Console, chỉ
cần email service account được mời ở bước 4. Còn muốn giữ trong org thì phải
tự cấp role Organization Policy Administrator rồi Override policy đó về Off
cho project (IAM & Admin → Organization Policies → "Disable service account
key creation").

⚠️ Play Console yêu cầu **bản AAB đầu tiên phải upload TAY** qua giao diện web
(Internal testing → Create release) thì API mới dùng được. Lấy file để upload:
chạy workflow `deploy` một lần (job android sẽ build ra artifact
`deskhub-android-aab` — tải về từ trang run, kể cả khi bước upload Play fail),
hoặc build local có ký bằng cách đặt 4 biến env keystore rồi
`cd client/android && ./gradlew bundleRelease`.

## Bước 4 — Khai GitHub Secrets (environment `stg`)

Secrets nằm trong **environment** `stg` (không phải repository secrets): Repo →
**Settings** → **Environments** → **stg** → Environment secrets → Add secret.
Các job trong deploy.yml/metadata.yml đã khai `environment: stg` để đọc chúng —
đổi tên environment thì phải sửa cả hai workflow.

| Secret | Giá trị |
|---|---|
| `ASC_KEY_ID` | Key ID (bước 1) |
| `ASC_ISSUER_ID` | Issuer ID (bước 1) |
| `ASC_KEY_CONTENT` | file .p8 đã base64 (bước 1) |
| `ANDROID_KEYSTORE_BASE64` | keystore đã base64 (bước 2) |
| `KEYSTORE_PASSWORD` | mật khẩu store (bước 2) |
| `KEY_ALIAS` | alias, vd `deskhub` (bước 2) |
| `KEY_PASSWORD` | mật khẩu key (bước 2) |
| `PLAY_JSON_KEY` | JSON service account nguyên văn (bước 3) |

## Bước 5 — Chạy

- **Phát hành bản mới**: sửa `VERSION` (vd `0.2.0`), cập nhật
  `client/ios/fastlane/metadata/*/release_notes.txt` và
  `client/android/fastlane/metadata/android/*/changelogs/default.txt`, commit,
  rồi `git tag v0.2.0 && git push origin v0.2.0` (hoặc Actions → deploy → Run
  workflow). iOS lên TestFlight, Android lên track internal — phát hành rộng
  hơn thì promote trong ASC/Play Console.
- **Sửa store listing**: sửa file txt trong `fastlane/metadata/` → commit →
  Actions → **metadata** → Run workflow.
- **Chạy tay trên máy dev** (cần `brew install fastlane`): đặt các env như
  bảng secrets rồi `cd client/ios && fastlane ios release` /
  `cd client/android && fastlane android release`.

## Việc còn lại làm tay trên hai console (một lần)

- **Screenshots**: cả hai chợ đều bắt buộc, hiện pipeline skip
  (`skip_upload_screenshots`) — upload tay trong ASC/Play Console.
- **App Privacy (Apple)**: khai "Data Not Collected" trong ASC → App Privacy;
  URL privacy policy đã có sẵn trong metadata
  (https://github.com/manhpham90vn/Deskhub/blob/main/PRIVACY.md — file
  PRIVACY.md ở gốc repo).
- **Data safety (Google)**: khai tương tự ("No data collected") trong Play
  Console → App content.
- **Play App content** còn lại: target audience, ads declaration ("No ads"),
  content rating questionnaire.
