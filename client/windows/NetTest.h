#pragma once
// Self-test M1 của GD3: packetize -> (trộn thứ tự / bỏ gói / trùng gói giả lập)
// -> reassemble, cộng với mô phỏng handshake HostSession/ClientSession nối nhau
// bằng "dây" trong bộ nhớ. Chạy hoàn toàn offline - không cần mạng, không GPU.
// Trả về 0 nếu mọi kiểm tra đạt.
int RunNetTest();
