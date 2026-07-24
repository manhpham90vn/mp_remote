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

// Nút chuột theo deskhub::MouseButton (Wire.h).
nonisolated enum MouseButton: Int32, Sendable {
    case left = 1
    case right = 2
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
        return (0..<Int(count)).map { idx in
            let info = buf[idx]
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

    // Gõ một phím rời (nhấn + nhả) sang host — dành cho phím đặc biệt tương lai
    // (Esc, F-key...); phím chữ đi đường charTap. Chỉ có tác dụng khi đang STREAMING.
    static func keyTap(vk: Int32, scan: Int32) {
        dh_key_tap(vk, scan)
    }

    // Chuột tuyệt đối từ touch: toạ độ chuẩn hoá 0..65535 trong khung video.
    static func mouseMove(nx: Int32, ny: Int32) {
        dh_mouse_move(nx, ny)
    }

    static func mouseButton(_ button: MouseButton, down: Bool) {
        dh_mouse_button(button.rawValue, down)
    }

    // Gõ một ký tự từ bàn phím ảo (core quy đổi sang VK theo layout US).
    static func charTap(_ codepoint: UInt32) {
        dh_char_tap(codepoint)
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
