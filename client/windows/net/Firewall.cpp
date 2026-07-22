// =============================================================================
// Firewall.cpp — cài đặt bằng COM (Windows Firewall API, INetFwPolicy2).
//
// BỐ CỤC
//   ComScope        — RAII cho CoInitializeEx, chịu được cả khi COM đã init sẵn.
//   SelfPath        — đường dẫn exe hiện tại, dùng làm ApplicationName của rule.
//   RuleState/OpenRules/Remove* — nội bộ, thao tác trên INetFwRules.
//   HostFirewallRulePresent / EnsureHostFirewallRule — API công khai (xem .h).
//
// VÌ SAO MỞ CẢ BA PROFILE (Domain + Private + Public)
//   Windows áp rule theo profile của mạng ĐANG nối. Một máy host Win10 để mạng ở
//   Public (rất hay gặp), hay rule tự-sinh của Windows chỉ gắn Public/Private lẻ,
//   đều làm gói inbound bị chặn dù "có rule". Không đoán được máy kia phân loại mạng
//   thế nào nên mở hết — rule vẫn chỉ giới hạn ĐÚNG exe này + UDP inbound.
//
// VÌ SAO KIỂM CHỨNG LẠI SAU KHI ADD
//   Add() trả S_OK không đồng nghĩa rule đã nằm trong danh sách trong mọi hoàn cảnh.
//   Ta tra lại theo tên để "in place" trong log là sự thật, không phải lời hứa.
//
// VÌ SAO CHỊU ĐƯỢC RPC_E_CHANGED_MODE
//   main.cpp đã init WinRT (MTA) trên chính thread gọi các hàm này. Gọi lại
//   CoInitializeEx với APARTMENTTHREADED trên thread đó trả RPC_E_CHANGED_MODE — KHÔNG
//   phải lỗi: COM vẫn dùng được ở apartment kia, chỉ là ta không được CoUninitialize.
//
// LIÊN QUAN: net/Firewall.h (vấn đề firewall + luồng), AgentLoop.cpp, ElevatedShare.h
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "net/Firewall.h"

#include <windows.h>
#include <netfw.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

// Tên định danh rule trong danh sách firewall. Remove/Item tra theo đúng tên này.
constexpr wchar_t kRuleName[] = L"RemoteGame (host)";

// Ba profile thật. NET_FW_PROFILE2_ALL (0x7fffffff) cũng được nhưng dùng tổ hợp bit
// tường minh để get_Profiles trả về đúng giá trị này, so sánh cho gọn.
constexpr long kWantProfiles =
    NET_FW_PROFILE2_DOMAIN | NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_PUBLIC;

// RAII cho CoInitializeEx. SUCCEEDED gồm cả S_OK lẫn S_FALSE (đều tăng ref -> đều
// phải CoUninitialize). RPC_E_CHANGED_MODE là thất bại theo SUCCEEDED nên không
// uninit, nhưng ok() vẫn true vì COM dùng được ở apartment đã init sẵn.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

std::wstring SelfPath() {
    wchar_t p[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(p, n);
}

bool PathEq(const wchar_t* a, const std::wstring& b) {
    return a && CompareStringOrdinal(a, -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// Mở INetFwRules. Người gọi Release() cả rules lẫn policy (qua ReleaseRules).
INetFwRules* OpenRules(INetFwPolicy2** outPolicy) {
    *outPolicy = nullptr;
    INetFwPolicy2* policy = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwPolicy2), (void**)&policy)) || !policy)
        return nullptr;
    INetFwRules* rules = nullptr;
    if (FAILED(policy->get_Rules(&rules)) || !rules) {
        policy->Release();
        return nullptr;
    }
    *outPolicy = policy;
    return rules;
}

void ReleaseRules(INetFwRules* rules, INetFwPolicy2* policy) {
    if (rules) rules->Release();
    if (policy) policy->Release();
}

// 0 = không có rule tên kRuleName; 1 = có nhưng thiếu profile (Public/Private) nên
// cần làm lại; 2 = có và phủ đủ ba profile.
int InspectOwnRule(INetFwRules* rules) {
    BSTR name = SysAllocString(kRuleName);
    if (!name) return 0;
    INetFwRule* rule = nullptr;
    int state = 0;
    if (SUCCEEDED(rules->Item(name, &rule)) && rule) {
        long prof = 0;
        if (SUCCEEDED(rule->get_Profiles(&prof)) &&
            (prof & kWantProfiles) == kWantProfiles)
            state = 2;
        else
            state = 1;
        rule->Release();
    }
    SysFreeString(name);
    return state;
}

// Bỏ mọi rule cùng tên kRuleName (Remove xoá tất cả rule trùng tên).
void RemoveOwnRule(INetFwRules* rules) {
    if (BSTR name = SysAllocString(kRuleName)) {
        rules->Remove(name);
        SysFreeString(name);
    }
}

