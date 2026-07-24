// =============================================================================
// SessionModel.swift — trạng thái phiên cho SwiftUI, đối ứng ViewModel của Android.
//
// Quản lý toàn bộ luồng người dùng: kết nối → chọn nguồn → xem. View chỉ đọc
// @Published và gọi action trên model, không chạm facade trực tiếp.
// =============================================================================
import Foundation
import Observation

enum AppScreen: Sendable {
    case connect
    case sourcePicker([Source])
    case stream
}

@MainActor @Observable
final class SessionModel {
    var screen: AppScreen = .connect
    var address: String = UserDefaults.standard.string(forKey: "lastAddress") ?? ""
    var isConnecting = false
    var phase: Phase = .idle
    var statusLine = ""
    var endReason = ""
    var videoWidth: UInt32 = 0
    var videoHeight: UInt32 = 0

    private var pollTimer: Timer?
    private var selectedSourceId: UInt8 = 0

    func connect() {
        guard !address.isEmpty else { return }
        isConnecting = true

        let addr = address
        UserDefaults.standard.set(addr, forKey: "lastAddress")

        Task.detached {
            let sources = DeskhubClient.listSources(address: addr)
            await MainActor.run { [weak self] in
                guard let self else { return }
                self.isConnecting = false
                if sources.count > 1 {
                    self.screen = .sourcePicker(sources)
                } else {
                    self.startStream(sourceId: sources.first?.id ?? 0)
                }
            }
        }
    }

    // Chọn nguồn từ danh sách.
    func pickSource(_ source: Source) {
        startStream(sourceId: source.id)
    }

    // Bắt đầu xem.
    func startStream(sourceId: UInt8) {
        selectedSourceId = sourceId
        endReason = ""
        statusLine = ""
        phase = .connecting
        DeskhubClient.start(address: address, sourceId: sourceId)
        screen = .stream
        startPolling()
    }

    // Dừng phiên và quay về màn hình kết nối.
    func disconnect() {
        stopPolling()
        DeskhubClient.stop()
        phase = .idle
        screen = .connect
    }

    // --- Điều khiển từ touch/bàn phím ảo (StreamView + TouchInputView/KeyInputView).
    // Chỉ chuyển tiếp xuống facade; tầng C++ tự bỏ qua khi chưa STREAMING. ---

    // Gõ một phím tắt rời (Esc/Tab/Enter/mũi tên... — thanh phím tắt của StreamView).
    func keyTap(vk: Int32, scan: Int32) {
        DeskhubClient.keyTap(vk: vk, scan: scan)
    }

    // Tổ hợp kiểu Ctrl+C từ thanh phím tắt.
    func keyChord(modVk: Int32, modScan: Int32, vk: Int32, scan: Int32) {
        DeskhubClient.keyChord(modVk: modVk, modScan: modScan, vk: vk, scan: scan)
    }

    func mouseMove(nx: Int32, ny: Int32) {
        DeskhubClient.mouseMove(nx: nx, ny: ny)
    }

    func mouseButton(_ button: MouseButton, down: Bool) {
        DeskhubClient.mouseButton(button, down: down)
    }

    func charTap(_ codepoint: UInt32) {
        DeskhubClient.charTap(codepoint)
    }

    // UI cần gọi khi StreamView xuất hiện/biến mất.
    func streamViewAppeared() {
        startPolling()
    }

    func streamViewDisappeared() {
        stopPolling()
    }

    // Hỏi C++ mỗi 500ms để cập nhật overlay.
    private func startPolling() {
        stopPolling()
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.poll()
        }
        poll()
    }

    private func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    private func poll() {
        phase = DeskhubClient.phase()
        statusLine = DeskhubClient.statusLine()
        videoWidth = DeskhubClient.videoWidth()
        videoHeight = DeskhubClient.videoHeight()

        if phase == .ended {
            endReason = DeskhubClient.endReason()
            stopPolling()
        }
    }
}
