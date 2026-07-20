#pragma once
//
// NetInfo - liệt kê địa chỉ IPv4 của máy theo từng card mạng (Ethernet, Wi-Fi...).
// Dùng cho màn hình chính kiểu AnyDesk: hiện "địa chỉ máy này" để người dùng
// đọc cho máy kia --connect tới, và để agent in ra khi bắt đầu lắng nghe.
//
#include <string>
#include <vector>

struct AdapterAddr {
    std::wstring name; // tên thân thiện của adapter ("Ethernet", "Wi-Fi"...)
    std::string  ip;   // "192.168.1.10"
};

// Chỉ trả về adapter đang Up, bỏ loopback và địa chỉ APIPA 169.254.x.x.
std::vector<AdapterAddr> ListLocalIPv4();
