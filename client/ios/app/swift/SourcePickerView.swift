// =============================================================================
// SourcePickerView.swift — chọn cửa sổ muốn xem. Đối ứng danh sách nguồn trên
//                           MainActivity bên Android.
// =============================================================================
import SwiftUI

struct SourcePickerView: View {
    let sources: [Source]
    @Bindable var model: SessionModel

    var body: some View {
        NavigationStack {
            List(sources) { source in
                Button {
                    model.pickSource(source)
                } label: {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(source.name)
                                .font(.body)
                            Text("\(source.width)×\(source.height)")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        Image(systemName: "chevron.right")
                            .foregroundStyle(.secondary)
                    }
                }
                .tint(.primary)
            }
            .navigationTitle("Select Source")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Back") { model.disconnect() }
                }
            }
        }
    }
}
