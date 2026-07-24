// =============================================================================
// TestSupport.cpp — cài đặt tiện ích dùng chung (xem TestSupport.h).
// =============================================================================
#include "support/TestSupport.h"

#include <cstdio>

using namespace deskhub;

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}

static uint32_t g_rng = 0x1234ABCD;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

std::vector<TestFrame> MakeFrames(size_t count, size_t gop) {
    std::vector<TestFrame> v;
    v.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        TestFrame f;
        f.id = uint32_t(i);
        f.idr = (i % gop) == 0;
        size_t size = 0;
        switch (Rnd() % 4) {
            case 0: size = 80 + Rnd() % 300; break;
            case 1: size = kMaxVideoPayload; break;
            case 2: size = kMaxVideoPayload + 1; break;
            default: size = 8'000 + Rnd() % 40'000; break;
        }
        f.nal.resize(size);
        for (auto& b : f.nal) b = uint8_t(Rnd());
        v.push_back(std::move(f));
    }
    return v;
}

TestFrame MakeIdrFrame(uint32_t id, size_t pkts) {
    TestFrame f{id, true, {}};
    f.nal.resize(pkts * kMaxVideoPayload - 100);
    for (auto& b : f.nal) b = uint8_t(Rnd());
    return f;
}

std::vector<Datagram> Packetize(Packetizer& pk, const TestFrame& f, uint64_t tsUs) {
    std::vector<Datagram> out;
    pk.SendFrame(f.nal, f.id, tsUs, f.idr,
        [&](std::span<const uint8_t> d) { out.emplace_back(d.begin(), d.end()); });
    return out;
}

void Feed(Reassembler& ra, const Datagram& d, uint64_t nowUs) {
    const auto h = ParseCommonHeader(d);
    if (!h) {
        Check(false, "ParseCommonHeader on packet from Packetizer");
        return;
    }
    if (h->type == MsgType::FecPacket) {
        const auto v = ParseFecPacket(*h, PayloadOf(d));
        if (!v) {
            Check(false, "ParseFecPacket on packet from Packetizer");
            return;
        }
        ra.PushFec(*v, nowUs);
        return;
    }
    const auto v = ParseVideoPacket(*h, PayloadOf(d));
    if (!v) {
        Check(false, "ParseVideoPacket on packet from Packetizer");
        return;
    }
    ra.Push(*v, nowUs);
}

bool IsFec(const Datagram& d) {
    const auto h = ParseCommonHeader(d);
    return h && h->type == MsgType::FecPacket;
}

size_t NthDataPacket(const std::vector<Datagram>& pkts, size_t n) {
    size_t seen = 0;
    for (size_t i = 0; i < pkts.size(); ++i) {
        if (IsFec(pkts[i])) continue;
        if (seen == n) return i;
        ++seen;
    }
    return pkts.size();
}

bool SameFrame(const Reassembler::Frame& got, const TestFrame& want) {
    return got.frameId == want.id && got.idr == want.idr && got.nal == want.nal;
}
