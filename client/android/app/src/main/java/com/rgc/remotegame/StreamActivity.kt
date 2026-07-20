package com.rgc.remotegame

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView

/**
 * Màn hình xem. SurfaceView giữ nguyên vai trò cũ — bộ giải mã ghi thẳng vào
 * Surface của nó qua hardware composer, không frame nào đi qua tầng Java.
 *
 * Dùng SurfaceView chứ KHÔNG phải TextureView: TextureView đi qua view hierarchy,
 * thêm một lần copy GPU và khoảng một frame trễ.
 */
class StreamActivity : Activity(), SurfaceHolder.Callback {

    private lateinit var surfaceView: SurfaceView
    private lateinit var statusText: TextView
    private lateinit var container: FrameLayout

    private val handler = Handler(Looper.getMainLooper())
    private var started = false
    private var appliedW = 0
    private var appliedH = 0

    private val poll = object : Runnable {
        override fun run() {
            updateUi()
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_stream)

        container = findViewById(R.id.container)
        surfaceView = findViewById(R.id.surface)
        statusText = findViewById(R.id.status)
        surfaceView.holder.addCallback(this)

        val addr = intent.getStringExtra("addr").orEmpty()
        statusText.text = getString(R.string.connecting_to, addr)

        started = NativeClient.nativeStart(addr)
        if (!started) {
            statusText.text = getString(R.string.invalid_address, addr)
            return
        }
        handler.post(poll)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        NativeClient.nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // Chặn tới khi bộ giải mã buông surface — xem chú thích ở NativeClient.
        NativeClient.nativeSetSurface(null)
    }

    override fun onDestroy() {
        handler.removeCallbacks(poll)
        if (started) NativeClient.nativeStop()
        super.onDestroy()
    }

    private fun updateUi() {
        when (NativeClient.nativePhase()) {
            NativeClient.PHASE_STREAMING -> {
                applyAspect()
                val line = NativeClient.nativeStatusLine()
                statusText.text = line
                statusText.visibility = if (line.isEmpty()) View.GONE else View.VISIBLE
            }
            NativeClient.PHASE_ENDED -> {
                val reason = NativeClient.nativeEndReason()
                statusText.visibility = View.VISIBLE
                statusText.text = getString(R.string.disconnected, reason)
                handler.removeCallbacks(poll)
                container.setOnClickListener { finish() }
            }
            else -> statusText.visibility = View.VISIBLE
        }
    }

    /**
     * Đặt SurfaceView đúng tỉ lệ video thay vì kéo giãn đầy màn hình. Đây chính là
     * thứ bản NativeActivity thuần không làm được: đổi kích thước Surface phải đi
     * qua SurfaceHolder bên Java.
     */
    private fun applyAspect() {
        val vw = NativeClient.nativeVideoWidth()
        val vh = NativeClient.nativeVideoHeight()
        if (vw <= 0 || vh <= 0) return
        if (vw == appliedW && vh == appliedH) return

        val cw = container.width
        val ch = container.height
        if (cw == 0 || ch == 0) return

        // Khung lớn nhất vừa trong container mà vẫn đúng tỉ lệ video (letterbox).
        val scale = minOf(cw.toFloat() / vw, ch.toFloat() / vh)
        val lp = surfaceView.layoutParams as FrameLayout.LayoutParams
        lp.width = (vw * scale).toInt()
        lp.height = (vh * scale).toInt()
        surfaceView.layoutParams = lp

        appliedW = vw
        appliedH = vh
    }
}
