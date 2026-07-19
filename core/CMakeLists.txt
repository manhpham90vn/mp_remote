# core — phần dùng chung giữa các OS (Windows/macOS/Ubuntu/iOS/Android):
# giao thức wire, packetize/reassemble, session. Thuần C++20, KHÔNG include
# header hệ điều hành nào. Build được trong cây RemoteGame lẫn standalone
# (clang/gcc, Xcode toolchain, Android NDK toolchain).
cmake_minimum_required(VERSION 3.20)
project(RemoteGameCore LANGUAGES CXX)

add_library(core STATIC
    src/Wire.cpp
)
target_compile_features(core PUBLIC cxx_std_20)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(core PRIVATE /W4 /permissive- /sdl)
endif()
