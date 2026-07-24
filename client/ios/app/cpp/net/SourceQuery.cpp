// =============================================================================
// SourceQuery.cpp — cài đặt vòng hỏi-đáp LIST_SOURCES (bản iOS, CHÉP từ Android).
//
// CẤU TRÚC: một vòng lặp chạy tối đa 3 giây, mỗi vòng làm hai việc
//   1. Cứ 500 ms thì phát lại LIST_SOURCES. Phát lại chứ không gửi một lần, vì gói
//      đi trên UDP và gói đầu tiên mất là chuyện hoàn toàn bình thường.
//   2. Chờ nhận tối đa 200 ms. Hết hạn thì quay lại kiểm tra tổng thời gian.
//
// VỀ HAI MỐC THỜI GIAN 200 ms VÀ 500 ms
//   Hạn nhận (200 ms) phải NGẮN HƠN nhịp phát lại (500 ms), nếu không vòng lặp sẽ
//   ngủ quên trong recvfrom và bỏ lỡ thời điểm phát lại.
//
// TRẢ VỀ NGAY KHI CÓ CÂU TRẢ LỜI ĐẦU TIÊN
//   Không chờ hết 3 giây để gom thêm — SOURCE_LIST mang trọn danh sách trong một
//   datagram.
//
// LIÊN QUAN: net/SourceQuery.h (hợp đồng gọi + ý nghĩa giá trị trả về)
// =============================================================================
#include "net/SourceQuery.h"

#include "Log.h"
#include "deskhubp/Clock.h"

namespace {
constexpr uint64_t kQueryTimeoutUs = 3'000'000; // người dùng đang đứng chờ
constexpr uint64_t kResendUs = 500'000;
} // namespace

bool QuerySources(const NetAddr& server, std::vector<deskhub::SourceInfo>& out) {
    out.clear();

    // Socket riêng, cổng ngẫu nhiên, sống đúng trong hàm này — destructor tự đóng.
    // Không dùng chung socket với ClientLoop vì lúc này ClientLoop còn chưa tồn tại.
    UdpSocket sock;
    if (!sock.Open(0)) {
        LOGE("[Sources] Failed to open socket.");
        return false;
    }
    sock.SetRecvTimeout(200);

    uint8_t buf[deskhub::kMaxDatagram];
    const size_t qn = deskhub::BuildListSources(buf);
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

        // Lọc kỹ: cổng này vừa mở nên về lý thuyết chỉ có host trả lời, nhưng gói
        // lạc từ máy khác trong mạng LAN vẫn tới được. Chỉ nhận đúng SOURCE_LIST.
        const auto span = std::span<const uint8_t>(buf, size_t(n));
        const auto h = deskhub::ParseCommonHeader(span);
        if (!h || h->type != deskhub::MsgType::SourceList) continue;

        // Đệm tạm cỡ cố định trên stack; ParseSourceList tự kẹp theo sức chứa này.
        deskhub::SourceInfo tmp[deskhub::kMaxSources];
        const size_t cnt = deskhub::ParseSourceList(deskhub::PayloadOf(span), tmp);
        for (size_t i = 0; i < cnt; ++i) out.push_back(std::move(tmp[i]));
        LOGI("[Sources] Host is sharing %zu source(s).", out.size());
        return true;
    }

    // Hết 3 giây mà im lặng. Cảnh báo chứ không phải lỗi — host đời trước GĐ6 không
    // biết LIST_SOURCES, và caller sẽ tự lùi về nguồn 0.
    LOGW("[Sources] No SOURCE_LIST from %s after %llu ms.", server.ToString().c_str(),
        (unsigned long long)(kQueryTimeoutUs / 1000));
    return false;
}
