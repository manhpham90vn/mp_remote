// =============================================================================
// ControlTests.cpp — chính sách điều tiết: BitrateController (tụt nhanh/lên chậm,
// bật-tắt FEC) và LinkStats (delta theo cửa sổ, dựng Feedback).
// =============================================================================
#include "Tests.h"
#include "support/TestSupport.h"

#include "deskhub/control/BitrateController.h"
#include "deskhub/control/LinkStats.h"

#include <cstdio>

using namespace deskhub;

namespace {

Feedback Fb(uint8_t lossPct, uint16_t rttMs = 20) {
    Feedback fb{};
    fb.lossPct = lossPct;
    fb.rttMs = rttMs;
    return fb;
}

void TestBitrateBackoff() {
    std::printf("[ctrl] bitrate: back off on loss, clamp at floor...\n");
    BitrateController c(20'000'000, 1'000'000);

    auto d = c.Update(Fb(5), 1'000'000);
    Check(d.changeBitrate && d.bitrateBps == 15'000'000, "loss 5% -> x0.75");
    c.CommitBitrate(d.bitrateBps);

    d = c.Update(Fb(2), 2'000'000);
    Check(d.changeBitrate && d.bitrateBps == 13'500'000, "loss 2% -> x0.90");
    c.CommitBitrate(d.bitrateBps);

    uint64_t now = 3'000'000;
    for (int i = 0; i < 40; ++i, now += 1'000'000) {
        d = c.Update(Fb(9), now);
        if (d.changeBitrate) c.CommitBitrate(d.bitrateBps);
    }
    Check(c.bitrateBps() >= 1'000'000, "sustained loss never drops below the floor");
    Check(c.bitrateBps() < 1'020'000, "sustained loss settles at the floor (within the 2% deadband)");

    const auto stable = c.Update(Fb(9), now);
    Check(!stable.changeBitrate, "at the floor -> no further renegotiation");
}

void TestBitrateRecovery() {
    std::printf("[ctrl] bitrate: ramp back up only after the link stays clean...\n");
    BitrateController c(20'000'000, 1'000'000);

    auto d = c.Update(Fb(5), 1'000'000);
    c.CommitBitrate(d.bitrateBps); // 15 Mbps, lastDecrease = 1s

    d = c.Update(Fb(0), 2'500'000);
    Check(!d.changeBitrate, "no ramp-up within the 2s cooldown after a decrease");

    d = c.Update(Fb(0), 4'000'000);
    Check(d.changeBitrate && d.bitrateBps == 16'000'000, "ramp-up is +5% of the ceiling");
    c.CommitBitrate(d.bitrateBps);

    uint64_t now = 5'000'000;
    for (int i = 0; i < 20; ++i, now += 1'000'000) {
        d = c.Update(Fb(0), now);
        if (d.changeBitrate) c.CommitBitrate(d.bitrateBps);
    }
    Check(c.bitrateBps() == 20'000'000, "ramp-up stops at the ceiling");
    d = c.Update(Fb(0), now);
    Check(!d.changeBitrate, "already at ceiling -> no renegotiation");
}

void TestBitrateUncommitted() {
    std::printf("[ctrl] bitrate: rejected change doesn't move the controller...\n");
    BitrateController c(20'000'000, 1'000'000);
    const auto d = c.Update(Fb(5), 1'000'000);
    Check(d.changeBitrate && d.bitrateBps == 15'000'000, "proposes the decrease");
    Check(c.bitrateBps() == 20'000'000, "but stays put until CommitBitrate");
    const auto d2 = c.Update(Fb(5), 2'000'000);
    Check(d2.bitrateBps == 15'000'000, "next round recomputes from the old rate");
}

void TestFecHysteresis() {
    std::printf("[ctrl] FEC: on immediately, off only after 5 clean seconds...\n");
    BitrateController c(20'000'000, 1'000'000);

    auto d = c.Update(Fb(0), 1'000'000);
    Check(!d.fecEnabled && !d.fecToggled, "FEC starts off and stays off on a clean link");

    d = c.Update(Fb(3), 2'000'000);
    Check(d.fecEnabled && d.fecToggled, "any real loss turns FEC on at once");

    uint64_t now = 3'000'000;
    for (int i = 0; i < 4; ++i, now += 1'000'000) {
        d = c.Update(Fb(0), now);
        Check(d.fecEnabled && !d.fecToggled, "FEC stays on through 4 clean seconds");
    }
    d = c.Update(Fb(0), now);
    Check(!d.fecEnabled && d.fecToggled, "FEC turns off on the 5th clean second");

    c.Update(Fb(4), now + 1'000'000);
    now += 2'000'000;
    for (int i = 0; i < 4; ++i, now += 1'000'000) c.Update(Fb(0), now);
    Check(c.fecEnabled(), "clean-second counter restarts after a fresh loss");
}

void TestLinkStatsWindow() {
    std::printf("[ctrl] LinkStats: per-window deltas and rates...\n");
    LinkStats ls(0);
    Check(!ls.Due(999'999), "window not due before 1s");
    Check(ls.Due(1'000'000), "window due at 1s");

    Reassembler::Stats st{};
    st.packetsReceived = 900;
    st.packetsLost = 100;
    st.packetsRecovered = 7;
    st.framesDropped = 3;
    st.lossRuns[0] = 2;
    st.lossRuns[3] = 1;
    st.lossRunMax = 6;

    const LinkWindow w = ls.Close(st, 250'000 /*bytes*/, 60 /*frames*/, 1'000'000);
    Check(w.packetsLost == 100 && w.packetsReceived == 900, "first window = raw counters");
    Check(w.lossPct > 9.99 && w.lossPct < 10.01, "lossPct = lost/(lost+received)");
    Check(w.fps > 59.9 && w.fps < 60.1, "fps from rendered count");
    Check(w.kbps > 1999.0 && w.kbps < 2001.0, "kbps from video bytes");
    Check(w.lossRunTotal == 3 && w.lossRunMax == 6, "loss-run buckets summed, max passed through");

    st.packetsReceived += 1000;
    st.framesDropped += 1;
    const LinkWindow w2 = ls.Close(st, 500'000, 60, 2'000'000);
    Check(w2.packetsReceived == 1000 && w2.packetsLost == 0, "second window is a delta");
    Check(w2.lossPct == 0.0, "clean second reports 0% loss");
    Check(w2.framesDropped == 1, "dropped frames are per-window too");

    // Gói về muộn: đếm theo cửa sổ, độ muộn trung bình tính trên đúng cửa sổ này,
    // lateMsMax là kỷ lục tích luỹ chép thẳng qua.
    st.latePackets += 4;
    st.lateMsSum += 100;
    st.lateMsMax = 60;
    const LinkWindow w3 = ls.Close(st, 0, 0, 3'000'000);
    Check(w3.latePackets == 4 && w3.lateMsAvg == 25.0 && w3.lateMsMax == 60,
        "late-packet stats are per-window, avg over this window only");
}

void TestLinkStatsUsesRealElapsed() {
    std::printf("[ctrl] LinkStats: rates use the real window length...\n");
    LinkStats ls(0);
    Reassembler::Stats st{};
    const LinkWindow w = ls.Close(st, 250'000, 60, 2'000'000); // cửa sổ dài 2s
    Check(w.secs > 1.99 && w.secs < 2.01, "window length is measured, not assumed");
    Check(w.fps > 29.9 && w.fps < 30.1, "60 frames over 2s = 30 fps, not 60");
}

void TestFeedbackFromWindow() {
    std::printf("[ctrl] LinkStats: Feedback packet mirrors the window...\n");
    LinkWindow w;
    w.lossPct = 3.6;
    w.framesDropped = 2;
    w.kbps = 8'500.4;

    const Feedback fb = MakeFeedback(w, 21'400 /*rttUs*/);
    Check(fb.lossPct == 4, "lossPct rounds to nearest (3.6 -> 4)");
    Check(fb.lostFrames == 2, "lostFrames carried through");
    Check(fb.rttMs == 21, "RTT converted us -> ms");
    Check(fb.recvBitrateKbps == 8500, "recv bitrate carried through");

    LinkWindow clean;
    const Feedback fb2 = MakeFeedback(clean, 0);
    Check(fb2.lossPct == 0 && fb2.lostFrames == 0, "a clean window is still a valid Feedback");
}

} // namespace

void RunControlTests() {
    TestBitrateBackoff();
    TestBitrateRecovery();
    TestBitrateUncommitted();
    TestFecHysteresis();
    TestLinkStatsWindow();
    TestLinkStatsUsesRealElapsed();
    TestFeedbackFromWindow();
}
