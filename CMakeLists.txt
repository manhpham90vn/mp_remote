# Remote game streaming/control — build bằng CMake + Ninja.
# Cấu trúc: core/ (thuần, dùng chung mọi OS) + client/<os>/ (app từng nền tảng,
# mỗi OS một file duy nhất kiểu AnyDesk — chứa cả vai trò host lẫn client).
# Windows: mở thư mục này bằng Visual Studio (Open Folder) hoặc từ Developer prompt:
#   cmake --preset x64-debug && cmake --build --preset x64-debug
cmake_minimum_required(VERSION 3.25)
project(Deskhub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(core)
add_subdirectory(platform)
if(WIN32)
    add_subdirectory(client/windows)
endif()
# client/android KHÔNG nằm ở đây: nó do Gradle/NDK dựng (client/android/, xem
# docs/08-android-client.md), và CMakeLists của nó tự add_subdirectory(core).
# Sau này: client/macos, client/linux, client/ios
