plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "com.deskhub.app"
    compileSdk = 37
    ndkVersion = "26.1.10909125"

    defaultConfig {
        // Danh tính + version do FASTLANE quản (nguồn chuẩn: client/android/fastlane/
        // Appfile và file VERSION ở gốc repo) — release bơm vào qua -PapplicationId/
        // -PversionName/-PversionCode; giá trị ?: bên dưới chỉ là fallback build tay.
        // applicationId phải đúng package đã đăng ký Play Console (cấm prefix
        // com.android./com.google.). ĐỘC LẬP với `namespace`: namespace giữ nguyên
        // com.deskhub.app vì tên hàm JNI (Java_com_deskhub_app_*) ăn theo nó.
        applicationId = (project.findProperty("applicationId") as String?) ?: "com.manhpham.deskhub"
        // 24: AMediaCodec NDK có từ 21, nhưng 24 mới đủ ổn định và phủ gần hết máy
        // còn dùng. Khóa "low-latency" chỉ có tác dụng từ 30, máy cũ lờ đi.
        minSdk = 24
        targetSdk = 36
        versionCode = (project.findProperty("versionCode") as String?)?.toInt() ?: 1
        versionName = (project.findProperty("versionName") as String?) ?: "0.1-dev"

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

    // Ký release bằng keystore chỉ định qua env (CI/fastlane đặt — xem
    // client/android/fastlane/Fastfile). Máy dev không có env thì release để
    // unsigned như trước; debug luôn ký bằng khoá debug mặc định.
    if (System.getenv("KEYSTORE_FILE") != null) {
        signingConfigs {
            create("release") {
                storeFile = file(System.getenv("KEYSTORE_FILE"))
                storePassword = System.getenv("KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KEY_ALIAS")
                keyPassword = System.getenv("KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.findByName("release")
        }
        debug {
            isJniDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }
}
dependencies {
    // core-ktx 1.19.0 đòi compileSdk ≥ 37 — đó là lý do compileSdk ở trên là 37.
    implementation("androidx.core:core-ktx:1.19.0")

    // BOM giữ mọi artifact Compose cùng một thế hệ, nên các dòng dưới không ghi
    // phiên bản. Đây là toàn bộ tầng UI; đường nóng video KHÔNG đi qua Compose.
    implementation(platform("androidx.compose:compose-bom:2026.06.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.activity:activity-compose:1.13.0")
}
