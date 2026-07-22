#pragma once
// =============================================================================
// SessionRow.h — một dòng trong danh sách nguồn của cửa sổ quản lý phiên.
//
// Dùng chung cho HAI cửa sổ đối xứng nhau:
//   ui/SessionWindow.h — phía host: các nguồn đang ĐƯỢC chia sẻ.
//   ui/ViewerWindow.h  — phía client: các nguồn đang XEM.
// Cả hai đều nhận danh sách qua SetRows(vector<SessionSourceRow>) từ vòng lặp
// chính của vai tương ứng (AgentLoop / ClientLoop).
// =============================================================================
#include <cstdint>
#include <string>

struct SessionSourceRow {
    uint8_t sourceId = 0;
    std::wstring label;   // "tên (WxH, ...)" / "tên (starting...)"
    bool pending = false; // true = đang chờ (frame đầu / đàm phán), chưa chạy hẳn

    bool operator==(const SessionSourceRow& o) const {
        return sourceId == o.sourceId && pending == o.pending && label == o.label;
    }
};
