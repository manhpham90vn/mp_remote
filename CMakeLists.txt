# Remote game streaming/control — build bằng CMake + Ninja.
# Cấu trúc: core/ (thuần, dùng chung mọi OS) + client/<os>/ (app từng nền tảng,
# mỗi OS một file duy nhất kiểu AnyDesk — chứa cả vai trò host lẫn client).
# Windows: mở thư mục này bằng Visual Studio (Open Folder) hoặc từ Developer prompt:
#   cmake --preset x64-debug && cmake --build --preset x64-debug
cmake_minimum_required(VERSION 3.25)
project(RemoteGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(core)
if(WIN32)
    add_subdirectory(client/windows)
endif()
# Sau này: client/macos, client/linux, client/ios, client/android
