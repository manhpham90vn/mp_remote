// =============================================================================
// StreamView.swift — màn hình xem + điều khiển: header bên trên, video bên dưới.
//                    Đối ứng StreamActivity (Android).
//
// Video sống trong AVSampleBufferDisplayLayer (qua VideoLayerView); SwiftUI chỉ vẽ
// phần chrome. Điều khiển host:
//   - Trackpad ảo trên khung video: con trỏ luôn hiện, rê ngón di chuột theo delta,
//     tap 1 = click trái, tap 2 = click phải, giữ rồi kéo = drag (TouchInputView).
//   - Thanh đáy: phím tắt Esc/Tab/Enter/F9 (kHotkeys — bàn phím ảo không có những
//     phím này), nút Keys bật bàn phím ảo (KeyInputView), nút Disconnect.
// Cập nhật mỗi 500ms từ SessionModel. Màn hình xoay theo hướng thiết bị.
// =============================================================================
import AVFoundation
import SwiftUI
import UIKit

/// Một phím tắt gửi thẳng sang host — bàn phím ảo không có những phím này.
/// Thêm phím mới = thêm một dòng: mã phím ảo Windows + scancode US (bit8 = cờ E0).
private struct Hotkey {
    let label: String
    let vk: Int32
    let scan: Int32
}

// Esc/Tab/Enter đủ cho thoát menu, chuyển ô, xác nhận; F9 cho phím tắt game.
private let kHotkeys: [Hotkey] = [
    Hotkey(label: "Esc", vk: 0x1B, scan: 0x01),
    Hotkey(label: "Tab", vk: 0x09, scan: 0x0F),
    Hotkey(label: "Enter", vk: 0x0D, scan: 0x1C),
    Hotkey(label: "F9", vk: 0x78, scan: 0x43),
]

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

                bottomBar
            }
        }
        .background(Color.black.ignoresSafeArea())
        // Không đẩy/nén layout khi bàn phím ảo mở — bàn phím đè lên video, khung
        // hình đứng yên (người dùng chọn vậy).
        .ignoresSafeArea(.keyboard)
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

    // Header cố định bên trên: chỉ dòng trạng thái (chế độ dọc cho 2 dòng để hiện
    // hết thông số); mọi nút nằm ở bottomBar.
    private func header(portrait: Bool) -> some View {
        HStack {
            Text(model.statusLine.isEmpty ? "Connecting…" : model.statusLine)
                .font(.caption.monospaced())
                .foregroundStyle(.white.opacity(0.8))
                .lineLimit(portrait ? 2 : 1)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, portrait ? 8 : 6)
        .background(Color(white: 0.09))
    }

    // Thanh nút dưới đáy — mọi chiều màn hình, cuộn ngang được khi chật.
    private var bottomBar: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 12) {
                controlButtons
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
        }
        .frame(maxWidth: .infinity)
        .background(Color(white: 0.09))
    }

    // Cụm nút của thanh đáy: phím tắt kHotkeys (gõ thẳng phím sang host — bàn phím
    // ảo không có những phím này), Keys (bật/tắt bàn phím ảo) và Disconnect. Phím
    // tắt và Keys chỉ bấm được khi đang STREAMING vì trước đó kênh input chưa có.
    @ViewBuilder
    private var controlButtons: some View {
        ForEach(kHotkeys, id: \.label) { hotkey in
            Button(hotkey.label) { model.keyTap(vk: hotkey.vk, scan: hotkey.scan) }
                .font(.caption.bold())
                .buttonStyle(.bordered)
                .controlSize(.small)
                .tint(.white)
                .disabled(model.phase != .streaming)
        }

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
