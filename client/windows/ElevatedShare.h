#pragma once
// =============================================================================
// ElevatedShare.h — xin quyền admin cho vai trò HOST khi bật điều khiển.
//
// VẤN ĐỀ: UIPI (User Interface Privilege Isolation)
//   Windows không cho một tiến trình ở mức toàn vẹn THẤP gửi input tới cửa sổ của
//   tiến trình ở mức CAO hơn. Hệ quả rất khó chẩn đoán: host chạy quyền thường vẫn
//   bắt hình được game/ứng dụng chạy admin bình thường — hình vẫn hiện đẹp — nhưng
//   SendInput bị nuốt IM LẶNG. Không lỗi, không cảnh báo, chỉ là bấm gì cũng không
//   có gì xảy ra. (Xem docs/07-phase4-input.md.)
//
// GIẢI PHÁP: TỰ KHỞI ĐỘNG LẠI Ở MỨC CAO HƠN
//   Khi người dùng bấm Share có tick "cho phép điều khiển", ta chạy lại chính exe
//   này qua ShellExecuteEx với verb "runas" — Windows bung hộp thoại UAC. Instance
//   mới nhận nguyên phiên share qua DÒNG LỆNH nên người dùng KHÔNG phải chọn nguồn
//   lần thứ hai.
//
// LUỒNG ĐẦY ĐỦ
//   Instance thường: MainMenu → chọn nguồn → RelaunchElevatedShare() → thoát
//   Instance admin:  main() → ParseElevatedShareArgs() → RunAgent() → MainMenu
//
// VÌ SAO CHỈ XIN QUYỀN KHI CẦN
//   Chia sẻ chỉ-xem không cần admin. Bắt nâng quyền ngay từ đầu vừa phiền vừa tạo
//   thói quen bấm Yes ở UAC mà không đọc. Chỉ xin đúng lúc tính năng thật sự đòi.
//
// LIÊN QUAN: main.cpp (đường vào của instance admin), ui/MainMenuWindow.cpp (nơi
//            gọi relaunch), input/InputInjector.h, docs/07-phase4-input.md
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <span>
#include <string>
#include <vector>

#include "AgentLoop.h"

// True khi tiến trình hiện tại đang chạy elevated (token có elevation).
bool IsProcessElevated();

// Bung UAC rồi khởi động lại exe này ở chế độ admin, mang theo `sources`+`opt`.
// True = instance mới đã chạy (instance gọi nên thoát). False = chưa elevate được;
// `outCancelled` phân biệt người dùng bấm No ở UAC với lỗi thật sự.
bool RelaunchElevatedShare(std::span<const AgentSource> sources,
                           const AgentOptions& opt, bool& outCancelled);

// Đọc phiên share do instance không-admin bàn giao qua dòng lệnh.
// False khi dòng lệnh không phải dạng bàn giao (chạy bình thường -> mở main menu).
bool ParseElevatedShareArgs(int adeskhub, wchar_t** argv,
                            std::vector<AgentSource>& outSources, AgentOptions& outOpt);