// Bỏ các rule INBOUND action=BLOCK trỏ đúng exe này — thường do bấm "Cancel" ở popup
// firewall của Windows sinh ra, mà block THẮNG allow nên phải dọn.
void RemoveConflictingBlockRules(INetFwRules* rules, const std::wstring& exe) {
    IUnknown* unk = nullptr;
    if (FAILED(rules->get__NewEnum(&unk)) || !unk) return;
    IEnumVARIANT* en = nullptr;
    if (SUCCEEDED(unk->QueryInterface(__uuidof(IEnumVARIANT), (void**)&en)) && en) {
        std::vector<std::wstring> victims;
        VARIANT v;
        VariantInit(&v);
        ULONG got = 0;
        while (en->Next(1, &v, &got) == S_OK && got) {
            if (v.vt == VT_DISPATCH && v.pdispVal) {
                INetFwRule* r = nullptr;
                if (SUCCEEDED(v.pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)&r)) && r) {
                    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
                    NET_FW_ACTION act = NET_FW_ACTION_ALLOW;
                    BSTR app = nullptr, nm = nullptr;
                    r->get_Direction(&dir);
                    r->get_Action(&act);
                    r->get_ApplicationName(&app);
                    r->get_Name(&nm);
                    if (dir == NET_FW_RULE_DIR_IN && act == NET_FW_ACTION_BLOCK &&
                        PathEq(app, exe) && nm)
                        victims.emplace_back(nm);
                    if (app) SysFreeString(app);
                    if (nm) SysFreeString(nm);
                    r->Release();
                }
            }
            VariantClear(&v);
        }
        en->Release();
        for (const auto& n : victims)
            if (BSTR b = SysAllocString(n.c_str())) { rules->Remove(b); SysFreeString(b); }
    }
    unk->Release();
}

// Tạo rule allow inbound UDP cho exe, phủ cả ba profile. true nếu Add() trả S_OK.
bool AddOwnRule(INetFwRules* rules, const std::wstring& exe) {
    INetFwRule* rule = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(INetFwRule), (void**)&rule)) || !rule)
        return false;

    bool ok = false;
    BSTR name = SysAllocString(kRuleName);
    BSTR desc = SysAllocString(L"Allow incoming connections for RemoteGame host.");
    BSTR app  = SysAllocString(exe.c_str());
    if (name && app) {
        rule->put_Name(name);
        if (desc) rule->put_Description(desc);
        rule->put_ApplicationName(app);
        rule->put_Protocol(NET_FW_IP_PROTOCOL_UDP);
        rule->put_Direction(NET_FW_RULE_DIR_IN);
        rule->put_Action(NET_FW_ACTION_ALLOW);
        rule->put_Enabled(VARIANT_TRUE);
        rule->put_Profiles(kWantProfiles);
        const HRESULT hr = rules->Add(rule);
        ok = SUCCEEDED(hr);
        if (!ok)
            std::printf("[Firewall] Add() failed: hr=0x%08lx\n", (unsigned long)hr);
    }
    SysFreeString(name);
    SysFreeString(desc);
    SysFreeString(app);
    rule->Release();
    return ok;
}

} // namespace

bool HostFirewallRulePresent() {
    ComScope com;
    if (!com.ok()) return false;
    INetFwPolicy2* policy = nullptr;
    INetFwRules* rules = OpenRules(&policy);
    if (!rules) return false;
    const int state = InspectOwnRule(rules);
    ReleaseRules(rules, policy);
    return state == 2; // chỉ coi là "có" khi đã phủ đủ ba profile
}

bool EnsureHostFirewallRule() {
    const std::wstring exe = SelfPath();
    if (exe.empty()) return false;

    ComScope com;
    if (!com.ok()) {
        std::printf("[Firewall] COM init failed.\n");
        return false;
    }
    INetFwPolicy2* policy = nullptr;
    INetFwRules* rules = OpenRules(&policy);
    if (!rules) {
        std::printf("[Firewall] Could not open the firewall policy.\n");
        return false;
    }

    // Đã có rule đúng (đủ ba profile) thì thôi — khỏi cần quyền admin cho lần này.
    if (InspectOwnRule(rules) == 2) {
        ReleaseRules(rules, policy);
        return true;
    }

    // Cần sửa: dọn rule cũ thiếu profile + block rule của exe, rồi thêm rule đủ ba
    // profile. Mọi thao tác ghi đều đòi admin; không có quyền thì Add trả false.
    RemoveOwnRule(rules);
    RemoveConflictingBlockRules(rules, exe);
    AddOwnRule(rules, exe);

    // Kiểm chứng lại: chỉ báo thành công khi rule THẬT SỰ đã nằm trong danh sách.
    const bool ok = InspectOwnRule(rules) == 2;
    ReleaseRules(rules, policy);
    return ok;
}
