// =============================================================================
// KeyInputView.kt — View vô hình hứng phím từ bàn phím ảo cho màn hình xem.
//
// NHIỆM VỤ
//   Android không cho "mở bàn phím rồi nghe phím" tùy tiện: IME chỉ gửi input cho
//   view đang focus có InputConnection. View này giữ focus và bắt phím trên CẢ HAI
//   đường mà một IME có thể dùng — vì mỗi IME chọn một kiểu, không đường nào một
//   mình là đủ:
//     1. commitText/deleteSurroundingText — Gboard và đa số IME hiện đại commit
//        từng ký tự qua InputConnection, KHÔNG gửi KeyEvent (đây là lý do bản đầu
//        dùng TYPE_NULL bị câm: kiểu đó trông chờ KeyEvent mà Gboard không gửi).
//     2. KeyEvent (onKeyDown) — bàn phím vật lý/Bluetooth và các phím IME vẫn gửi
//        dạng event thô: DEL, ENTER...
//
// VÌ SAO VISIBLE_PASSWORD + NO_SUGGESTIONS
//   Kiểu ô mật khẩu-hiện-chữ khiến IME tắt gợi ý từ, tắt bộ gõ đa ngôn ngữ và
//   commit NGAY từng phím thay vì compose cả từ rồi mới commit — thứ cần gửi sang
//   host là PHÍM, không phải văn bản đã qua bộ gõ. Ký tự được giao cho tầng C++
//   quy đổi sang VK (KeyMap.h, layout US); ký tự ngoài ASCII bị bỏ qua ở đó.
//
// LIÊN QUAN: StreamActivity.kt (nơi gắn view + toggle IME), NativeClient.nativeCharTap
// =============================================================================
package com.deskhub.app

import android.content.Context
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection

/** View 1px vô hình: giữ focus để IME gửi phím, chuyển từng phím ra [onChar]. */
class KeyInputView(context: Context) : View(context) {
    /** Nhận codepoint của phím vừa gõ ('\b' = backspace, '\n' = enter). */
    var onChar: ((Int) -> Unit)? = null

    init {
        isFocusable = true
        isFocusableInTouchMode = true
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE or
            EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI

        // fullEditor = false: chế độ "dummy" — không có buffer văn bản thật phía sau,
        // và các phím không phải chữ (DEL, ENTER) được BaseInputConnection tự đổi
        // thành KeyEvent rơi xuống onKeyDown bên dưới.
        return object : BaseInputConnection(this, false) {
            override fun commitText(
                text: CharSequence,
                newCursorPosition: Int,
            ): Boolean {
                for (ch in text) onChar?.invoke(ch.code)
                return true
            }

            // Gboard xoá bằng deleteSurroundingText khi nó nghĩ "ô nhập" có chữ.
            override fun deleteSurroundingText(
                beforeLength: Int,
                afterLength: Int,
            ): Boolean {
                repeat(beforeLength) { onChar?.invoke('\b'.code) }
                return true
            }
        }
    }

    override fun onKeyDown(
        keyCode: Int,
        event: KeyEvent,
    ): Boolean {
        // DEL không có unicodeChar -> tự quy về '\b'; KeyMap bên C++ hiểu ký tự này.
        if (keyCode == KeyEvent.KEYCODE_DEL) {
            onChar?.invoke('\b'.code)
            return true
        }
        val ch = event.unicodeChar
        if (ch != 0) {
            onChar?.invoke(ch)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }
}
