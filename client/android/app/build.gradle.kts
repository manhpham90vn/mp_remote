plugins {
    id("com.android.application")
}

android {
    namespace = "com.rgc.remotegame"
    compileSdk = 37
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.rgc.remotegame"
        // 24: AMediaCodec NDK có từ 21, nhưng 24 mới đủ ổn định và phủ gần hết máy
        // còn dùng. Khóa "low-latency" chỉ có tác dụng từ 30, máy cũ lờ đi.
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-phase3"

        ndk {
            // arm64-v8a: máy thật. x86_64: máy ảo trên PC — không có nó thì emulator
            // phải chạy qua lớp dịch ARM (chậm, và không đáng tin với app giải mã
            // video). Thêm "armeabi-v7a" nếu cần máy cũ.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                // c++_static thay vì c++_shared: app chỉ có MỘT .so nên đây là lựa
                // chọn Google khuyến nghị, và quan trọng hơn — libc++_shared.so là
                // prebuilt của NDK 26 căn theo trang 4 KB, ta không re-link được nó
                // để căn 16 KB. Nhúng tĩnh thì alignment do ta quyết định hoàn toàn.
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
dependencies {
    // Hiện KHÔNG dòng code nào import androidx — UI chỉ dùng lớp framework
    // (android.app.Activity, android.widget.*). Giữ lại vì nó là nền cho hầu hết
    // thư viện Android sau này; bản 1.19.0 đòi compileSdk ≥ 37, đó là lý do
    // compileSdk ở trên là 37 chứ không phải 36.
    implementation("androidx.core:core-ktx:1.19.0")
}
