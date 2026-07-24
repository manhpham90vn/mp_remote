// =============================================================================
// DeskhubClient.mm — cài đặt mặt tiền C, bọc ClientLoop và SourceQuery.
//
// Đối ứng JniBridge.cpp bên Android: một biến toàn cục giữ phiên hiện tại, mọi
// hàm facade thao tác trên đó. Obj-C++ vì cần __bridge cast (layer là kiểu Obj-C).
//
// LIÊN QUAN: DeskhubClient.h (hợp đồng), ClientLoop.h, net/SourceQuery.h
// =============================================================================
#import <AVFoundation/AVFoundation.h>

#include "DeskhubClient.h"
#include "ClientLoop.h"
#include "net/SourceQuery.h"
#include "Log.h"

#include <memory>
#include <mutex>

namespace {

// Phiên duy nhất, y như g_client bên Android.
std::unique_ptr<ClientLoop> g_client;
std::mutex g_mutex;

// Buffer tĩnh cho chuỗi trả về (hợp lệ tới lần gọi kế). Thread-safe vì chỉ
// main thread gọi status_line/end_reason.
char g_statusBuf[256];
char g_reasonBuf[256];

constexpr uint16_t kDefaultPort = 47777;

} // namespace

int dh_list_sources(const char* address, DHSourceInfo* out, int capacity) {
    if (!address || !out || capacity <= 0) return 0;

    NetAddr addr;
    if (!ParseNetAddr(address, kDefaultPort, addr)) {
        LOGE("[Bridge] Invalid address: %s", address);
        return 0;
    }

    std::vector<deskhub::SourceInfo> sources;
    if (!QuerySources(addr, sources)) return 0;

    const int count = int(sources.size()) < capacity ? int(sources.size()) : capacity;
    for (int i = 0; i < count; ++i) {
        out[i].sourceId = sources[i].sourceId;
        out[i].width = sources[i].width;
        out[i].height = sources[i].height;
        std::strncpy(out[i].name, sources[i].name.c_str(), sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = '\0';
    }
    return count;
}

bool dh_start(const char* address, uint8_t sourceId) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) {
        g_client->Stop();
        g_client.reset();
    }

    NetAddr addr;
    if (!ParseNetAddr(address, kDefaultPort, addr)) {
        LOGE("[Bridge] Invalid address: %s", address);
        return false;
    }

    g_client = std::make_unique<ClientLoop>();
    if (!g_client->Start(addr, sourceId)) {
        g_client.reset();
        return false;
    }
    return true;
}

void dh_stop(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) {
        g_client->Stop();
        g_client.reset();
    }
}

void dh_set_layer(void* layer) {
    ClientLoop* cl = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        cl = g_client.get();
    }
    // SetLayer blocks until Decode thread acks — must not hold g_mutex during that
    // wait, otherwise poll calls (dh_phase, dh_status_line) from main thread deadlock.
    if (cl) cl->SetLayer(layer);
}

void dh_key_tap(int32_t vk, int32_t scan) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) g_client->QueueKeyTap(vk, scan);
}

void dh_key_chord(int32_t mod_vk, int32_t mod_scan, int32_t vk, int32_t scan) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) g_client->QueueKeyChord(mod_vk, mod_scan, vk, scan);
}

void dh_mouse_move(int32_t nx, int32_t ny) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) g_client->QueueMouseMoveAbs(nx, ny);
}

void dh_mouse_move_rel(int32_t dx, int32_t dy) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) g_client->QueueMouseMoveRel(dx, dy);
}

void dh_mouse_button(int32_t button, bool down) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) g_client->QueueMouseButton(button, down);
}

void dh_char_tap(uint32_t codepoint) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_client) g_client->QueueCharTap(codepoint);
}

DHPhase dh_phase(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_client) return DHPhaseIdle;
    return DHPhase(int(g_client->phase()));
}

const char* dh_status_line(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_client) {
        g_statusBuf[0] = '\0';
        return g_statusBuf;
    }
    const std::string s = g_client->StatusLine();
    std::strncpy(g_statusBuf, s.c_str(), sizeof(g_statusBuf) - 1);
    g_statusBuf[sizeof(g_statusBuf) - 1] = '\0';
    return g_statusBuf;
}

const char* dh_end_reason(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_client) {
        g_reasonBuf[0] = '\0';
        return g_reasonBuf;
    }
    const std::string s = g_client->EndReason();
    std::strncpy(g_reasonBuf, s.c_str(), sizeof(g_reasonBuf) - 1);
    g_reasonBuf[sizeof(g_reasonBuf) - 1] = '\0';
    return g_reasonBuf;
}

uint32_t dh_video_width(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_client ? g_client->videoWidth() : 0;
}

uint32_t dh_video_height(void) {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_client ? g_client->videoHeight() : 0;
}
