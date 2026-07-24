// =============================================================================
// App.swift — entry point. Không khóa hướng màn hình: app xoay theo hướng thiết
// bị trong phạm vi các hướng khai báo ở Info.plist (portrait + landscape).
// =============================================================================
import SwiftUI

@main
struct DeskhubApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
