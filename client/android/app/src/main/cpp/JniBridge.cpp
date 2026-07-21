//
// JniBridge — mặt tiền JNI của libremotegame.so, thay cho NativeActivity cũ.
//
// Ranh giới cố ý mỏng: Kotlin lo phần người dùng nhìn thấy (ô nhập IP, nút, chữ
// trạng thái), C++ lo mạng + giải mã. Không có logic nào ở đây ngoài chuyển đổi
// kiểu và giữ MỘT ClientLoop toàn cục.
//
// Vì sao toàn cục thay vì trả con trỏ về cho Kotlin giữ: app chỉ xem được một
// nguồn tại một thời điểm (view-only v1), nên một biến tĩnh vừa đủ và bỏ được cả
// lớp lỗi "Kotlin giữ con trỏ đã hủy". Mọi hàm dưới đây gọi từ UI thread.
//
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "ClientLoop.h"
#include "Log.h"
#include "SourceQuery.h"
#include "UdpSocket.h"

namespace {

constexpr uint16_t kDefaultPort = 47777; // trùng client/windows/MainMenuWindow.cpp

std::unique_ptr<ClientLoop> g_client;
ANativeWindow* g_window = nullptr;

jstring ToJString(JNIEnv* env, const std::string& s) {
    return env->NewStringUTF(s.c_str());
}

// Đọc jstring rồi thả ngay — mọi hàm dưới đây chỉ cần bản copy.
std::string FromJString(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

} // namespace

extern "C" {

// Trả về mảng String, mỗi dòng "id\twidth\theight\tname" — xem NativeClient.kt.
// Mảng RỖNG nghĩa là host không trả lời (bản cũ / mất gói), KHÁC với host trả lời
// rằng nó không chia sẻ gì; Kotlin gộp cả hai thành "cứ thử nguồn 0".
// CHẶN tới ~3s: gọi trên thread nền.
JNIEXPORT jobjectArray JNICALL
Java_com_rgc_remotegame_NativeClient_nativeListSources(JNIEnv* env, jobject, jstring addrStr) {
    jclass stringClass = env->FindClass("java/lang/String");

    const std::string addr = FromJString(env, addrStr);
    NetAddr server;
    std::vector<rgc::SourceInfo> sources;
    if (ParseNetAddr(addr, kDefaultPort, server)) {
        QuerySources(server, sources);
    } else {
        LOGE("[JNI] Invalid host address: \"%s\"", addr.c_str());
    }

    jobjectArray arr = env->NewObjectArray(jsize(sources.size()), stringClass, nullptr);
    for (size_t i = 0; i < sources.size(); ++i) {
        const rgc::SourceInfo& s = sources[i];
        char head[32];
        std::snprintf(head, sizeof(head), "%u\t%u\t%u\t", s.sourceId, s.width, s.height);
        env->SetObjectArrayElement(arr, jsize(i), ToJString(env, head + s.name));
    }
    return arr;
}

JNIEXPORT jboolean JNICALL
Java_com_rgc_remotegame_NativeClient_nativeStart(JNIEnv* env, jobject, jstring addrStr,
                                                 jint sourceId) {
    const std::string addr = FromJString(env, addrStr);

    NetAddr server;
    if (!ParseNetAddr(addr, kDefaultPort, server)) {
        LOGE("[JNI] Invalid host address: \"%s\"", addr.c_str());
        return JNI_FALSE;
    }

    g_client = std::make_unique<ClientLoop>();
    if (!g_client->Start(server, uint8_t(sourceId))) {
        g_client.reset();
        return JNI_FALSE;
    }
    // Surface có thể đã sẵn sàng trước khi bấm Connect (SurfaceView tạo xong trước).
    if (g_window) g_client->SetWindow(g_window);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_rgc_remotegame_NativeClient_nativeStop(JNIEnv*, jobject) {
    if (g_client) {
        g_client->Stop();
        g_client.reset();
    }
}

// surface = null khi Surface bị hủy. Phải gọi TRƯỚC khi Surface thật sự biến mất
// (tức trong surfaceDestroyed), vì SetWindow chặn tới khi codec buông nó ra.
JNIEXPORT void JNICALL
Java_com_rgc_remotegame_NativeClient_nativeSetSurface(JNIEnv* env, jobject, jobject surface) {
    if (surface) {
        ANativeWindow* w = ANativeWindow_fromSurface(env, surface);
        if (g_window && g_window != w) {
            if (g_client) g_client->SetWindow(nullptr);
            ANativeWindow_release(g_window);
        }
        g_window = w;
        if (g_client) g_client->SetWindow(g_window);
    } else {
        // Thứ tự bắt buộc: bảo ClientLoop buông trước, RỒI mới release. Đảo lại là
        // codec đang render vào một ANativeWindow đã bị hủy.
        if (g_client) g_client->SetWindow(nullptr);
        if (g_window) {
            ANativeWindow_release(g_window);
            g_window = nullptr;
        }
    }
}

JNIEXPORT jint JNICALL
Java_com_rgc_remotegame_NativeClient_nativePhase(JNIEnv*, jobject) {
    return g_client ? jint(g_client->phase()) : jint(ClientLoop::Phase::Idle);
}

JNIEXPORT jstring JNICALL
Java_com_rgc_remotegame_NativeClient_nativeStatusLine(JNIEnv* env, jobject) {
    return ToJString(env, g_client ? g_client->StatusLine() : std::string());
}

JNIEXPORT jstring JNICALL
Java_com_rgc_remotegame_NativeClient_nativeEndReason(JNIEnv* env, jobject) {
    return ToJString(env, g_client ? g_client->EndReason() : std::string());
}

JNIEXPORT jint JNICALL
Java_com_rgc_remotegame_NativeClient_nativeVideoWidth(JNIEnv*, jobject) {
    return g_client ? jint(g_client->videoWidth()) : 0;
}

JNIEXPORT jint JNICALL
Java_com_rgc_remotegame_NativeClient_nativeVideoHeight(JNIEnv*, jobject) {
    return g_client ? jint(g_client->videoHeight()) : 0;
}

} // extern "C"
