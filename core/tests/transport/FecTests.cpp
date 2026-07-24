// =============================================================================
// FecTests.cpp — FEC parity XOR nhóm XEN KẼ: khôi phục mất lẻ, mất chùm liên tiếp,
// giới hạn (2 mất cùng nhóm), nội dung byte-khớp, và frame một gói.
// =============================================================================
#include "Tests.h"
#include "support/TestSupport.h"

#include "deskhub/transport/Packetizer.h"
#include "deskhub/transport/Reassembler.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

using namespace deskhub;

namespace {

// Khung sườn chung: 20 frame cố định `pktsPerFrame` mảnh, IDR mỗi 10 frame, bật FEC,
// bỏ các gói dữ liệu theo `dropIdx` ở MỌI frame không phải IDR.
struct FecCase {
    size_t pktsPerFrame;
    std::vector<size_t> dropIdx; // chỉ số gói dữ liệu bị bỏ (trong frame)
};

void RunFecCase(const FecCase& c, std::vector<uint32_t>& got, Reassembler::Stats& stats,
    std::vector<TestFrame>& frames) {
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);
    frames.clear();
    for (uint32_t i = 0; i < 20; ++i) {
        TestFrame f{i, (i % 10) == 0, {}};
        f.nal.resize(c.pktsPerFrame * kMaxVideoPayload - 100); // -100: gói cuối ngắn
        for (auto& b : f.nal) b = uint8_t(Rnd());
        frames.push_back(std::move(f));
    }
    uint64_t now = 1'000'000;
    for (const auto& f : frames) {
        auto pkts = Packetize(pk, f, now);
        if (!f.idr) {
            std::vector<size_t> pos;
            for (size_t d : c.dropIdx) pos.push_back(NthDataPacket(pkts, d));
            std::sort(pos.begin(), pos.end(), std::greater<size_t>());
            for (size_t p : pos)
                if (p < pkts.size()) pkts.erase(pkts.begin() + p);
        }
        for (const auto& d : pkts) Feed(ra, d, now);
        while (auto out = ra.PopReady(now)) got.push_back(out->frameId);
        now += 16'667;
    }
    stats = ra.stats();
}

