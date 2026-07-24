// =============================================================================
// ContentView.swift — root view, điều hướng giữa ba màn theo trạng thái model.
// =============================================================================
import SwiftUI

struct ContentView: View {
    @State private var model = SessionModel()

    var body: some View {
        switch model.screen {
        case .connect:
            ConnectView(model: model)
        case .sourcePicker(let sources):
            SourcePickerView(sources: sources, model: model)
        case .stream:
            StreamView(model: model)
        }
    }
}
