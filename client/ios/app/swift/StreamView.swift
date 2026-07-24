// =============================================================================
// StreamView.swift — hiển thị video + overlay số liệu. Đối ứng StreamActivity (Android).
//
// Video sống trong AVSampleBufferDisplayLayer (qua VideoLayerView); SwiftUI chỉ vẽ
// phần chrome (overlay text, nút disconnect). Cập nhật mỗi 500ms từ SessionModel.
// =============================================================================
import AVFoundation
import SwiftUI
import UIKit

struct StreamView: View {
    @Bindable var model: SessionModel
    @Environment(\.scenePhase) private var scenePhase
    @State private var layer: AVSampleBufferDisplayLayer?

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            videoContent
                .aspectRatio(aspectRatio, contentMode: .fit)

            overlay
        }
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
            AppDelegate.orientationLock = .landscape
            enforceOrientation()
            model.streamViewAppeared()
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
            AppDelegate.orientationLock = .all
            enforceOrientation()
            releaseLayer()
            model.streamViewDisappeared()
        }
        .statusBarHidden()
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

    @ViewBuilder
    private var overlay: some View {
        VStack {
            HStack {
                Spacer()
                Button {
                    model.disconnect()
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.title2)
                        .foregroundStyle(.white.opacity(0.7))
                }
                .padding()
            }
            Spacer()
            if !model.statusLine.isEmpty {
                Text(model.statusLine)
                    .font(.caption.monospaced())
                    .foregroundStyle(.white.opacity(0.8))
                    .padding(6)
                    .background(.black.opacity(0.5), in: RoundedRectangle(cornerRadius: 4))
                    .padding(.bottom, 8)
            }
            if !model.endReason.isEmpty {
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
        }
    }

    private func releaseLayer() {
        DeskhubClient.setLayer(nil)
    }

    private func enforceOrientation() {
        guard let scene = UIApplication.shared.connectedScenes.first as? UIWindowScene else { return }
        let geometryPreferences = UIWindowScene.GeometryPreferences.iOS(
            interfaceOrientations: AppDelegate.orientationLock
        )
        scene.requestGeometryUpdate(geometryPreferences) { _ in }
    }
}
