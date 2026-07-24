// =============================================================================
// InputTests.cpp — chuỗi event chuột/phím: khứ hồi wire, khử trùng + bù gói mất
// (chống kẹt phím), và bỏ gói đảo thứ tự.
// =============================================================================
#include "Tests.h"
#include "support/TestSupport.h"

#include "deskhub/input/InputReceiver.h"
#include "deskhub/input/InputSender.h"
#include "deskhub/input/KeyMap.h"

#include <cstdio>
#include <vector>

using namespace deskhub;

namespace {

InputEvent MakeKey(int32_t vk, int32_t scan, bool down) {
    InputEvent e;
    e.type = InputType::Key;
    e.a = vk;
    e.b = scan;
    e.state = down ? 1 : 0;
    return e;
}

void TestInputWireRoundtrip() {
    std::printf("[input] wire round-trip (batch of mixed events)...\n");
    std::vector<InputEvent> in;
    in.push_back(MakeKey('W', 0x11, true));
    InputEvent mv;
    mv.type = InputType::MouseMove;
    mv.a = -1234;
    mv.b = 5678;
    mv.absolute = 0;
    in.push_back(mv);
    InputEvent ab;
    ab.type = InputType::MouseMove;
    ab.a = 65535;
    ab.b = 0;
    ab.absolute = 1;
    in.push_back(ab);
    InputEvent wh;
    wh.type = InputType::MouseWheel;
    wh.b = -120;
    in.push_back(wh);
    for (auto& e : in) e.timestampUs = 0x0123456789ABCDEFull;

    uint8_t buf[kMaxDatagram];
    const size_t n = BuildInputEvents(buf, 0xCAFEBABE, 77, in);
    Check(n > 0, "BuildInputEvents succeeded");
    const auto h = ParseCommonHeader(std::span<const uint8_t>(buf, n));
    Check(h && h->type == MsgType::InputEvent && h->chan == Chan::Input, "input header correct");
    Check(h && h->sessionId == 0xCAFEBABE, "sessionId preserved");

    InputEvent out[kMaxInputEvents];
    uint32_t firstSeq = 0;
    const size_t got = ParseInputEvents(PayloadOf(std::span<const uint8_t>(buf, n)),
        firstSeq, out);
    Check(got == in.size() && firstSeq == 77, "event count + firstSeq correct");
    for (size_t i = 0; i < got && i < in.size(); ++i) {
        Check(out[i].type == in[i].type && out[i].a == in[i].a && out[i].b == in[i].b &&
                  out[i].state == in[i].state && out[i].absolute == in[i].absolute &&
                  out[i].timestampUs == in[i].timestampUs,
            "event roundtrip intact");
    }
    Check(got >= 2 && out[1].a == -1234, "negative coordinate keeps correct sign");
}

void TestInputSenderReceiver() {
    std::printf("[input] sender/receiver: dedupe + compensate for lost packets...\n");
    InputSender sender;
    sender.SetSessionId(1234);
    InputReceiver receiver;

    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    std::vector<InputEvent> sentEvents;
    uint64_t now = 0;
    for (int i = 0; i < 20; ++i) {
        const bool down = (i % 2) == 0;
        const auto e = MakeKey('A' + (i / 2), 0x1E + (i / 2), down);
        sender.Queue(e);
        sentEvents.push_back(e);
        now += 10'000;
        sender.Flush(now, send);
    }
    for (int i = 0; i < 4; ++i) {
        now += kInputRepeatIntervalUs;
        sender.Flush(now, send);
    }

    std::vector<InputEvent> applied;
    auto apply = [&](const InputEvent& e) { applied.push_back(e); };
    size_t dropped = 0;
    for (size_t i = 0; i < wire.size(); ++i) {
        if (i % 3 == 1) {
            ++dropped;
            continue;
        }
        receiver.HandlePacket(PayloadOf(wire[i]), apply);
    }
    Check(dropped > 0, "packet loss was simulated");
    Check(applied.size() == sentEvents.size(),
        "every event arrives exactly once despite losing 1/3 of packets");
    for (size_t i = 0; i < applied.size() && i < sentEvents.size(); ++i) {
        Check(applied[i].a == sentEvents[i].a && applied[i].state == sentEvents[i].state,
            "event correct order and content");
    }
    Check(receiver.stats().duplicates > 0, "duplicates were deduped");
    Check(receiver.stats().lost == 0, "no events lost");
}

void TestInputReorder() {
    std::printf("[input] out-of-order packet doesn't rewind applied state...\n");
    InputSender sender;
    InputReceiver receiver;
    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    uint64_t now = 0;
    for (int i = 0; i < 6; ++i) {
        sender.Queue(MakeKey('A' + i, 0x1E + i, true));
        now += 10'000;
        sender.Flush(now, send);
    }
    std::vector<InputEvent> applied;
    auto apply = [&](const InputEvent& e) { applied.push_back(e); };
    receiver.HandlePacket(PayloadOf(wire.back()), apply);
    const size_t afterNewest = applied.size();
    for (size_t i = 0; i + 1 < wire.size(); ++i)
        receiver.HandlePacket(PayloadOf(wire[i]), apply);
    Check(applied.size() == afterNewest, "late-arriving packet doesn't reapply old events");
    Check(applied.back().a == 'A' + 5, "final state is the newest event");
}

// Mất vượt sức bù của redundancy: chỉ gói đầu và gói cuối tới nơi -> receiver
// phải ĐẾM đúng số event đã mất (các test trên luôn về lost == 0).
void TestInputLossCounted() {
    std::printf("[input] loss beyond redundancy is counted, not hidden...\n");
    InputSender sender;
    InputReceiver receiver;
    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    uint64_t now = 0;
    for (int i = 0; i < 30; ++i) {
        sender.Queue(MakeKey('A' + i % 26, 0x1E, true));
        now += 10'000;
        sender.Flush(now, send);
    }
    Check(wire.size() == 30, "one datagram per flush");

    size_t applied = 0;
    auto apply = [&](const InputEvent&) { ++applied; };
    receiver.HandlePacket(PayloadOf(wire.front()), apply); // seq 0
    receiver.HandlePacket(PayloadOf(wire.back()), apply);  // seq 21..29
    Check(applied == 1 + kInputRedundancy + 1, "only the head and the tail arrive");
    // Giữa seq 0 và 21 là 20 event không gói nào mang tới.
    Check(receiver.stats().lost == 20, "the gap is counted as lost events");
}

// Dồn quá kInputBatchMax event vào MỘT lần Flush: phải chia nhiều datagram; dồn
// tiếp cho lịch sử vượt kHistoryMax: cắt đầu nhưng seq trên wire vẫn liên tục.
void TestInputMultiBatchAndTrim() {
    std::printf("[input] over-max batch splits, history trim keeps seq contiguous...\n");
    InputSender sender;
    InputReceiver receiver;
    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };
    std::vector<InputEvent> applied;
    auto apply = [&](const InputEvent& e) { applied.push_back(e); };

