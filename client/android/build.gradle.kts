// Kotlin chỉ dùng cho tầng UI (Activity, layout). Toàn bộ mạng + giải mã vẫn là
// C++ trong libremotegame.so, không đi qua JVM.
//
// Không khai plugin org.jetbrains.kotlin.android: từ AGP 9, Kotlin được tích hợp
// thẳng vào plugin Android, khai thêm plugin JetBrains là lỗi ("Failed to apply
// plugin ... Solution: Remove the plugin").
plugins {
    id("com.android.application") version "9.3.0" apply false
}
