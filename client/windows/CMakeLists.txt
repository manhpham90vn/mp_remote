# client/windows — app Windows, MỘT exe duy nhất (kiểu AnyDesk) chứa cả hai vai trò:
#   --serve  = host/agent (capture, encode, phát video, nhận input)
#   --connect ip:port = client (nhận video, decode, render, gửi input)
# Phần thuần đa nền tảng nằm ở core/ (link qua target `core`).

# Thư mục chứa nvEncodeAPI.h — đang pin header NVENC 13.0 (khớp driver hiện tại).
# Sau khi update driver NVIDIA hỗ trợ API 13.1, đổi bằng:
#   cmake --preset x64-debug -DNVENC_INTERFACE_DIR="C:/Tools/Video_Codec_Interface_13.1.15/Interface"
set(NVENC_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nvenc-13.0/include/ffnvcodec"
    CACHE PATH "Thu muc chua nvEncodeAPI.h")

add_executable(client
    main.cpp
    WindowCapture.cpp
    WindowFinder.cpp
    BmpWriter.cpp
    GpuSelect.cpp
    MfEncoder.cpp
    NvencEncoder.cpp
    EncoderFactory.cpp
    MfDecoder.cpp
    Renderer.cpp
    UdpSocket.cpp
    NetInfo.cpp
    AgentLoop.cpp
    ClientLoop.cpp
    InputCapture.cpp
    InputInjector.cpp
    NetTest.cpp
    ElevatedShare.cpp
    WindowPickerDialog.cpp
    SourcePickerDialog.cpp
    MainMenuWindow.cpp
)

target_compile_definitions(client PRIVATE UNICODE _UNICODE _CONSOLE)
target_include_directories(client PRIVATE "${NVENC_INTERFACE_DIR}")
target_link_libraries(client PRIVATE core)

if(MSVC)
    target_compile_options(client PRIVATE /W3 /permissive- /sdl)
endif()
