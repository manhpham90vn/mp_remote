#pragma once
// =============================================================================
// KeyMap.h — quy đổi ký tự gõ được thành tổ hợp phím ảo Windows, phía CLIENT.
//
// NHIỆM VỤ
//   Client mobile/web không có bàn phím vật lý gửi được VK/scancode trực tiếp —
//   thứ chúng có là KÝ TỰ từ bàn phím ảo (insertText / commitText). File này đổi
//   một ký tự ASCII thành (mã phím ảo Windows, cần Shift hay không) để client dựng
//   chuỗi event Key cho InputSender.
//
// VÌ SAO ĐẶT Ở CORE
//   Cùng một bảng dùng cho client Android, iOS (và web sau này). Bảng là dữ liệu
//   thuần, không đụng OS nào — đúng tiêu chí "thuần C++20" của core.
//
// GIỚI HẠN CÓ CHỦ Ý: LAYOUT US
//   VK của ký hiệu (OEM key) phụ thuộc layout bàn phím của HOST. Bảng này giả định
//   host dùng layout US — trường hợp phổ biến nhất. Host layout khác thì ký hiệu
//   có thể ra sai phím; chữ cái và chữ số thì luôn đúng.
//
// SCANCODE = 0
//   Không gửi scancode: InputInjector bên host lùi về wVk khi scan = 0 (xem
//   InputInjector.cpp). Đủ cho việc gõ chữ vào ứng dụng thường; game đọc raw input
//   cần scancode thật thì dùng đường phím rời (như nút F9) vốn có scan đầy đủ.
//
// LIÊN QUAN: deskhub/wire/Wire.h (InputEvent), deskhub/input/InputSender.h,
//            client/windows/input/InputInjector.cpp (đầu nhận, thứ tự ưu tiên scan/vk)
// =============================================================================
#include <cstdint>
#include <optional>

namespace deskhub {

// Vài mã phím ảo Windows mà client cần gọi tên trực tiếp.
inline constexpr int32_t kVkBack = 0x08;   // Backspace
inline constexpr int32_t kVkTab = 0x09;    // Tab
inline constexpr int32_t kVkReturn = 0x0D; // Enter
inline constexpr int32_t kVkShift = 0x10;
inline constexpr int32_t kVkSpace = 0x20;

// Một ký tự = một phím + có thể kèm Shift (layout US).
struct KeyChord {
    int32_t vk = 0;
    bool shift = false;
};

// Ký tự (codepoint) -> tổ hợp phím, hoặc nullopt nếu không gõ được bằng một phím
// trên layout US (ký tự ngoài ASCII, ký tự điều khiển lạ...). Nhận cả '\b', '\t',
// '\n', '\r' để caller khỏi phải phân loại phím điều khiển riêng.
inline std::optional<KeyChord> CharToKeyChord(uint32_t cp) {
    // Chữ cái: VK trùng mã chữ HOA; chữ hoa = kèm Shift.
    if (cp >= 'a' && cp <= 'z') return KeyChord{int32_t(cp - 'a' + 'A'), false};
    if (cp >= 'A' && cp <= 'Z') return KeyChord{int32_t(cp), true};
    // Chữ số: VK trùng mã ASCII.
    if (cp >= '0' && cp <= '9') return KeyChord{int32_t(cp), false};

    switch (cp) {
        case '\b': return KeyChord{kVkBack, false};
        case '\t': return KeyChord{kVkTab, false};
        case '\n':
        case '\r': return KeyChord{kVkReturn, false};
        case ' ': return KeyChord{kVkSpace, false};

        // Ký hiệu trên hàng số (Shift + chữ số, layout US).
        case '!': return KeyChord{'1', true};
        case '@': return KeyChord{'2', true};
        case '#': return KeyChord{'3', true};
        case '$': return KeyChord{'4', true};
        case '%': return KeyChord{'5', true};
        case '^': return KeyChord{'6', true};
        case '&': return KeyChord{'7', true};
        case '*': return KeyChord{'8', true};
        case '(': return KeyChord{'9', true};
        case ')': return KeyChord{'0', true};

        // Cụm phím OEM (mã VK_OEM_* của Windows, layout US).
        case ';': return KeyChord{0xBA, false}; // VK_OEM_1
        case ':': return KeyChord{0xBA, true};
        case '=': return KeyChord{0xBB, false}; // VK_OEM_PLUS
        case '+': return KeyChord{0xBB, true};
        case ',': return KeyChord{0xBC, false}; // VK_OEM_COMMA
        case '<': return KeyChord{0xBC, true};
        case '-': return KeyChord{0xBD, false}; // VK_OEM_MINUS
        case '_': return KeyChord{0xBD, true};
        case '.': return KeyChord{0xBE, false}; // VK_OEM_PERIOD
        case '>': return KeyChord{0xBE, true};
        case '/': return KeyChord{0xBF, false}; // VK_OEM_2
        case '?': return KeyChord{0xBF, true};
        case '`': return KeyChord{0xC0, false}; // VK_OEM_3
        case '~': return KeyChord{0xC0, true};
        case '[': return KeyChord{0xDB, false}; // VK_OEM_4
        case '{': return KeyChord{0xDB, true};
        case '\\': return KeyChord{0xDC, false}; // VK_OEM_5
        case '|': return KeyChord{0xDC, true};
        case ']': return KeyChord{0xDD, false}; // VK_OEM_6
        case '}': return KeyChord{0xDD, true};
        case '\'': return KeyChord{0xDE, false}; // VK_OEM_7
        case '"': return KeyChord{0xDE, true};
        default: return std::nullopt;
    }
}

} // namespace deskhub
