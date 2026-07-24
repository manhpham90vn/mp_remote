// =============================================================================
// TouchInputView.swift — trackpad ảo phủ lên khung video, kiểu bàn di chuột laptop.
//                        Đối ứng TrackpadOverlay bên Android.
//
// VÌ SAO TRACKPAD CHỨ KHÔNG PHẢI CHẠM-TRỰC-TIẾP
//   Chạm thẳng vào điểm muốn click nghe hợp lý nhưng khó dùng thật: ngón tay che
//   mất chỗ cần bấm và không bấm chính xác được mục tiêu nhỏ (nút đóng cửa sổ,
//   menu). Trackpad tách ngón tay khỏi con trỏ: con trỏ LUÔN hiện, ngón rê ở đâu
//   cũng được — con trỏ dịch theo DELTA.
//
// CỬ CHỈ -> CHUỘT
//   Rê ngón       = di con trỏ.
//   Tap 1 lần     = click trái TẠI CON TRỎ (chờ hết cửa sổ double-tap mới nổ).
//   Tap 2 lần     = click phải tại con trỏ.
//   Giữ rồi kéo   = giữ chuột trái và rê (kéo cửa sổ, bôi đen), nhấc tay là nhả.
//   Các recognizer mặc định loại trừ lẫn nhau: pan cần chuyển động trước, long
//   press cần đứng yên trước — không tranh chấp.
//
// TOẠ ĐỘ
//   Con trỏ tính bằng pt trong bounds của view; view được đặt cùng aspectRatio với
//   khung video nên bounds trùng khít khung hình host capture. Gửi sang host dạng
//   chuẩn hoá 0..65535 — hệ mà InputInjector bên host mong đợi.
//
// LIÊN QUAN: StreamView.swift (nơi đặt overlay), SessionModel (chuyển tiếp xuống C++)
// =============================================================================
import SwiftUI
import UIKit

struct TouchInputView: UIViewRepresentable {
    let model: SessionModel

    func makeUIView(context: Context) -> TouchCaptureUIView {
        let view = TouchCaptureUIView()
        view.model = model
        return view
    }

    func updateUIView(_ uiView: TouchCaptureUIView, context: Context) {
        uiView.model = model
    }
}

final class TouchCaptureUIView: UIView {
    weak var model: SessionModel?

    // Mũi tên con trỏ: SF Symbol trắng + bóng đen để nổi trên mọi nền video.
    private let cursorView: UIImageView = {
        let view = UIImageView(image: UIImage(systemName: "cursorarrow"))
        view.tintColor = .white
        view.layer.shadowColor = UIColor.black.cgColor
        view.layer.shadowOpacity = 0.9
        view.layer.shadowOffset = .zero
        view.layer.shadowRadius = 1.5
        view.frame = CGRect(x: 0, y: 0, width: 18, height: 20)
        return view
    }()

    private var cursor: CGPoint = .zero
    private var cursorPlaced = false
    private var lastDragLocation: CGPoint = .zero

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isMultipleTouchEnabled = false
        addSubview(cursorView)

        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(handleDoubleTap))
        doubleTap.numberOfTapsRequired = 2
        let singleTap = UITapGestureRecognizer(target: self, action: #selector(handleSingleTap))
        // Tap 1 phải chờ chắc chắn không phải tap 2 — giá của việc phân biệt hai cử chỉ.
        singleTap.require(toFail: doubleTap)
        let pan = UIPanGestureRecognizer(target: self, action: #selector(handlePan(_:)))
        pan.maximumNumberOfTouches = 1
        let longPress = UILongPressGestureRecognizer(
            target: self, action: #selector(handleLongPress(_:)))

        addGestureRecognizer(doubleTap)
        addGestureRecognizer(singleTap)
        addGestureRecognizer(pan)
        addGestureRecognizer(longPress)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    // Lần đầu biết kích thước: đặt con trỏ giữa khung (chỉ hiển thị, KHÔNG gửi —
    // đừng tự di chuột host khi vừa kết nối). Khung đổi kích thước (xoay máy):
    // kẹp con trỏ lại trong khung mới.
    override func layoutSubviews() {
        super.layoutSubviews()
        guard bounds.width > 0, bounds.height > 0 else { return }
        if !cursorPlaced {
            cursorPlaced = true
            cursor = CGPoint(x: bounds.midX, y: bounds.midY)
        } else {
            cursor = clamped(cursor)
        }
        cursorView.frame.origin = cursor
    }

    private func clamped(_ p: CGPoint) -> CGPoint {
        CGPoint(
            x: min(max(0, p.x), bounds.width),
            y: min(max(0, p.y), bounds.height))
    }

    // Đỉnh mũi tên của "cursorarrow" nằm ở góc trên-trái icon -> origin đặt đúng
    // vị trí con trỏ.
    private func moveCursor(by delta: CGPoint) {
        cursor = clamped(CGPoint(x: cursor.x + delta.x, y: cursor.y + delta.y))
        cursorView.frame.origin = cursor
        sendMove()
    }

    private func sendMove() {
        guard bounds.width > 0, bounds.height > 0 else { return }
        model?.mouseMove(
            nx: Int32((cursor.x / bounds.width * 65535).rounded()),
            ny: Int32((cursor.y / bounds.height * 65535).rounded()))
    }

    // Host cũng có người dùng thật di chuột được — gửi lại vị trí con trỏ ngay
    // trước mỗi cú click để click rơi đúng chỗ con trỏ đang hiển thị.
    private func click(_ button: MouseButton) {
        sendMove()
        model?.mouseButton(button, down: true)
        model?.mouseButton(button, down: false)
    }

    @objc private func handleSingleTap() { click(.left) }

    @objc private func handleDoubleTap() { click(.right) }

    @objc private func handlePan(_ gesture: UIPanGestureRecognizer) {
        let location = gesture.location(in: self)
        switch gesture.state {
        case .began:
            lastDragLocation = location
        case .changed:
            moveCursor(by: CGPoint(
                x: location.x - lastDragLocation.x,
                y: location.y - lastDragLocation.y))
            lastDragLocation = location
        default:
            break
        }
    }

    @objc private func handleLongPress(_ gesture: UILongPressGestureRecognizer) {
        let location = gesture.location(in: self)
        switch gesture.state {
        case .began:
            lastDragLocation = location
            sendMove()
            model?.mouseButton(.left, down: true)
        case .changed:
            moveCursor(by: CGPoint(
                x: location.x - lastDragLocation.x,
                y: location.y - lastDragLocation.y))
            lastDragLocation = location
        case .ended, .cancelled, .failed:
            model?.mouseButton(.left, down: false)
        default:
            break
        }
    }
}
