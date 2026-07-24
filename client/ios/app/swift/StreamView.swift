// =============================================================================
// StreamView.swift — màn hình xem + điều khiển: header bên trên, video bên dưới.
//                    Đối ứng StreamActivity (Android).
//
// Video sống trong AVSampleBufferDisplayLayer (qua VideoLayerView); SwiftUI chỉ vẽ
// phần chrome. Điều khiển host:
//   - Trackpad ảo trên khung video: con trỏ luôn hiện, rê ngón di chuột theo delta,
//     tap 1 = click trái, tap 2 = click phải, giữ rồi kéo = drag (TouchInputView).
//   - Nút Keys bật bàn phím ảo, phím gõ chạy sang host (KeyInputView).
//   - Nút Disconnect kết thúc phiên.
// Cập nhật mỗi 500ms từ SessionModel. Màn hình xoay theo hướng thiết bị.
// =============================================================================
import AVFoundation
import SwiftUI
import UIKit

struct StreamView: View {
    @Bindable var model: SessionModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var layer: AVSampleBufferDisplayLayer?
    @State private var keyboardOn = false

    var body: some View {
        // Ngang: trạng thái + cụm nút nằm gọn một hàng trên cùng. Dọc: màn hình hẹp
        // không đủ chỗ cho cả hai — header chỉ còn dòng trạng thái (cho 2 dòng để
        // hiện hết thông số), cụm nút dời xuống thanh đáy.
        GeometryReader { geo in
            let portrait = geo.size.height > geo.size.width

            VStack(spacing: 0) {
                header(portrait: portrait)

                ZStack {
                    Color.black

                    videoContent
                        .aspectRatio(aspectRatio, contentMode: .fit)

                    // Lớp phủ touch cùng aspectRatio -> bounds trùng khít khung video,
                    // toạ độ chuẩn hoá khớp hệ mà host mong đợi.
                    if model.phase == .streaming {
                        TouchInputView(model: model)
                            .aspectRatio(aspectRatio, contentMode: .fit)
                    }

                    // View hứng phím: vô hình, chỉ tồn tại để giữ first responder.
                    // allowsHitTesting(false): không được nuốt cú chạm của lớp touch.
                    KeyInputView(model: model, active: $keyboardOn)
                        .frame(width: 1, height: 1)
                        .opacity(0)
                        .allowsHitTesting(false)

                    if !model.endReason.isEmpty {
                        endedOverlay
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)

                if portrait {
                    bottomBar
                }
            }
        }
        .background(Color.black.ignoresSafeArea())
        .onChange(of: scenePhase) { _, newPhase in
            switch newPhase {
            case .background:
                releaseLayer()
            case .active:
                if let layer {
                    DeskhubClient.setLayer(layer)
                }
            case .inactive:
                break
            @unknown default:
                break
            }
        }
        .onAppear {
            UIApplication.shared.isIdleTimerDisabled = true
            model.streamViewAppeared()
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
            keyboardOn = false
            releaseLayer()
            model.streamViewDisappeared()
        }
        .statusBarHidden()
    }

    // Header cố định bên trên: trạng thái bên trái; ở chế độ ngang kèm cụm nút bên
    // phải, ở chế độ dọc nút nằm ở bottomBar.
    private func header(portrait: Bool) -> some View {
        HStack(spacing: 8) {
            Text(model.statusLine.isEmpty ? "Connecting…" : model.statusLine)
                .font(.caption.monospaced())
                .foregroundStyle(.white.opacity(0.8))
                .lineLimit(portrait ? 2 : 1)
                .frame(maxWidth: .infinity, alignment: .leading)

            if !portrait {
                controlButtons
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, portrait ? 8 : 6)
        .background(Color(white: 0.09))
    }

    // Thanh nút dưới đáy — chỉ ở chế độ dọc.
    private var bottomBar: some View {
        HStack(spacing: 24) {
            controlButtons
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 8)
        .background(Color(white: 0.09))
    }

    // Cụm nút điều khiển phiên: Keys (bật/tắt bàn phím ảo) và Disconnect. Keys chỉ
    // bấm được khi đang STREAMING vì trước đó kênh input chưa tồn tại.
    @ViewBuilder
    private var controlButtons: some View {
        Button("Keys") { keyboardOn.toggle() }
            .font(.caption.bold())
            .buttonStyle(.bordered)
            .controlSize(.small)
            .tint(keyboardOn ? .mint : .white)
            .disabled(model.phase != .streaming)

        Button("Disconnect") { model.disconnect() }
            .font(.caption.bold())
            .buttonStyle(.bordered)
            .controlSize(.small)
            .tint(.red)
    }

    private var videoContent: some View {
        VideoLayerView { newLayer in
            layer = newLayer
            DeskhubClient.setLayer(newLayer)
        }
    }

    private var aspectRatio: CGFloat {
        let width = CGFloat(model.videoWidth)
        let height = CGFloat(model.videoHeight)
        guard width > 0, height > 0 else { return 16.0 / 9.0 }
        return width / height
    }

    private var endedOverlay: some View {
        VStack(spacing: 8) {
            Text("Session ended")
                .font(.headline)
                .foregroundStyle(.white)
            Text(model.endReason)
                .font(.subheadline)
                .foregroundStyle(.white.opacity(0.7))
            Button("Disconnect") { model.disconnect() }
                .buttonStyle(.borderedProminent)
        }
        .padding()
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
    }

    private func releaseLayer() {
        DeskhubClient.setLayer(nil)
    }
}
