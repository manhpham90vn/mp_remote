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
        DeskhubClient.start(address: address, sourceId: sourceId)
        screen = .stream
        startPolling()
    }

    // Dừng phiên và quay về màn hình kết nối.
    func disconnect() {
        stopPolling()
        DeskhubClient.stop()
        screen = .connect
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
        let phase = DeskhubClient.phase()
        statusLine = DeskhubClient.statusLine()
        videoWidth = DeskhubClient.videoWidth()
        videoHeight = DeskhubClient.videoHeight()

        if phase == .ended {
            endReason = DeskhubClient.endReason()
            stopPolling()
        }
    }
}
