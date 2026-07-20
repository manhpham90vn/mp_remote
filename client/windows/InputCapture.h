#pragma once
//
// InputCapture (GD4, phia CLIENT) - bat phim/chuot tren cua so preview roi
// bien thanh rgc::InputEvent de gui di.
//
// Hai che do chuot:
//   TUYET DOI (mac dinh): WM_MOUSEMOVE -> toa do client chuan hoa 0..65535.
//       Con tro chuot cua nguoi dung va con tro tren may host trung nhau -
//       dung cho ung dung cua so, menu, game chien thuat.
//   TUONG DOI (F9 bat/tat): delta tho tu Raw Input, con tro bi khoa + an.
//       Bat buoc cho game FPS: game doc chuot tho va tu keo con tro ve giua,
//       neu gui toa do tuyet doi thi camera se giat lien tuc.
//
// Ban phim luon lay tu Raw Input de co SCANCODE. Game DirectInput/raw doc
// scancode chu khong doc ma phim ao -> gui vk khong thoi la game khong nhan.
//
// Toan bo chay tren LUONG MESSAGE (main). Sink duoc goi ngay trong WndProc nen
// phai nhe: chi day vao hang doi, khong gui socket o day.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <functional>

#include "rgc/Wire.h"

class InputCapture {
public:
    using Sink = std::function<void(const rgc::InputEvent&)>;

    // Dang ky Raw Input cho cua so preview. Goi tren luong se bom message.
    bool Attach(HWND hwnd, Sink sink);
    void Detach();

    // Goi tu WndProc TRUOC khi Renderer xu ly. true = da tieu thu message.
    bool OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Tam dung/tiep tuc gui input (vd. chua STREAMING, hoac nguoi dung tat).
    void SetEnabled(bool on);
    bool enabled() const { return enabled_; }
    bool relativeMode() const { return relative_; }

    // GD5: cung duong voi phim tat F9/F10, de nut bam tren overlay preview goi
    // truc tiep (khong lap lai logic).
    void ToggleRelativeMode(); // == F9
    void TogglePause();        // == F10

private:
    void SetRelativeMode(bool on);
    void Emit(rgc::InputType type, int32_t a, int32_t b, uint8_t state, uint8_t absolute);
    void OnRawInput(LPARAM lp);
    void EmitButton(rgc::MouseButton btn, bool down);

    HWND hwnd_ = nullptr;
    Sink sink_;
    bool enabled_  = false;
    bool relative_ = false;
    bool attached_ = false;
    int  buttonsDown_ = 0; // dem nut dang giu -> biet khi nao nha SetCapture
};
