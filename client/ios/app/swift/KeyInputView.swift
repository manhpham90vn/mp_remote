// =============================================================================
// KeyInputView.swift — view vô hình hứng phím từ bàn phím ảo cho màn hình xem.
//                      Đối ứng KeyInputView.kt bên Android.
//
// CƠ CHẾ
//   UIKeyInput là giao thức tối giản của UIKit cho "nhận phím không cần ô nhập":
//   insertText(_:) nhận từng ký tự gõ, deleteBackward() nhận backspace. hasText
//   luôn true để bàn phím không bao giờ chặn deleteBackward. Ký tự được giao cho
//   tầng C++ quy đổi sang VK (KeyMap.h, layout US) — ký tự ngoài ASCII bị bỏ qua
//   ở đó, nên tắt luôn autocorrect/gợi ý cho đỡ nhiễu.
//
// LIÊN QUAN: StreamView.swift (toggle bàn phím trên header), SessionModel.charTap
// =============================================================================
import SwiftUI
import UIKit

struct KeyInputView: UIViewRepresentable {
    let model: SessionModel
    @Binding var active: Bool

    func makeUIView(context: Context) -> KeyCaptureUIView {
        let view = KeyCaptureUIView()
        view.model = model
        return view
    }

    func updateUIView(_ uiView: KeyCaptureUIView, context: Context) {
        uiView.model = model
        // Bàn phím có thể bị hạ ngoài nút Keys (nút ẩn trên iPad, app xuống nền) —
        // báo ngược để nút Keys không kẹt trạng thái "đang bật".
        let binding = $active
        uiView.onKeyboardDismissed = { binding.wrappedValue = false }
        // Toggle bàn phím = giành/nhả first responder. Gọi lặp vô hại: có kiểm tra
        // trạng thái trước nên không giật focus mỗi lần SwiftUI cập nhật.
        if active, !uiView.isFirstResponder {
            uiView.becomeFirstResponder()
        } else if !active, uiView.isFirstResponder {
            uiView.resignFirstResponder()
        }
    }
}

final class KeyCaptureUIView: UIView, UIKeyInput {
    weak var model: SessionModel?
    var onKeyboardDismissed: (() -> Void)?

    // UITextInputTraits: bàn phím ASCII, không autocorrect — cái cần là PHÍM,
    // không phải văn bản đã qua bộ gõ.
    var keyboardType: UIKeyboardType = .asciiCapable
    var autocorrectionType: UITextAutocorrectionType = .no
    var spellCheckingType: UITextSpellCheckingType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none

    override init(frame: CGRect) {
        super.init(frame: frame)
        NotificationCenter.default.addObserver(
            self, selector: #selector(keyboardWillHide),
            name: UIResponder.keyboardWillHideNotification, object: nil)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    // Chỉ báo khi ta CÒN là first responder — tức bàn phím hạ không phải do mình
    // resign (bấm Keys tắt thì active đã false sẵn, khỏi báo).
    @objc private func keyboardWillHide() {
        if isFirstResponder { onKeyboardDismissed?() }
    }

    override var canBecomeFirstResponder: Bool { true }

    var hasText: Bool { true }

    func insertText(_ text: String) {
        for scalar in text.unicodeScalars {
            model?.charTap(scalar.value)
        }
    }

    func deleteBackward() {
        model?.charTap(0x08) // '\b' — KeyMap bên C++ đổi thành VK_BACK
    }
}
