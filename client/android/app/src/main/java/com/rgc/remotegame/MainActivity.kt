package com.rgc.remotegame

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.Toast

/**
 * Màn hình nhập địa chỉ host. Nhớ lại địa chỉ lần trước để khỏi phải gõ lại IP
 * mỗi lần mở app — đây là app dùng đi dùng lại với đúng một cái máy.
 */
class MainActivity : Activity() {

    private lateinit var prefs: android.content.SharedPreferences

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        prefs = getSharedPreferences("rgc", Context.MODE_PRIVATE)

        val edit = findViewById<EditText>(R.id.editAddr)
        edit.setText(prefs.getString("addr", ""))

        findViewById<Button>(R.id.btnConnect).setOnClickListener {
            val addr = edit.text.toString().trim()
            if (addr.isEmpty()) {
                Toast.makeText(this, R.string.enter_address_first, Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            prefs.edit().putString("addr", addr).apply()
            startActivity(Intent(this, StreamActivity::class.java).putExtra("addr", addr))
        }
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        // Cho phép chạy thẳng từ adb như trước:
        //   am start -n com.rgc.remotegame/.MainActivity --es addr 10.0.2.2:47777
        intent?.getStringExtra("addr")?.let { addr ->
            startActivity(Intent(this, StreamActivity::class.java).putExtra("addr", addr))
        }
    }

    override fun onResume() {
        super.onResume()
        // Cùng mục đích như onNewIntent, cho lần khởi động đầu tiên.
        intent?.getStringExtra("addr")?.let { addr ->
            intent.removeExtra("addr") // chỉ dùng một lần, tránh vào lại là tự nhảy
            startActivity(Intent(this, StreamActivity::class.java).putExtra("addr", addr))
        }
    }
}
