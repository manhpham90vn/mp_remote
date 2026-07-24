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
//   inputAccessoryView: thanh dính trên bàn phím với một nút Done để hạ bàn phím
//   tại chỗ — bàn phím ảo iPhone không có phím tự đóng, khỏi với lên nút Keys.
//
// LIÊN QUAN: StreamView.swift (toggle bàn phím trên header), SessionModel.charTap
// =============================================================================
import SwiftUI
import UIKit

struct KeyInputView: UIViewRepresentable {
    let model: SessionModel
    @Binding var active: Bool

    func makeUIView(context _: Context) -> KeyCaptureUIView {
        let view = KeyCaptureUIView()
        view.model = model
        return view
    }

    func updateUIView(_ uiView: KeyCaptureUIView, context _: Context) {
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

    // UIView trả nil mặc định — override để iOS gắn thanh Done lên trên bàn phím.
    override var inputAccessoryView: UIView? { accessoryBar }

    private lazy var accessoryBar: UIView = {
        // UIView thường, nền trong suốt — nút Done nổi một mình góc phải, không cần
        // dải nền. KHÔNG dùng UIInputView: nó tự quyết chiều cao (mỗi đời iOS một
        // kiểu) làm nút trôi khỏi vị trí. Thanh cao 48pt nhưng nút ghim mép TRÊN:
        // bàn phím iOS 26 trên máy thật có viền kính trong suốt phía trên đè lên
        // đáy accessory view — chừa 16pt đáy để nút không chìm xuống mép bàn phím
        // (nền trong suốt nên khoảng chừa này vô hình trên máy không bị đè).
        let bar = UIView(frame: CGRect(x: 0, y: 0, width: 0, height: 48))
        var config = UIButton.Configuration.gray()
        config.title = "Done"
        config.buttonSize = .mini
        let done = UIButton(
            configuration: config,
            primaryAction: UIAction { [weak self] _ in
                guard let self else { return }
                // Báo trước cho StreamView tắt trạng thái nút Keys: sau khi resign,
                // keyboardWillHide đến lúc isFirstResponder đã false nên không tự báo.
                onKeyboardDismissed?()
                resignFirstResponder()
            }
        )
        done.translatesAutoresizingMaskIntoConstraints = false
        bar.addSubview(done)
        NSLayoutConstraint.activate([
            done.trailingAnchor.constraint(equalTo: bar.trailingAnchor, constant: -8),
            done.topAnchor.constraint(equalTo: bar.topAnchor, constant: 4),
        ])
        return bar
    }()

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
            name: UIResponder.keyboardWillHideNotification, object: nil
        )
    }

    @available(*, unavailable)
    required init?(coder _: NSCoder) { fatalError("init(coder:) is not supported") }

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
