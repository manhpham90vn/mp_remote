// =============================================================================
// VideoLayerView.swift — UIViewRepresentable bọc AVSampleBufferDisplayLayer.
//
// Đây là nơi duy nhất chạm UIKit: một UIView có layerClass là
// AVSampleBufferDisplayLayer. SwiftUI không render video — layer nhận frame thẳng
// từ VtDecoder qua hardware compositor. Đối ứng SurfaceView trong StreamActivity
// bên Android.
// =============================================================================
import AVFoundation
import SwiftUI
import UIKit

final class VideoDisplayView: UIView {
    override static var layerClass: AnyClass { AVSampleBufferDisplayLayer.self }

    var displayLayer: AVSampleBufferDisplayLayer {
        // swiftlint:disable:next force_cast
        layer as! AVSampleBufferDisplayLayer
    }
}

struct VideoLayerView: UIViewRepresentable {
    let onLayerReady: @MainActor (AVSampleBufferDisplayLayer) -> Void

    func makeUIView(context: Context) -> VideoDisplayView {
        let view = VideoDisplayView()
        view.displayLayer.videoGravity = .resizeAspect
        onLayerReady(view.displayLayer)
        return view
    }

    func updateUIView(_ uiView: VideoDisplayView, context: Context) {}
}
