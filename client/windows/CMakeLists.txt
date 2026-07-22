# client/windows — app Windows, MỘT exe duy nhất (kiểu AnyDesk) chứa cả hai vai trò:
#   --serve  = host/agent (capture, encode, phát video, nhận input)
#   --connect ip:port = client (nhận video, decode, render, gửi input)
# Phần thuần đa nền tảng nằm ở core/ (link qua target `core`).

# Thư mục chứa nvEncodeAPI.h — đang pin header NVENC 13.0 (khớp driver hiện tại).
# Sau khi update driver NVIDIA hỗ trợ API 13.1, đổi bằng:
#   cmake --preset x64-debug -DNVENC_INTERFACE_DIR="C:/Tools/Video_Codec_Interface_13.1.15/Interface"
set(NVENC_INTERFACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/nvenc-13.0/include/ffnvcodec"
    CACHE PATH "Thu muc chua nvEncodeAPI.h")

# Nguồn nhóm theo thư mục con: net/ (socket, pacing), capture/ (WGC + D3D11),
# encode/ + decode/ (codec), input/, ui/ (dialog Win32). Ở gốc chỉ còn main + hai
# vai trò (AgentLoop = host, ClientLoop = client) + ElevatedShare (vòng đời UAC).
add_executable(client
    main.cpp
    AgentLoop.cpp
    ClientLoop.cpp
    ElevatedShare.cpp
    DiagLog.cpp

    net/UdpSocket.cpp
    net/NetInfo.cpp
    net/Firewall.cpp
    # net/Pacer.cpp — CHƯA nối vào build: không file nào include Pacer.h và nó chưa
    # từng được compile (thêm ở c55a8f9 nhưng thiếu bước wire vào Packetizer).
    # Giữ nguyên trạng thái cũ ở đây; xem ghi chú đầu net/Pacer.h.

    capture/WindowCapture.cpp
    capture/WindowFinder.cpp
    capture/GpuSelect.cpp
    capture/BmpWriter.cpp

    encode/MfEncoder.cpp
    encode/NvencEncoder.cpp
    encode/EncoderFactory.cpp

    decode/MfDecoder.cpp
    decode/Renderer.cpp

    input/InputCapture.cpp
    input/InputInjector.cpp

    ui/MainMenuWindow.cpp
    ui/WindowPickerDialog.cpp
    ui/SourcePickerDialog.cpp
)

target_compile_definitions(client PRIVATE UNICODE _UNICODE _CONSOLE)
# Include gốc client/windows để mọi #include ghi đủ đường dẫn nhóm ("net/UdpSocket.h").
target_include_directories(client PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${NVENC_INTERFACE_DIR}"
)
target_link_libraries(client PRIVATE core platform)

if(MSVC)
    # /utf-8: file nguồn là UTF-8 (comment tiếng Việt + chuỗi L"..." có ký tự ngoài
    # ASCII). Thiếu cờ này MSVC đọc theo codepage hệ thống nên byte UTF-8 trong chuỗi
    # rộng bị giải mã sai -> nhãn nút hiện ra ký tự rác.
    target_compile_options(client PRIVATE /W3 /permissive- /sdl /utf-8)
endif()
