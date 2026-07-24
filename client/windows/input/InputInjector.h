#pragma once
// =============================================================================
// InputInjector.h — bơm input nhận được vào máy này, phía HOST (GĐ4).
//
// NHIỆM VỤ
//   Đối tác của InputCapture: nhận deskhub::InputEvent đã khử trùng và biến nó thành
//   thao tác thật trên máy host bằng SendInput.
//
// VỊ TRÍ TRONG LUỒNG DỮ LIỆU
//   InputCapture (client) → UDP ~~~> InputReceiver → **InputInjector** → SendInput
//
// ÁNH XẠ TOẠ ĐỘ
//   Client gửi toạ độ CHUẨN HOÁ (0..65535) trong khung hình nó nhìn thấy — đúng
//   vùng WGC capture: TOÀN BỘ khung cửa sổ kể cả thanh tiêu đề (extended frame
//   bounds của DWM), không phải chỉ client rect. Host quy đổi ngược về pixel trong
//   đúng vùng đó, rồi đổi tiếp sang toạ độ màn hình ảo cho SendInput. Nhờ đi qua
//   thang chuẩn hoá này mà client thu nhỏ cửa sổ preview bao nhiêu tuỳ ý vẫn trỏ
//   đúng chỗ — và bấm được cả nút trên thanh tiêu đề của cửa sổ được share.
//
// BƠM BẰNG SCANCODE, KHÔNG PHẢI MÃ PHÍM ẢO
//   KEYEVENTF_SCANCODE. Game dùng DirectInput/Raw Input đọc thẳng scancode; gửi vk
//   không thôi thì game không thấy gì. Đây là lý do phần lớn công cụ điều khiển từ
//   xa không chơi được game — và là điểm đối xứng với InputCapture ở đầu kia.
//
// ⚠ BA CƠ CHẾ AN TOÀN / ƯU TIÊN
//   1. CHỐT FOREGROUND (TargetHasFocus). SendInput bơm vào cửa sổ ĐANG FOREGROUND
//      chứ không vào một HWND cụ thể. Nếu người ngồi ở máy host bấm sang ứng dụng
//      khác, mọi phím/chuột từ xa sẽ rơi vào ứng dụng đó — vừa sai vừa NGUY HIỂM
//      (người điều khiển từ xa vô tình gõ vào trình duyệt, terminal của chủ máy).
//      Nên: chỉ bơm khi cửa sổ đang chia sẻ thật sự đang foreground.
//   2. CHỐNG KẸT PHÍM (ReleaseAll). Injector nhớ mọi phím/nút đang giữ. Client mất
//      kết nối giữa lúc đang giữ W thì sự kiện nhả không bao giờ tới, và nhân vật
//      chạy mãi. HostSession gọi ReleaseAll khi BYE/timeout.
//   3. HOST THẮNG (LocalInputMonitor). Hai bên cùng thao tác thì input trộn thẳng
//      vào nhau: giằng con trỏ, phím bổ trợ lây chéo (host giữ Ctrl thật + remote
//      gõ S = Ctrl+S). Người ngồi tại máy vừa động chuột/phím THẬT là input từ xa
//      nhường trong ~1s — xem input/LocalInputMonitor.h và Apply().
//
// MÔ HÌNH LUỒNG
//   Apply() được gọi từ luồng Recv của AgentLoop.
//
// LIÊN QUAN: input/InputCapture.h (đầu kia), deskhub/input/InputReceiver.h,
//            deskhub/session/HostSession.h (nơi gọi ReleaseAll), docs/07-phase4-input.md
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <map>
#include <set>

#include "deskhub/wire/Wire.h"

class InputInjector {
public:
    // `target` = cửa sổ đang chia sẻ (gốc tọa độ). Đưa lên foreground để input
    // tới đúng nó. Trả false nếu target không hợp lệ.
    bool Init(HWND target);

    // Nguồn là CẢ MÀN HÌNH: gốc tọa độ là rect của monitor, và không có chốt
    // foreground — chốt đó tồn tại để input không rơi sang ứng dụng KHÁC không được
    // chia sẻ, mà ở đây cả màn hình đã được chia sẻ nên không có "ứng dụng khác".
    bool InitMonitor(HMONITOR monitor);

    // Kéo cửa sổ đích lên foreground (client vừa chuyển sang xem nguồn này —
    // SET_FOCUS). Không làm gì và trả true nếu nguồn là cả màn hình, hoặc nếu cửa
    // sổ vốn đã foreground. Trả false khi Windows từ chối.
    bool FocusTarget();

    // Bơm một event. Bỏ qua nếu đang tắt hoặc cửa sổ đích đã đóng.
    void Apply(const deskhub::InputEvent& e);

    // Nhả mọi phím/nút còn đang giữ (mất kết nối, kết thúc phiên, tắt input).
    void ReleaseAll();

    void SetEnabled(bool on);
    bool enabled() const {
        return enabled_;
    }

    uint64_t applied() const {
        return applied_;
    }
    uint64_t skipped() const {
        return skipped_;
    }

    // Dev (--injecttest): gõ một chuỗi ASCII vào cửa sổ `target` bằng chính
    // đường bơm thật (scancode). Để kiểm chứng riêng nửa "bơm input" mà không
    // cần dùng mạng - xem docs/07-phase4-input.md §6.
    static int SelfTest(HWND target, const char* text);

private:
    // SendInput bơm vào CỬA SỔ ĐANG FOREGROUND, không phải vào một HWND cụ thể.
    // Nếu người ở máy host bấm sang ứng dụng khác, mọi phím/chuột từ client sẽ
    // rơi vào ứng dụng đó - vừa sai vừa nguy hiểm (người điều khiển từ xa gõ vào
    // trình duyệt, terminal... của chủ máy). Nên: chỉ bơm khi cửa sổ đang chia sẻ
    // thật sự đang foreground; không thì bỏ qua và nhả hết phím đang giữ.
    bool TargetHasFocus();

    void SendKey(int32_t vk, int32_t scan, bool down);
    void SendButton(deskhub::MouseButton btn, bool down);
    void SendMoveAbsolute(int32_t nx, int32_t ny);
    void SendMoveRelative(int32_t dx, int32_t dy);

    HWND target_ = nullptr; // đúng một trong hai khác nullptr
    HMONITOR monitor_ = nullptr;
    bool enabled_ = true;
    bool hadFocus_ = true;         // để chỉ log một lần mỗi khi đổi trạng thái focus
    bool localSuppressed_ = false; // đang nhường "host thắng" — log một lần mỗi lượt
    uint64_t applied_ = 0;
    uint64_t skipped_ = 0;                // event bị bỏ vì cửa sổ đích không còn foreground
    std::map<int32_t, int32_t> keysDown_; // scancode (kèm bit E0) -> mã phím ảo
    std::set<deskhub::MouseButton> buttonsDown_;
};
