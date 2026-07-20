#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "NetInfo.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "iphlpapi.lib")

std::vector<AdapterAddr> ListLocalIPv4() {
    std::vector<AdapterAddr> out;

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf;
    bool ok = false;
    for (int tries = 0; tries < 3 && !ok; ++tries) {
        buf.resize(size);
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                            GAA_FLAG_SKIP_DNS_SERVER;
        const ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                             (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
        if (r == NO_ERROR) ok = true;
        else if (r != ERROR_BUFFER_OVERFLOW) return out; // size đã được cập nhật -> thử lại
    }
    if (!ok) return out;

    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sin = (const sockaddr_in*)u->Address.lpSockaddr;
            char ip[32];
            if (!InetNtopA(AF_INET, &sin->sin_addr, ip, sizeof(ip))) continue;
            if (std::strncmp(ip, "169.254.", 8) == 0) continue; // APIPA: mạng chưa có DHCP
            out.push_back(AdapterAddr{a->FriendlyName ? a->FriendlyName : L"?", ip});
        }
    }
    // Adapter ảo (vEthernet của Hyper-V/WSL...) xuống cuối: máy khác thường không
    // tới được các dải này. Vẫn giữ lại vì Hyper-V external switch là trường hợp
    // hợp lệ (IP thật nằm trên vEthernet).
    std::stable_sort(out.begin(), out.end(), [](const AdapterAddr& x, const AdapterAddr& y) {
        auto virt = [](const AdapterAddr& v) { return v.name.rfind(L"vEthernet", 0) == 0 ? 1 : 0; };
        return virt(x) < virt(y);
    });
    return out;
}
