// =============================================================================
// StreamActivity.kt — màn hình xem: khung hình + overlay trạng thái.
//
// NHIỆM VỤ
//   Dựng Surface cho bộ giải mã vẽ vào, khởi động phiên, và hiển thị dòng trạng
//   thái. Nhận địa chỉ host và sourceId qua Intent extra từ MainActivity.
//
// ĐIỀU QUAN TRỌNG NHẤT: KHUNG HÌNH KHÔNG ĐI QUA COMPOSE
//   Compose chỉ lo phần chrome (chữ trạng thái, bố cục, letterbox). Pixel của video
//   đi thẳng từ bộ giải mã phần cứng ra màn hình qua hardware composer, không qua
//   view hierarchy, không qua CPU. Compose không hề biết tới nội dung đó.
//
// VÌ SAO SurfaceView CHỨ KHÔNG PHẢI TextureView
//   TextureView đi qua view hierarchy, thêm một lần copy trên GPU và khoảng một
//   frame trễ. Với một app mà độ trễ là tiêu chí hàng đầu thì đó là cái giá vô lý.
//   Đổi lại, SurfaceView không xoay/biến hình mượt được — ta không cần hai thứ đó.
//
// VÒNG ĐỜI SURFACE LÀ PHẦN DỄ SAI NHẤT
//   surfaceCreated  → giao Surface xuống C++.
//   surfaceDestroyed → thu hồi, và lời gọi này CHẶN tới khi bộ giải mã buông ra.
//   Thứ tự đó bắt buộc: hàm surfaceDestroyed trả về là hệ điều hành hủy Surface
//   thật, codec còn vẽ vào đó là lỗi dùng-sau-giải-phóng.
//
// VÌ SAO HỎI TRẠNG THÁI THEO NHỊP THAY VÌ ĐỂ C++ GỌI NGƯỢC LÊN
//   Gọi ngược từ C++ vào JVM đòi phải gắn thread vào JVM và giữ global ref, và phải
//   làm mỗi frame. Hỏi 500 ms một lần rẻ hơn nhiều, mà overlay chỉ đổi mỗi giây một
//   lần nên không cần nhanh hơn.
//
// LIÊN QUAN: MainActivity.kt (nơi mở màn hình này), NativeClient.kt, ClientLoop.h
// =============================================================================
package com.deskhub.app

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.delay

/**
 * Màn hình xem. Compose chỉ lo phần chrome (chữ trạng thái, bố cục) — khung hình
 * KHÔNG đi qua Compose: bộ giải mã ghi thẳng vào Surface của SurfaceView qua
 * hardware composer.
 *
 * Dùng SurfaceView bọc trong AndroidView chứ KHÔNG phải TextureView: TextureView đi
 * qua view hierarchy, thêm một lần copy GPU và khoảng một frame trễ.
 */
class StreamActivity : ComponentActivity() {

    private var started = false

    // Giữ ở Activity chứ không tạo trong composable: callback này phải sống đúng
    // bằng vòng đời SurfaceView, không được dựng lại theo mỗi lần recomposition.
    private val holderCallback = object : SurfaceHolder.Callback {
        override fun surfaceCreated(holder: SurfaceHolder) {
            NativeClient.nativeSetSurface(holder.surface)
        }

        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) {}

        override fun surfaceDestroyed(holder: SurfaceHolder) {
            // Chặn tới khi bộ giải mã buông surface — xem chú thích ở NativeClient.
            NativeClient.nativeSetSurface(null)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Người xem không chạm màn hình trong lúc xem, nên nếu không giữ cờ này thì
        // máy tự tắt màn hình giữa chừng — kéo theo Surface bị hủy và phiên đứt.
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        val addr = intent.getStringExtra("addr").orEmpty()
        // Không có "source" (vd. chạy thẳng từ adb) -> nguồn 0, như trước.
        started = NativeClient.nativeStart(addr, intent.getIntExtra("source", 0))

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                StreamScreen(
                    address = addr,
                    started = started,
                    holderCallback = holderCallback,
                    onDismiss = { finish() }
                )
            }
        }
    }

    override fun onDestroy() {
        if (started) NativeClient.nativeStop()
        super.onDestroy()
    }
}

@Composable
private fun StreamScreen(
    address: String,
    started: Boolean,
    holderCallback: SurfaceHolder.Callback,
    onDismiss: () -> Unit
) {
    var phase by remember { mutableIntStateOf(NativeClient.PHASE_IDLE) }
    var statusLine by remember { mutableStateOf("") }
    var endReason by remember { mutableStateOf("") }
    var videoW by remember { mutableIntStateOf(0) }
    var videoH by remember { mutableIntStateOf(0) }

    // Hỏi trạng thái từ tầng C++ 500ms/lần. Rẻ hơn nhiều so với để C++ gọi ngược
    // lên JVM mỗi frame, và overlay chỉ đổi mỗi giây một lần nên không cần nhanh hơn.
    LaunchedEffect(started) {
        if (!started) return@LaunchedEffect
        while (true) {
            phase = NativeClient.nativePhase()
            statusLine = NativeClient.nativeStatusLine()
            videoW = NativeClient.nativeVideoWidth()
            videoH = NativeClient.nativeVideoHeight()
            // Hết phiên thì thoát hẳn coroutine: lý do kết thúc không đổi nữa, hỏi
            // tiếp chỉ tốn pin. LaunchedEffect tự hủy coroutine khi rời màn hình.
            if (phase == NativeClient.PHASE_ENDED) {
                endReason = NativeClient.nativeEndReason()
                return@LaunchedEffect
            }
            delay(500)
        }
    }

    val message = when {
        !started -> stringResource(R.string.invalid_address, address)
        phase == NativeClient.PHASE_ENDED -> stringResource(R.string.disconnected, endReason)
        phase == NativeClient.PHASE_STREAMING -> statusLine
        else -> stringResource(R.string.connecting_to, address)
    }

    // Hết phiên thì chạm vào đâu cũng quay lại màn hình nhập địa chỉ.
    val rootModifier = Modifier
        .fillMaxSize()
        .background(Color.Black)
        .let { if (phase == NativeClient.PHASE_ENDED) it.clickable(onClick = onDismiss) else it }

    Box(modifier = rootModifier, contentAlignment = Alignment.Center) {
        if (started) {
            // Modifier.aspectRatio lo luôn việc letterbox theo tỉ lệ video — đây là
            // thứ bản NativeActivity thuần không làm nổi (phải tự tính layout params).
            val videoModifier =
                if (videoW > 0 && videoH > 0)
                    Modifier.aspectRatio(videoW.toFloat() / videoH.toFloat())
                else
                    Modifier.fillMaxSize()

            AndroidView(
                factory = { ctx -> SurfaceView(ctx).apply { holder.addCallback(holderCallback) } },
                modifier = videoModifier
            )
        }

        if (message.isNotEmpty()) {
            Text(
                text = message,
                color = Color.White,
                fontSize = 13.sp,
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 12.dp)
                    .background(Color(0xA0000000))
                    .padding(horizontal = 12.dp, vertical = 6.dp)
            )
        }
    }

}
