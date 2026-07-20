package com.rgc.remotegame

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Màn hình nhập địa chỉ host. Nhớ lại địa chỉ lần trước để khỏi phải gõ lại IP mỗi
 * lần mở app — đây là app dùng đi dùng lại với đúng một cái máy.
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prefs = getSharedPreferences("rgc", Context.MODE_PRIVATE)

        // Vẫn cho chạy thẳng từ adb để test nhanh:
        //   am start -n com.rgc.remotegame/.MainActivity --es addr 10.0.2.2:47777
        intent?.getStringExtra("addr")?.let { addr ->
            intent.removeExtra("addr") // chỉ dùng một lần, quay lại không tự nhảy nữa
            openStream(addr)
        }

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    ConnectScreen(
                        initialAddress = prefs.getString("addr", "").orEmpty(),
                        onConnect = { addr ->
                            prefs.edit().putString("addr", addr).apply()
                            openStream(addr)
                        }
                    )
                }
            }
        }
    }

    private fun openStream(addr: String) {
        startActivity(Intent(this, StreamActivity::class.java).putExtra("addr", addr))
    }
}

@Composable
private fun ConnectScreen(initialAddress: String, onConnect: (String) -> Unit) {
    var address by remember { mutableStateOf(initialAddress) }
    val trimmed = address.trim()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = stringResource(R.string.app_name),
            fontSize = 28.sp,
            color = MaterialTheme.colorScheme.onSurface
        )
        Text(
            text = stringResource(R.string.host_address_label),
            fontSize = 14.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 8.dp, bottom = 24.dp)
        )

        OutlinedTextField(
            value = address,
            onValueChange = { address = it },
            singleLine = true,
            placeholder = { Text(stringResource(R.string.host_address_hint)) },
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Go),
            // Enter trên bàn phím ảo = bấm Connect, khỏi phải với tay xuống nút.
            keyboardActions = KeyboardActions(onGo = {
                if (trimmed.isNotEmpty()) onConnect(trimmed)
            }),
            modifier = Modifier.fillMaxWidth()
        )

        Button(
            onClick = { onConnect(trimmed) },
            enabled = trimmed.isNotEmpty(), // thay cho Toast "nhập địa chỉ trước"
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 20.dp)
        ) {
            Text(stringResource(R.string.connect))
        }

        Text(
            text = stringResource(R.string.emulator_hint),
            fontSize = 12.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 28.dp)
        )
    }
}
