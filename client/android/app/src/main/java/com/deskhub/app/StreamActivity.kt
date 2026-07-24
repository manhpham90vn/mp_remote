// =============================================================================
// StreamActivity.kt — màn hình xem + điều khiển: header bên trên, khung hình dưới.
//
// NHIỆM VỤ
//   Dựng Surface cho bộ giải mã vẽ vào, khởi động phiên, và điều khiển host:
//   - Trackpad ảo trên khung video: con trỏ luôn hiện, rê ngón di chuột theo delta,
//     tap 1 = click trái, tap 2 = click phải, giữ rồi kéo = drag (TrackpadOverlay).
//   - Nút Keys bật bàn phím ảo, phím gõ chạy sang host (KeyInputView).
//   - Nút Disconnect kết thúc phiên.
//   Nhận địa chỉ host và sourceId qua Intent extra từ MainActivity. Màn hình xoay
//   theo hướng thiết bị (fullUser trong manifest) — không ép ngang.
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

import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectDragGesturesAfterLongPress
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.isSpecified
import androidx.compose.ui.geometry.isUnspecified
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import kotlin.math.roundToInt
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
    private val holderCallback =
        object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                NativeClient.nativeSetSurface(holder.surface)
            }

            override fun surfaceChanged(
                h: SurfaceHolder,
                f: Int,
                w: Int,
                ht: Int,
            ) {}

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
                    onDismiss = { finish() },
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
    onDismiss: () -> Unit,
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

    val message =
        when {
            !started -> stringResource(R.string.invalid_address, address)
            phase == NativeClient.PHASE_ENDED -> stringResource(R.string.disconnected, endReason)
            phase == NativeClient.PHASE_STREAMING -> statusLine
            else -> stringResource(R.string.connecting_to, address)
        }

    // Hết phiên thì chạm vào đâu cũng quay lại màn hình nhập địa chỉ.
    // imePadding: bàn phím ảo đẩy lên thì cả cột co lại phía trên nó — video thu nhỏ
    // chứ không bị che, thanh nút dưới đáy nổi lên trên bàn phím (nút Keys vẫn với
    // tới để đóng). Cần adjustResize trong manifest để inset IME được phát xuống.
    val rootModifier =
        Modifier
            .fillMaxSize()
            .background(Color.Black)
            .imePadding()
            .let { if (phase == NativeClient.PHASE_ENDED) it.clickable(onClick = onDismiss) else it }

    val streaming = phase == NativeClient.PHASE_STREAMING

    // Bàn phím ảo: bật/tắt bằng nút Keys trên header. KeyInputView vô hình giữ focus
    // để IME gửi phím; xem giải thích cơ chế TYPE_NULL trong KeyInputView.kt.
    var keyboardOn by remember { mutableStateOf(false) }
    var keyView by remember { mutableStateOf<KeyInputView?>(null) }
    LaunchedEffect(keyboardOn) {
        val v = keyView ?: return@LaunchedEffect
        val imm = v.context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (!keyboardOn) {
            imm.hideSoftInputFromWindow(v.windowToken, 0)
            return@LaunchedEffect
        }
        v.requestFocus()
        imm.showSoftInput(v, 0)

        // Người dùng có thể hạ bàn phím bằng nút ẩn/Back của chính IME, không qua nút
        // Keys — canh ime inset để trạng thái nút không kẹt ở "đang bật". Phải chờ
        // THẤY bàn phím hiện rồi mới canh lúc ẩn, kẻo tắt nhầm khi IME còn đang trượt
        // lên. rootWindowInsets chỉ báo được IME từ API 30; máy cũ hơn giữ hành vi cũ
        // (nút không tự tắt, bấm Keys hai lần để mở lại). Coroutine tự hủy khi
        // keyboardOn đổi hoặc rời màn hình — không rò vòng lặp.
        if (Build.VERSION.SDK_INT < 30) return@LaunchedEffect
        var seen = false
        while (true) {
            val visible =
                v.rootWindowInsets?.isVisible(android.view.WindowInsets.Type.ime()) == true
            if (visible) {
                seen = true
            } else if (seen) {
                keyboardOn = false
                return@LaunchedEffect
            }
            delay(200)
        }
    }

    // Header cố định bên trên, video chiếm phần còn lại — thay cho overlay đè lên
    // hình như trước. Chế độ NGANG: trạng thái + cụm nút nằm gọn một hàng trên cùng.
    // Chế độ DỌC: màn hình hẹp không đủ chỗ cho cả hai — header chỉ còn dòng trạng
    // thái (cho 2 dòng để hiện hết thông số), cụm nút dời xuống thanh đáy.
    val portrait =
        LocalConfiguration.current.orientation == Configuration.ORIENTATION_PORTRAIT

    Column(modifier = rootModifier) {
        Row(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .background(Color(0xFF161616))
                    .padding(horizontal = 12.dp, vertical = if (portrait) 8.dp else 2.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = message,
                color = Color.White,
                fontSize = 12.sp,
                maxLines = if (portrait) 2 else 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f),
            )
            if (!portrait) {
                ControlButtons(
                    controlsEnabled = streaming,
                    keyboardOn = keyboardOn,
                    onToggleKeyboard = { keyboardOn = !keyboardOn },
                    onDisconnect = onDismiss,
                )
            }
        }

        Box(
            modifier =
                Modifier
                    .fillMaxWidth()
                    .weight(1f),
            contentAlignment = Alignment.Center,
        ) {
            if (started) {
                // Modifier.aspectRatio lo luôn việc letterbox theo tỉ lệ video — đây là
                // thứ bản NativeActivity thuần không làm nổi (phải tự tính layout params).
                // Trackpad phủ đúng khung video (không phải cả Box) để toạ độ con trỏ
                // chuẩn hoá 0..65535 khớp với khung hình host capture.
                val videoModifier =
                    if (videoW > 0 && videoH > 0) {
                        Modifier.aspectRatio(videoW.toFloat() / videoH.toFloat())
                    } else {
                        Modifier.fillMaxSize()
                    }

                Box(modifier = videoModifier) {
                    AndroidView(
                        factory = { ctx ->
                            SurfaceView(ctx).apply { holder.addCallback(holderCallback) }
                        },
                        modifier = Modifier.fillMaxSize(),
                    )
                    if (streaming) TrackpadOverlay(modifier = Modifier.fillMaxSize())
                }

                // View hứng phím: 1dp, vô hình, chỉ tồn tại để giữ focus cho IME.
                AndroidView(
                    factory = { ctx ->
                        KeyInputView(ctx)
                            .apply { onChar = { cp -> NativeClient.nativeCharTap(cp) } }
                            .also { keyView = it }
                    },
                    modifier = Modifier.size(1.dp),
                )
            }
        }

        // Thanh nút dưới đáy — chỉ ở chế độ dọc (xem chú thích trên header).
        if (portrait) {
            Row(
                modifier =
                    Modifier
                        .fillMaxWidth()
                        .background(Color(0xFF161616)),
                horizontalArrangement = Arrangement.SpaceEvenly,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ControlButtons(
                    controlsEnabled = streaming,
                    keyboardOn = keyboardOn,
                    onToggleKeyboard = { keyboardOn = !keyboardOn },
                    onDisconnect = onDismiss,
                )
            }
        }
    }
}