void TestFecDisabledByDefault() {
    std::printf("[fec] off by default -> no parity packets on the wire...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    TestFrame f{0, true, {}};
    f.nal.resize(3 * kMaxVideoPayload);
    for (auto& b : f.nal) b = uint8_t(Rnd());
    const auto pkts = Packetize(pk, f, 1'000'000);
    Check(pkts.size() == 3, "3 data packets, no parity when FEC disabled");
    for (const auto& d : pkts) Check(!IsFec(d), "no FEC packet when disabled");
}

void TestFecRecoverOne() {
    std::printf("[fec] 1 packet lost per frame -> recovered, no drop...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    RunFecCase({5, {2}}, got, st, frames);

    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 20; ++i) want.push_back(i);
    Check(got == want, "all frames delivered despite 1 loss each");
    Check(st.framesDropped == 0, "no frame dropped (FEC)");
    Check(st.packetsRecovered == 18, "18 non-IDR frames each recovered 1 packet");
}

void TestFecRecoverLastPacket() {
    std::printf("[fec] lost packet is the SHORT last one -> length restored...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    RunFecCase({5, {4}}, got, st, frames); // mảnh 4 = mảnh cuối, ngắn hơn kMaxVideoPayload

    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 20; ++i) want.push_back(i);
    Check(got == want, "all frames delivered when the short tail packet is lost");
    Check(st.framesDropped == 0, "no frame dropped (FEC, tail packet)");
}

void TestFecContentIntact() {
    std::printf("[fec] recovered frame is byte-identical...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);

    TestFrame f{0, true, {}};
    f.nal.resize(5 * kMaxVideoPayload - 100);
    for (auto& b : f.nal) b = uint8_t(Rnd());

    auto pkts = Packetize(pk, f, 1'000'000);
    pkts.erase(pkts.begin() + NthDataPacket(pkts, 3));
    for (const auto& d : pkts) Feed(ra, d, 1'000'000);

    auto out = ra.PopReady(1'000'000);
    Check(out.has_value(), "frame completed via FEC");
    if (out) Check(SameFrame(*out, f), "recovered frame byte-identical to original");
}

void TestFecTwoLossesSameGroup() {
    std::printf("[fec] 2 losses in one group -> cannot recover, old policy...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    // 5 mảnh -> numGroups = ceil(5/8) = 1, cả frame là một nhóm; bỏ 2 -> parity vô dụng.
    RunFecCase({5, {1, 3}}, got, st, frames);

    Check(st.packetsRecovered == 0, "nothing recovered with 2 losses in a group");
    Check(st.framesDropped > 0, "frames still dropped with 2 losses in a group");
    Check(std::find(got.begin(), got.end(), 10u) != got.end(), "recovers at next IDR");
}

// Điểm then chốt của nhóm XEN KẼ: một CHÙM mất liên tiếp mà cách gom liên tiếp cũ
// (nhóm = 8 gói cạnh nhau) sẽ chịu chết, nhóm xen kẽ lại cứu trọn.
void TestFecInterleavedBurst() {
    std::printf("[fec] interleaved: consecutive burst loss -> recovered...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    // 20 mảnh -> numGroups = 3. Chùm {3,4}: 3%3=0, 4%3=1 -> khác nhóm -> cứu cả hai.
    RunFecCase({20, {3, 4}}, got, st, frames);

    std::vector<uint32_t> want;
    for (uint32_t i = 0; i < 20; ++i) want.push_back(i);
    Check(got == want, "all frames delivered despite a 2-packet consecutive burst");
    Check(st.framesDropped == 0, "no frame dropped (interleaved FEC beats the burst)");
    Check(st.packetsRecovered == 36, "18 non-IDR frames each recovered 2 burst packets");
}

// Mặt trái trung thực: chùm mất rơi TRÙNG một nhóm xen kẽ (cách nhau đúng numGroups)
// thì vẫn không cứu nổi — parity một-ẩn không phải phép màu.
void TestFecInterleavedSameGroup() {
    std::printf("[fec] interleaved: 2 losses aligned to one group -> cannot recover...\n");
    std::vector<uint32_t> got;
    Reassembler::Stats st{};
    std::vector<TestFrame> frames;
    // 20 mảnh -> numGroups = 3. Bỏ {3,6}: 3%3=0 và 6%3=0 -> cùng nhóm 0 -> thiếu 2 -> chịu.
    RunFecCase({20, {3, 6}}, got, st, frames);
    Check(st.packetsRecovered == 0, "nothing recovered when both losses hit the same group");
    Check(st.framesDropped > 0, "frames still dropped when losses align to one group");
    Check(std::find(got.begin(), got.end(), 10u) != got.end(), "recovers at next IDR");
}

void TestFecSinglePacketFrame() {
    std::printf("[fec] 1-packet frame, data lost, parity alone rebuilds it...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);

    TestFrame f{0, true, {}};
    f.nal.resize(300);
    for (auto& b : f.nal) b = uint8_t(Rnd());

    auto pkts = Packetize(pk, f, 1'000'000);
    Check(pkts.size() == 2, "1 data packet + 1 parity packet");
    pkts.erase(pkts.begin() + NthDataPacket(pkts, 0)); // bỏ gói dữ liệu duy nhất
    for (const auto& d : pkts) Feed(ra, d, 1'000'000);

    auto out = ra.PopReady(1'000'000);
    Check(out.has_value(), "single-packet frame rebuilt from parity alone");
    if (out) Check(SameFrame(*out, f), "rebuilt single-packet frame identical");
}

// Parity hỏng (bẻ 2 byte lenXor): độ dài dựng lại ra số vô nghĩa -> TryRecover
// phải từ chối thay vì nhét rác vào NAL; frame chịu chết theo chính sách cũ.
void TestFecCorruptParityRejected() {
    std::printf("[fec] corrupt parity -> recovery refused, no garbage NAL...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);
    Reassembler ra(16'667);

    TestFrame f{0, true, {}};
    f.nal.resize(5 * kMaxVideoPayload - 100);
    for (auto& b : f.nal) b = uint8_t(Rnd());

    auto pkts = Packetize(pk, f, 1'000'000);
    pkts.erase(pkts.begin() + NthDataPacket(pkts, 2)); // mất một mảnh dữ liệu
    for (auto& d : pkts) {
        if (IsFec(d)) { // bẻ lenXor đứng đầu payload parity
            d[kCommonHeaderSize + kFecHeaderSize] = 0xFF;
            d[kCommonHeaderSize + kFecHeaderSize + 1] = 0xFF;
        }
        Feed(ra, d, 1'000'000);
    }
    Check(!ra.PopReady(1'000'000).has_value(), "corrupt parity doesn't complete the frame");
    Check(ra.stats().packetsRecovered == 0, "nothing 'recovered' from a corrupt parity");
}

// Frame cần quá kMaxFecGroups nhóm (groupIndex là u8 không đánh số nổi):
// Packetizer phải gửi trần không FEC thay vì phát parity sai.
void TestFecTooManyGroupsSendsPlain() {
    std::printf("[fec] frame needing > kMaxFecGroups groups -> sent plain, no parity...\n");
    Packetizer pk;
    pk.SetSessionId(42);
    pk.SetFecEnabled(true);

    // 2049 mảnh -> numGroups = 257 > 256. Nội dung không quan trọng, để 0 cho nhanh.
    const size_t count = kMaxFecGroups * kFecGroupSize + 1;
    TestFrame f{0, true, {}};
    f.nal.resize(count * kMaxVideoPayload);

    const auto pkts = Packetize(pk, f, 1'000'000);
    Check(pkts.size() == count, "all data packets still sent");
    size_t fec = 0;
    for (const auto& d : pkts) fec += IsFec(d) ? 1 : 0;
    Check(fec == 0, "no parity when the frame needs more than kMaxFecGroups groups");
}

} // namespace

void RunFecTests() {
    TestFecDisabledByDefault();
    TestFecRecoverOne();
    TestFecRecoverLastPacket();
    TestFecContentIntact();
    TestFecTwoLossesSameGroup();
    TestFecInterleavedBurst();
    TestFecInterleavedSameGroup();
    TestFecSinglePacketFrame();
    TestFecCorruptParityRejected();
    TestFecTooManyGroupsSendsPlain();
}
