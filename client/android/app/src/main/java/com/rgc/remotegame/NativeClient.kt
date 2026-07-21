package com.rgc.remotegame

import android.view.Surface
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

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

    /**
     * Host đang chia sẻ những cửa sổ nào. CHẶN tới ~3 giây (LIST_SOURCES đi trên UDP,
     * phát lại vài lần) nên phải gọi ngoài main thread — dùng [listSources].
     */
    private external fun nativeListSources(addr: String): Array<String>

    /**
     * `sourceId` lấy từ [listSources]; 0 = nguồn đầu tiên, cũng là thứ host đời cũ
     * (chỉ một nguồn) hiểu được. false nếu địa chỉ sai cú pháp.
     */
    external fun nativeStart(addr: String, sourceId: Int): Boolean
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

    /** Một cửa sổ host đang chia sẻ. `name` chỉ để hiển thị. */
    data class Source(val id: Int, val width: Int, val height: Int, val name: String)

    /**
     * Bọc [nativeListSources] và giải mã dạng "id\twidth\theight\tname" mà JNI trả về
     * (chuỗi phẳng thay vì dựng object Kotlin từ C++: bên đó chỉ cần NewStringUTF, khỏi
     * phải tra FindClass/GetMethodID cho một kiểu chỉ dùng đúng ở đây).
     *
     * Danh sách rỗng = host im lặng HOẶC không chia sẻ gì; caller cứ thử nguồn 0 và
     * để tầng dưới báo lỗi thật.
     */
    suspend fun listSources(addr: String): List<Source> = withContext(Dispatchers.IO) {
        nativeListSources(addr).mapNotNull { line ->
            // limit = 4: tiêu đề cửa sổ có thể chứa tab, không được cắt tiếp.
            val f = line.split('\t', limit = 4)
            if (f.size < 4) return@mapNotNull null
            val id = f[0].toIntOrNull() ?: return@mapNotNull null
            Source(id, f[1].toIntOrNull() ?: 0, f[2].toIntOrNull() ?: 0, f[3])
        }
    }
}
