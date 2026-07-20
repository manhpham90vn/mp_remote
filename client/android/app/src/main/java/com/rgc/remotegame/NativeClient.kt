package com.rgc.remotegame

import android.view.Surface

/**
 * Mặt tiền Kotlin của libremotegame.so. Tên hàm native phải khớp đúng chữ với
 * JniBridge.cpp (Java_com_rgc_remotegame_NativeClient_*) — đổi tên gói hoặc tên
 * lớp là phải sửa cả bên C++.
 */
object NativeClient {

    // Trùng ClientLoop::Phase bên C++.
    const val PHASE_IDLE = 0
    const val PHASE_CONNECTING = 1
    const val PHASE_STREAMING = 2
    const val PHASE_ENDED = 3

    init {
        System.loadLibrary("remotegame")
    }

    /** "ip" hoặc "ip:port" (mặc định 47777). false nếu địa chỉ sai cú pháp. */
    external fun nativeStart(addr: String): Boolean
    external fun nativeStop()

    /**
     * Giao/thu hồi Surface. Truyền null CHẶN tới khi bộ giải mã buông surface ra,
     * nên bắt buộc gọi trong surfaceDestroyed() — trả về rồi thì Surface bị hủy
     * thật, codec còn vẽ vào đó là dùng-sau-giải-phóng.
     */
    external fun nativeSetSurface(surface: Surface?)

    external fun nativePhase(): Int
    external fun nativeStatusLine(): String
    external fun nativeEndReason(): String
    external fun nativeVideoWidth(): Int
    external fun nativeVideoHeight(): Int
}
