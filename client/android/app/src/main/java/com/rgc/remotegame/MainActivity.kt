package com.rgc.remotegame

import android.content.Context
import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.launch

/**
 * Màn hình nhập địa chỉ host, rồi chọn cửa sổ muốn xem nếu host chia sẻ nhiều cửa sổ.
 * Nhớ lại địa chỉ lần trước để khỏi phải gõ lại IP mỗi lần mở app — đây là app dùng
 * đi dùng lại với đúng một cái máy.
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val prefs = getSharedPreferences("rgc", Context.MODE_PRIVATE)

        // Vẫn cho chạy thẳng từ adb để test nhanh (bỏ qua bước chọn nguồn):
        //   am start -n com.rgc.remotegame/.MainActivity --es addr 10.0.2.2:47777
        intent?.getStringExtra("addr")?.let { addr ->
            intent.removeExtra("addr") // chỉ dùng một lần, quay lại không tự nhảy nữa
            openStream(addr, 0)
        }

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                Surface(modifier = Modifier.fillMaxSize()) {
                    MainScreen(
                        initialAddress = prefs.getString("addr", "").orEmpty(),
                        onRemember = { addr -> prefs.edit().putString("addr", addr).apply() },
                        onOpenStream = ::openStream
                    )
                }
            }
        }
    }

    private fun openStream(addr: String, sourceId: Int) {
        startActivity(
            Intent(this, StreamActivity::class.java)
                .putExtra("addr", addr)
                .putExtra("source", sourceId)
        )
    }
}

/** Ba bước của màn hình: gõ địa chỉ -> hỏi host có nguồn nào -> chọn nguồn. */
private sealed interface Step {
    data object Address : Step
    data object Querying : Step
    data class Picking(val sources: List<NativeClient.Source>) : Step
}

@Composable
private fun MainScreen(
    initialAddress: String,
    onRemember: (String) -> Unit,
    onOpenStream: (String, Int) -> Unit
) {
    var step by remember { mutableStateOf<Step>(Step.Address) }
    var address by remember { mutableStateOf(initialAddress) }
    val scope = rememberCoroutineScope()

    // Đang hỏi hoặc đang chọn: Back quay về ô địa chỉ thay vì thoát app. Coroutine hỏi
    // nguồn vẫn chạy nốt 3 giây của nó, nhưng kết quả bị bỏ qua vì step đã đổi.
    BackHandler(enabled = step != Step.Address) { step = Step.Address }

    when (val s = step) {
        is Step.Address -> AddressScreen(
            address = address,
            onAddressChange = { address = it },
            onConnect = { addr ->
                onRemember(addr)
                step = Step.Querying
                scope.launch {
                    val sources = NativeClient.listSources(addr)
                    if (step !is Step.Querying) return@launch // người dùng đã bấm Back
                    // Rỗng = host im lặng hoặc host đời cũ; một nguồn = không có gì để
                    // chọn. Cả hai trường hợp vào thẳng, để tầng dưới báo lỗi thật nếu có.
                    if (sources.size <= 1) {
                        step = Step.Address
                        onOpenStream(addr, sources.firstOrNull()?.id ?: 0)
                    } else {
                        step = Step.Picking(sources)
                    }
                }
            }
        )

        is Step.Querying -> CenteredMessage(stringResource(R.string.looking_for_sources), busy = true)

        is Step.Picking -> SourcePickerScreen(
            sources = s.sources,
            onPick = { source ->
                step = Step.Address // quay lại từ StreamActivity là thấy ô địa chỉ
                onOpenStream(address, source.id)
            }
        )
    }
}

@Composable
private fun AddressScreen(
    address: String,
    onAddressChange: (String) -> Unit,
    onConnect: (String) -> Unit
) {
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
            onValueChange = onAddressChange,
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

/**
 * Danh sách cửa sổ host đang chia sẻ. Đối ứng SourcePickerDialog bên client Windows,
 * nhưng chỉ cho chọn MỘT: Android chỉ xem được một nguồn tại một thời điểm (bên
 * Windows mỗi nguồn là một cửa sổ preview riêng, ở đây chỉ có một Activity).
 */
@Composable
private fun SourcePickerScreen(
    sources: List<NativeClient.Source>,
    onPick: (NativeClient.Source) -> Unit
) {
    Column(modifier = Modifier.fillMaxSize()) {
        Text(
            text = stringResource(R.string.pick_source),
            fontSize = 18.sp,
            color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 32.dp, bottom = 16.dp)
        )

        LazyColumn(modifier = Modifier.fillMaxWidth()) {
            items(sources, key = { it.id }) { source ->
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { onPick(source) }
                        .padding(horizontal = 24.dp, vertical = 14.dp)
                ) {
                    Text(
                        // Host cắt tên ở 64 byte và có cửa sổ không có tiêu đề.
                        text = source.name.ifBlank { stringResource(R.string.unnamed_source, source.id) },
                        fontSize = 16.sp,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Text(
                        text = stringResource(R.string.source_size, source.width, source.height),
                        fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                HorizontalDivider()
            }
        }
    }
}

@Composable
private fun CenteredMessage(text: String, busy: Boolean) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        if (busy) CircularProgressIndicator(modifier = Modifier.padding(bottom = 20.dp))
        Text(text = text, fontSize = 14.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}
