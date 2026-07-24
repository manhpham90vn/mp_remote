#pragma once
// =============================================================================
// Tests.h — điểm vào của từng nhóm test. Mỗi file test tầng định nghĩa một hàm
// Run*Tests() gom các ca của tầng đó; TestMain gọi lần lượt theo thứ tự này.
// =============================================================================
void RunWireTests();
void RunReassemblerTests();
void RunFecTests();
void RunRetransmitCacheTests();
void RunSessionTests();
void RunInputTests();
void RunControlTests();