    // 30 event > kInputBatchMax = 24 -> một Flush phát 2 datagram.
    for (int i = 0; i < 30; ++i) sender.Queue(MakeKey('A', 0x1E, (i % 2) == 0));
    Check(sender.Flush(10'000, send) == 2, "over-max batch split into two datagrams");
    for (const auto& d : wire) receiver.HandlePacket(PayloadOf(d), apply);
    Check(applied.size() == 30, "all 30 events applied exactly once");

    // Dồn tiếp 60 event: lịch sử vượt kHistoryMax -> cắt phần đầu đã gửi,
    // firstSeq_ dời theo — receiver không được thấy lỗ hổng hay bản lặp nào.
    wire.clear();
    for (int i = 0; i < 60; ++i) sender.Queue(MakeKey('B', 0x30, (i % 2) == 0));
    sender.Flush(20'000, send);
    for (const auto& d : wire) receiver.HandlePacket(PayloadOf(d), apply);
    Check(applied.size() == 90, "history trim loses and duplicates nothing");
    Check(receiver.stats().lost == 0 && sender.nextSeq() == 90,
        "seq stays contiguous through the trim");
}

// Reset cả hai đầu: sender quên lịch sử/seq (phiên mới bắt đầu từ 0), receiver
// quên lastAppliedSeq (gói cũ áp dụng lại được).
void TestInputReset() {
    std::printf("[input] Reset clears both sender and receiver state...\n");
    InputSender sender;
    std::vector<Datagram> wire;
    auto send = [&](std::span<const uint8_t> d) { wire.emplace_back(d.begin(), d.end()); };

    sender.Queue(MakeKey('A', 0x1E, true));
    sender.Flush(10'000, send);
    sender.Reset();
    Check(sender.nextSeq() == 0 && !sender.pending(), "sender reset clears seq and queue");
    Check(sender.Flush(20'000, send) == 0, "nothing to send after reset");

    sender.Queue(MakeKey('B', 0x30, true));
    wire.clear();
    sender.Flush(30'000, send);
    uint32_t fs = 99;
    InputEvent ev[kMaxInputEvents];
    Check(ParseInputEvents(PayloadOf(wire[0]), fs, ev) == 1 && fs == 0,
        "first packet after reset restarts at seq 0");

    InputReceiver receiver;
    size_t applied = 0;
    auto apply = [&](const InputEvent&) { ++applied; };
    receiver.HandlePacket(PayloadOf(wire[0]), apply);
    receiver.HandlePacket(PayloadOf(wire[0]), apply); // bản lặp bị khử
    Check(applied == 1 && receiver.stats().duplicates == 1, "receiver dedupes before reset");
    receiver.Reset();
    receiver.HandlePacket(PayloadOf(wire[0]), apply);
    Check(applied == 2 && receiver.stats().duplicates == 0,
        "receiver reset forgets the seq history");
}

// Bảng ký tự -> tổ hợp phím (layout US) cho bàn phím ảo mobile/web: chữ thường/hoa,
// ký hiệu cần Shift, phím điều khiển, và ký tự ngoài bảng phải trả nullopt.
void TestCharToKeyChord() {
    std::printf("[input] KeyMap: char -> VK chord (US layout)...\n");

    auto chord = [](uint32_t cp) { return CharToKeyChord(cp); };

    Check(chord('a') && chord('a')->vk == 'A' && !chord('a')->shift, "'a' -> VK 'A' no shift");
    Check(chord('Z') && chord('Z')->vk == 'Z' && chord('Z')->shift, "'Z' -> VK 'Z' + shift");
    Check(chord('7') && chord('7')->vk == '7' && !chord('7')->shift, "digit maps to itself");
    Check(chord('!') && chord('!')->vk == '1' && chord('!')->shift, "'!' -> shift + '1'");
    Check(chord('?') && chord('?')->vk == 0xBF && chord('?')->shift, "'?' -> shift + VK_OEM_2");
    Check(chord('"') && chord('"')->vk == 0xDE && chord('"')->shift, "quote -> shift + VK_OEM_7");
    Check(chord(' ') && chord(' ')->vk == kVkSpace, "space -> VK_SPACE");
    Check(chord('\n') && chord('\n')->vk == kVkReturn, "newline -> VK_RETURN");
    Check(chord('\r') && chord('\r')->vk == kVkReturn, "CR -> VK_RETURN");
    Check(chord('\b') && chord('\b')->vk == kVkBack, "backspace -> VK_BACK");
    Check(chord('\t') && chord('\t')->vk == kVkTab, "tab -> VK_TAB");
    Check(!chord(0x1B), "bare ESC control char is not typeable");
    Check(!chord(0x1EA1 /* 'ạ' */), "non-ASCII has no chord");

    // Mọi ký tự ASCII in được phải có chord — bàn phím ảo gõ gì cũng không rơi rụng.
    for (uint32_t cp = 0x20; cp < 0x7F; ++cp)
        Check(chord(cp).has_value(), "every printable ASCII char has a chord");
}

} // namespace

void RunInputTests() {
    TestInputWireRoundtrip();
    TestInputSenderReceiver();
    TestInputReorder();
    TestInputLossCounted();
    TestInputMultiBatchAndTrim();
    TestInputReset();
    TestCharToKeyChord();
}