/**
 * Trackpad ảo phủ lên khung video, kiểu bàn di chuột laptop: con trỏ LUÔN hiện,
 * ngón tay rê ở đâu cũng được — con trỏ dịch theo DELTA chứ không nhảy tới điểm
 * chạm (khác hẳn kiểu chạm-trực-tiếp trước đây: ngón tay không che mất chỗ cần
 * bấm, và bấm được chính xác từng pixel).
 *
 *   Rê ngón       = di con trỏ.
 *   Tap 1 lần     = click trái TẠI CON TRỎ.
 *   Tap 2 lần     = click phải tại con trỏ.
 *   Giữ rồi kéo   = giữ chuột trái và rê (kéo cửa sổ, bôi đen), nhấc tay là nhả.
 *
 * Con trỏ tính bằng px trong khung video; gửi sang host dưới dạng chuẩn hoá
 * 0..65535 qua [sendMouseMove].
 */
@Composable
private fun TrackpadOverlay(modifier: Modifier) {
    var cursor by remember { mutableStateOf(Offset.Unspecified) }
    // Khung đổi kích thước (xoay màn hình) -> kẹp con trỏ lại trong khung mới.
    var bounds by remember { mutableStateOf(IntSize.Zero) }

    fun moveBy(delta: Offset) {
        if (bounds.width <= 0 || cursor.isUnspecified) return
        cursor =
            Offset(
                (cursor.x + delta.x).coerceIn(0f, bounds.width.toFloat()),
                (cursor.y + delta.y).coerceIn(0f, bounds.height.toFloat()),
            )
        sendMouseMove(cursor, bounds)
    }

    // Host cũng có người dùng thật di chuột được — gửi lại vị trí con trỏ ngay
    // trước mỗi cú click để chắc chắn click rơi đúng chỗ con trỏ đang hiển thị.
    fun clickAt(button: Int) {
        if (cursor.isUnspecified) return
        sendMouseMove(cursor, bounds)
        NativeClient.nativeMouseButton(button, true)
        NativeClient.nativeMouseButton(button, false)
    }

    Box(
        modifier =
            modifier
                .onSizeChanged { sz ->
                    bounds = sz
                    cursor =
                        if (cursor.isUnspecified) {
                            Offset(sz.width / 2f, sz.height / 2f)
                        } else {
                            Offset(
                                cursor.x.coerceIn(0f, sz.width.toFloat()),
                                cursor.y.coerceIn(0f, sz.height.toFloat()),
                            )
                        }
                }.pointerInput(Unit) {
                    // Có onDoubleTap nên onTap phải chờ hết cửa sổ double-tap
                    // (~300ms) mới nổ — giá phải trả để phân biệt được hai cử chỉ.
                    detectTapGestures(
                        onTap = { clickAt(NativeClient.MOUSE_LEFT) },
                        onDoubleTap = { clickAt(NativeClient.MOUSE_RIGHT) },
                    )
                }.pointerInput(Unit) {
                    // Rê tự do (không giữ nút nào): di con trỏ theo delta.
                    detectDragGestures(
                        onDrag = { change, delta ->
                            change.consume()
                            moveBy(delta)
                        },
                    )
                }.pointerInput(Unit) {
                    // Giữ yên tới ngưỡng long-press RỒI kéo = drag giữ chuột trái.
                    // Không tranh chấp với detectDrag thường: bên đó cần vượt touch
                    // slop trước, bên này cần đứng yên trước — loại trừ lẫn nhau.
                    detectDragGesturesAfterLongPress(
                        onDragStart = {
                            sendMouseMove(cursor, bounds)
                            NativeClient.nativeMouseButton(NativeClient.MOUSE_LEFT, true)
                        },
                        onDrag = { change, delta ->
                            change.consume()
                            moveBy(delta)
                        },
                        onDragEnd = {
                            NativeClient.nativeMouseButton(NativeClient.MOUSE_LEFT, false)
                        },
                        onDragCancel = {
                            NativeClient.nativeMouseButton(NativeClient.MOUSE_LEFT, false)
                        },
                    )
                },
    ) {
        if (cursor.isSpecified) {
            CursorArrow(
                modifier =
                    Modifier.offset { IntOffset(cursor.x.roundToInt(), cursor.y.roundToInt()) },
            )
        }
    }
}

