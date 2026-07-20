#pragma once
//
// InputInjector (GD4, phia HOST) - nhan rgc::InputEvent tu client va bom vao
// may nay bang SendInput.
//
// Anh xa toa do: client gui toa do CHUAN HOA (0..65535) trong khung hinh no
// nhin thay = vung client cua cua so dang chia se. Host quy doi nguoc ve pixel
// trong client rect cua cua so dich, roi doi sang toa do man hinh ao cho
// SendInput. Nho vay client thu nho cua so preview van tro dung cho.
//
// Ban phim bom bang SCANCODE (KEYEVENTF_SCANCODE) chu khong bang ma phim ao:
// game dung DirectInput/Raw Input doc thang scancode, gui vk khong thoi thi
// game khong thay gi (day la ly do phan lon cong cu remote khong dieu khien
// duoc game).
//
// Chong KET PHIM: injector nho moi phim/nut dang giu. Client mat ket noi giua
// luc dang giu W (chay toi) ma khong nhan duoc su kien nha -> nhan vat chay mai.
// ReleaseAll() nha het, HostSession goi khi BYE/timeout.
//
// Apply() duoc goi tu luong Recv cua AgentLoop.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <map>
#include <set>

#include "rgc/Wire.h"

class InputInjector {
public:
    // `target` = cua so dang chia se (goc toa do). Dua len foreground de input
    // toi dung no. Tra false neu target khong hop le.
    bool Init(HWND target);

    // Bom mot event. Bo qua neu dang tat hoac cua so dich da dong.
    void Apply(const rgc::InputEvent& e);

    // Nha moi phim/nut con dang giu (mat ket noi, ket thuc phien, tat input).
    void ReleaseAll();

    void SetEnabled(bool on);
    bool enabled() const { return enabled_; }

    uint64_t applied() const { return applied_; }
    uint64_t skipped() const { return skipped_; }

    // Dev (--injecttest): go mot chuoi ASCII vao cua so `target` bang chinh
    // duong bom that (scancode). De kiem chung rieng nua "bom input" ma khong
    // can dung mang - xem docs/07-phase4-input.md §6.
    static int SelfTest(HWND target, const char* text);

private:
    // SendInput bom vao CUA SO DANG FOREGROUND, khong phai vao mot HWND cu the.
    // Neu nguoi o may host bam sang ung dung khac, moi phim/chuot tu client se
    // roi vao ung dung do - vua sai vua nguy hiem (nguoi dieu khien tu xa go vao
    // trinh duyet, terminal... cua chu may). Nen: chi bom khi cua so dang chia se
    // that su dang foreground; khong thi bo qua va nha het phim dang giu.
    bool TargetHasFocus();

    void SendKey(int32_t vk, int32_t scan, bool down);
    void SendButton(rgc::MouseButton btn, bool down);
    void SendMoveAbsolute(int32_t nx, int32_t ny);
    void SendMoveRelative(int32_t dx, int32_t dy);

    HWND target_ = nullptr;
    bool enabled_ = true;
    bool hadFocus_ = true;   // de chi log mot lan moi khi doi trang thai focus
    uint64_t applied_ = 0;
    uint64_t skipped_ = 0;   // event bi bo vi cua so dich khong con foreground
    std::map<int32_t, int32_t> keysDown_;     // scancode (kem bit E0) -> ma phim ao
    std::set<rgc::MouseButton> buttonsDown_;
};
