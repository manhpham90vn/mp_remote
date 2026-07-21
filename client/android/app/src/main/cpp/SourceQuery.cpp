#include "SourceQuery.h"

#include "Log.h"
#include "TimeUs.h"

namespace {
constexpr uint64_t kQueryTimeoutUs = 3'000'000; // người dùng đang đứng chờ
constexpr uint64_t kResendUs       = 500'000;
} // namespace

bool QuerySources(const NetAddr& server, std::vector<rgc::SourceInfo>& out) {
    out.clear();

    UdpSocket sock;
    if (!sock.Open(0)) { // cổng ngẫu nhiên
        LOGE("[Sources] Failed to open socket.");
        return false;
    }
    sock.SetRecvTimeout(200);

    uint8_t buf[rgc::kMaxDatagram];
    const size_t qn = rgc::BuildListSources(buf);
    if (!qn) return false;

    // Phát lại mỗi 500ms: LIST_SOURCES đi trên UDP, gói đầu mất là chuyện bình thường.
    const uint64_t startUs = NowUs();
    uint64_t lastSendUs = 0;
    while (NowUs() - startUs < kQueryTimeoutUs) {
        const uint64_t now = NowUs();
        if (now - lastSendUs >= kResendUs) {
            lastSendUs = now;
            sock.SendTo(server, buf, qn);
        }

        NetAddr from;
        const int n = sock.RecvFrom(buf, sizeof(buf), from);
        if (n <= 0) continue; // 0 = timeout 200ms, quay lại kiểm tra hạn 3s

        const auto span = std::span<const uint8_t>(buf, size_t(n));
        const auto h = rgc::ParseCommonHeader(span);
        if (!h || h->type != rgc::MsgType::SourceList) continue;

        rgc::SourceInfo tmp[rgc::kMaxSources];
        const size_t cnt = rgc::ParseSourceList(rgc::PayloadOf(span), tmp);
        for (size_t i = 0; i < cnt; ++i) out.push_back(std::move(tmp[i]));
        LOGI("[Sources] Host is sharing %zu source(s).", out.size());
        return true;
    }

    LOGW("[Sources] No SOURCE_LIST from %s after %llu ms.", server.ToString().c_str(),
         (unsigned long long)(kQueryTimeoutUs / 1000));
    return false;
}