/** Mũi tên con trỏ — vẽ tay bằng Path, trắng viền đen để nổi trên mọi nền video. */
@Composable
private fun CursorArrow(modifier: Modifier) {
    Canvas(modifier = modifier.size(18.dp)) {
        val w = size.width
        val h = size.height
        val p =
            Path().apply {
                moveTo(0f, 0f)
                lineTo(0f, h * 0.80f)
                lineTo(w * 0.22f, h * 0.62f)
                lineTo(w * 0.40f, h * 0.98f)
                lineTo(w * 0.55f, h * 0.90f)
                lineTo(w * 0.37f, h * 0.55f)
                lineTo(w * 0.63f, h * 0.55f)
                close()
            }
        drawPath(p, Color.White)
        drawPath(p, Color.Black, style = Stroke(width = 1.dp.toPx()))
    }
}

private fun sendMouseMove(
    pos: Offset,
    size: IntSize,
) {
    if (size.width <= 0 || size.height <= 0) return
    NativeClient.nativeMouseMove(
        (pos.x / size.width * 65535f).roundToInt(),
        (pos.y / size.height * 65535f).roundToInt(),
    )
}

/**
 * Cụm nút điều khiển phiên: Keys (bật/tắt bàn phím ảo để gõ chữ sang host) và
 * Disconnect. Keys chỉ bấm được khi đang STREAMING vì trước đó kênh input chưa
 * tồn tại. Không tự bọc Row — caller đặt vào header (ngang) hoặc thanh đáy (dọc).
 */
@Composable
private fun ControlButtons(
    controlsEnabled: Boolean,
    keyboardOn: Boolean,
    onToggleKeyboard: () -> Unit,
    onDisconnect: () -> Unit,
) {
    TextButton(onClick = onToggleKeyboard, enabled = controlsEnabled) {
        Text(
            text = stringResource(R.string.keyboard_button),
            fontSize = 13.sp,
            // Đang bật thì tô sáng để biết phím gõ đang chạy sang host.
            color = if (keyboardOn) Color(0xFF80CBC4) else Color.Unspecified,
        )
    }
    TextButton(onClick = onDisconnect) {
        Text(stringResource(R.string.disconnect), fontSize = 13.sp)
    }
}
