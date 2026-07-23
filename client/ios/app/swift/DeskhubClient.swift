// =============================================================================
// DeskhubClient.swift — điểm gọi xuống C++ duy nhất phía Swift.
//                        Đối ứng NativeClient.kt bên Android.
//
// KHÔNG View nào gọi trực tiếp hàm C — mọi lối đi qua đây. Tương lai nếu cần
// mock cho test thì chỉ cần mock lớp này.
// =============================================================================
import AVFoundation

nonisolated enum Phase: Int, Sendable {
    case idle = 0
    case connecting = 1
    case streaming = 2
    case ended = 3
}

struct Source: Identifiable, Sendable {
    let id: UInt8
    let width: UInt16
    let height: UInt16
    let name: String
}

nonisolated enum DeskhubClient {
    // CHẶN ~3s — gọi ngoài main thread (Task.detached / nonisolated).
    static func listSources(address: String) -> [Source] {
        var buf = [DHSourceInfo](repeating: DHSourceInfo(), count: 16)
        let count = buf.withUnsafeMutableBufferPointer { ptr in
            dh_list_sources(address, ptr.baseAddress, Int32(ptr.count))
        }
        guard count > 0 else { return [] }
        return (0..<Int(count)).map { i in
            let info = buf[i]
            let name = withUnsafeBytes(of: info.name) { rawBuf in
                let ptr = rawBuf.baseAddress!.assumingMemoryBound(to: CChar.self)
                return String(cString: ptr)
            }
            return Source(id: info.sourceId, width: info.width, height: info.height, name: name)
        }
    }

    @discardableResult
    static func start(address: String, sourceId: UInt8) -> Bool {
        dh_start(address, sourceId)
    }

    static func stop() {
        dh_stop()
    }

    static func setLayer(_ layer: AVSampleBufferDisplayLayer?) {
        let ptr = layer.map { Unmanaged.passUnretained($0).toOpaque() }
        dh_set_layer(ptr)
    }

    static func phase() -> Phase {
        Phase(rawValue: Int(dh_phase().rawValue)) ?? .idle
    }

    static func statusLine() -> String {
        String(cString: dh_status_line())
    }

    static func endReason() -> String {
        String(cString: dh_end_reason())
    }

    static func videoWidth() -> UInt32 {
        dh_video_width()
    }

    static func videoHeight() -> UInt32 {
        dh_video_height()
    }
}
