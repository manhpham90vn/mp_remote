#pragma once
//
// NetInfo - liet ke dia chi IPv4 cua may theo tung card mang (Ethernet, Wi-Fi...).
// Dung cho man hinh chinh kieu AnyDesk: hien "dia chi may nay" de nguoi dung
// doc cho may kia --connect toi, va de agent in ra khi bat dau lang nghe.
//
#include <string>
#include <vector>

struct AdapterAddr {
    std::wstring name; // ten than thien cua adapter ("Ethernet", "Wi-Fi"...)
    std::string  ip;   // "192.168.1.10"
};

// Chi tra ve adapter dang Up, bo loopback va dia chi APIPA 169.254.x.x.
std::vector<AdapterAddr> ListLocalIPv4();
