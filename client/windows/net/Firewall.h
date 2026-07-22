#pragma once
// =============================================================================
// Firewall.h — tự mở Windows Firewall cho vai HOST.
//
// VẤN ĐỀ
//   Host lắng nghe UDP trên INADDR_ANY (xem net/UdpSocket.cpp). Windows Firewall
//   mặc định CHẶN mọi gói inbound không khớp rule nào, nên client gửi HELLO tới host
//   quyền-thường sẽ không bao giờ tới nơi — phía client chỉ thấy RecvFrom timeout,
//   không có lỗi cụ thể. Đây là nguyên nhân "kết nối được trong LAN nhưng cứ timeout"
//   hay gặp nhất.
//
// GIẢI PHÁP: TỰ THÊM RULE LÚC SHARE
//   Thay vì bắt người dùng vào Control Panel, app tự tạo một rule "allow inbound UDP
//   cho chính client.exe" qua COM (INetFwPolicy2), phủ CẢ BA profile (Domain/Private/
//   Public) để không phụ thuộc Windows phân loại mạng thế nào — máy host Win10 hay để
//   Public là ca hỏng kinh điển. Thêm rule ĐÒI QUYỀN ADMIN, nên việc này gộp chung
//   vào lần bung UAC của nút Share (xem ElevatedShare.h): instance admin vừa có quyền
//   gửi input vừa thêm được rule.
//
// VÌ SAO THEO PROGRAM, KHÔNG PHẢI THEO CỔNG
//   Người dùng đổi được Port trong UI. Rule gắn theo đường dẫn exe nên đổi cổng vẫn
//   chạy, và cũng chỉ cần một rule cho mọi cổng.
//
// LIÊN QUAN: net/UdpSocket.cpp (bind INADDR_ANY), AgentLoop.cpp (gọi Ensure khi mở
//            socket), ui/MainMenuWindow.cpp (quyết định elevate), ElevatedShare.h
// =============================================================================

// True nếu rule inbound của Deskhub đã tồn tại. Chỉ ĐỌC firewall nên chạy được ở
// quyền thường — dùng để quyết định có cần bung UAC hay không trước khi share.
bool HostFirewallRulePresent();

// Bảo đảm rule tồn tại: có sẵn thì thôi, chưa có thì thêm. Trả true khi rule đã hiện
// diện sau lời gọi. Việc THÊM đòi quyền admin — chạy quyền thường sẽ trả false (và
// không gây lỗi gì, chỉ là chưa mở được firewall).
bool EnsureHostFirewallRule();
