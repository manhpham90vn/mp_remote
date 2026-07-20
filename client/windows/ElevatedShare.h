#pragma once
//
// ElevatedShare - xin quyền admin cho vai trò HOST khi bật điều khiển.
//
// UIPI: một tiến trình integrity level thấp hơn không được gửi input tới cửa sổ
// của tiến trình cao hơn. Nên host chạy quyền thường sẽ capture được hình của
// game/app chạy admin nhưng SendInput bị nuốt im lặng (xem docs/07-phase4-input.md).
// Vì vậy khi người dùng bấm Share với "cho phép điều khiển", ta relaunch chính
// exe này qua ShellExecuteEx verb "runas" (bung UAC) và bàn giao nguyên phiên
// share sang instance mới bằng dòng lệnh - người dùng không phải chọn nguồn lại.
//
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
bool ParseElevatedShareArgs(int argc, wchar_t** argv,
                            std::vector<AgentSource>& outSources, AgentOptions& outOpt);
