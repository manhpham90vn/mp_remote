// =============================================================================
// ConnectView.swift — ô nhập địa chỉ + nút Connect. Đối ứng MainActivity (Android).
// =============================================================================
import SwiftUI

struct ConnectView: View {
    @Bindable var model: SessionModel

    var body: some View {
        VStack(spacing: 24) {
            Spacer()

            Image(systemName: "desktopcomputer")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)

            Text("Deskhub")
                .font(.largeTitle.bold())

            VStack(spacing: 12) {
                // numbersAndPunctuation chứ KHÔNG phải decimalPad: decimalPad hiện dấu
                // thập phân theo locale (máy tiếng Việt ra "," — không gõ nổi IP) và
                // không có phím Return nên submitLabel(.go)/onSubmit không hoạt động.
                TextField("Host IP address", text: $model.address)
                    .textFieldStyle(.roundedBorder)
                    .keyboardType(.numbersAndPunctuation)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                    .submitLabel(.go)
                    .onSubmit { model.connect() }

                Button {
                    model.connect()
                } label: {
                    if model.isConnecting {
                        ProgressView()
                            .frame(maxWidth: .infinity)
                    } else {
                        Text("Connect")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.address.isEmpty || model.isConnecting)
            }
            .padding(.horizontal, 40)

            Spacer()
            Spacer()
        }
    }
}
