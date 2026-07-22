# CMake do Gradle/NDK gọi — dựng libdeskhub.so (toàn bộ app, không có Java).
# Cây core/ được kéo vào nguyên vẹn: nó thuần C++20 không header hệ điều hành nên
# biên dịch bằng toolchain NDK không cần sửa gì (xem core/CMakeLists.txt).
cmake_minimum_required(VERSION 3.22.1)
project(deskhub LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# <repo>/client/android/app/src/main/cpp -> lùi 6 mức về gốc repo.
get_filename_component(DESKHUB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)
add_subdirectory(${DESKHUB_ROOT}/core ${CMAKE_CURRENT_BINARY_DIR}/core)
# platform/ — lớp mỏng bọc header hệ điều hành (Clock.h); INTERFACE lib, không sinh
# .a. Nhánh #else của Clock.h (clock_gettime) là nhánh Android dùng.
add_subdirectory(${DESKHUB_ROOT}/platform ${CMAKE_CURRENT_BINARY_DIR}/platform)

# Không còn native_app_glue: vòng đời app do Activity Kotlin lo, native chỉ là thư
# viện được gọi qua JNI. Nhờ vậy cũng bỏ được cả trò -u ANativeActivity_onCreate.
# Nhóm theo thư mục con giống client/windows: net/ (socket, truy vấn nguồn),
# decode/ (MediaCodec). Ở gốc còn JniBridge (mặt tiếp xúc Kotlin) + ClientLoop.
add_library(deskhub SHARED
    JniBridge.cpp
    ClientLoop.cpp

    net/UdpSocket.cpp
    net/SourceQuery.cpp

    decode/MediaCodecDecoder.cpp
)

# Include gốc cpp/ để mọi #include ghi đủ đường dẫn nhóm ("net/UdpSocket.h").
target_include_directories(deskhub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(deskhub
    PRIVATE
        core        # deskhub:: Wire/ClientSession/Reassembler/...
        platform    # NowUs()
        android     # ANativeWindow, ANativeWindow_fromSurface
        mediandk    # AMediaCodec, AMediaFormat
        log
)

# Android 15 trở đi có thiết bị (và ảnh máy ảo "..._16k") dùng trang nhớ 16 KB.
# NDK 26 vẫn căn ELF theo 4 KB, nạp lên máy đó là chết ngay:
#   dlopen failed: program alignment (4096) cannot be smaller than system page size (16384)
# NDK r27+ mặc định làm việc này; ở r26 phải tự khai. Căn 16 KB vẫn chạy bình
# thường trên máy 4 KB nên bật vô điều kiện.
target_link_options(deskhub PRIVATE -Wl,-z,max-page-size=16384)

target_compile_options(deskhub PRIVATE -Wall -Wextra)
