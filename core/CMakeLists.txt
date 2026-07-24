# core — phần dùng chung giữa các OS (Windows/macOS/Ubuntu/iOS/Android):
# giao thức wire, packetize/reassemble, session. Thuần C++20, KHÔNG include
# header hệ điều hành nào. Build được trong cây Deskhub lẫn standalone
# (clang/gcc, Xcode toolchain, Android NDK toolchain).
cmake_minimum_required(VERSION 3.20)
project(DeskhubCore LANGUAGES CXX)

# Nhóm theo tầng, dưới lên trên:
#   wire/      — khung byte trên dây (header chung, build/parse từng loại message)
#   transport/ — cắt/ghép frame thành datagram, FEC, chống trùng & mất gói
#   session/   — handshake + vòng đời phiên hai phía (host/client)
#   input/     — chuỗi sự kiện chuột/phím có thứ tự trên nền transport
#   control/   — policy điều tiết: bitrate/FEC phía host, gom số liệu phía client
add_library(core STATIC
    src/wire/Wire.cpp

    src/transport/Packetizer.cpp
    src/transport/Reassembler.cpp
    src/transport/RetransmitCache.cpp

    src/session/HostSession.cpp
    src/session/ClientSession.cpp
    src/session/ClipboardAssembler.cpp

    src/input/InputSender.cpp
    src/input/InputReceiver.cpp

    src/control/BitrateController.cpp
    src/control/LinkStats.cpp
)
target_compile_features(core PUBLIC cxx_std_20)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(core PRIVATE /W4 /permissive- /sdl)
endif()

# Test của core — chạy offline, không mạng/GPU, nên build được bằng mọi toolchain
# dựng được core. Mặc định tắt trên Android: exe host không chạy được trên thiết bị,
# và Gradle chỉ cần libdeskhub.so. Muốn chạy test bằng NDK thì bật tay:
#   cmake ... -DDESKHUB_CORE_TESTS=ON
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(deskhub_tests_default OFF)
else()
    set(deskhub_tests_default ON)
endif()
option(DESKHUB_CORE_TESTS "Build core_tests" ${deskhub_tests_default})

if(DESKHUB_CORE_TESTS)
    # Test tách theo tầng, gương với cây src/: mỗi module một file, dùng chung
    # tiện ích ở tests/support. TestMain gom lại và chạy tất cả.
    add_executable(core_tests
        tests/TestMain.cpp
        tests/support/TestSupport.cpp

        tests/wire/WireTests.cpp

        tests/transport/ReassemblerTests.cpp
        tests/transport/FecTests.cpp
        tests/transport/RetransmitCacheTests.cpp

        tests/session/SessionTests.cpp

        tests/input/InputTests.cpp

        tests/control/ControlTests.cpp
    )
    target_link_libraries(core_tests PRIVATE core)
    target_include_directories(core_tests PRIVATE tests)
    if(MSVC)
        target_compile_options(core_tests PRIVATE /W4 /permissive-)
    endif()

    include(CTest)
    add_test(NAME core_tests COMMAND core_tests)
endif()
