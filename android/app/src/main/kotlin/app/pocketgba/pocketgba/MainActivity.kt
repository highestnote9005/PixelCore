package app.pocketgba.pocketgba

import android.app.Activity
import android.content.Intent
import android.provider.OpenableColumns
import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.File
import kotlin.concurrent.thread

class MainActivity : FlutterActivity() {
    private var pending: MethodChannel.Result? = null
    private val pickCode = 4711

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, "pocketgba/platform")
            .setMethodCallHandler { call, result ->
                when (call.method) {
                    "filesDir" -> result.success(filesDir.absolutePath)
                    "keepScreenOn" -> {
                        val on = call.arguments as? Boolean ?: false
                        if (on) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                        result.success(null)
                    }
                    "pickFile" -> {
                        if (pending != null) {
                            result.error("busy", "O seletor de arquivos já está aberto", null)
                        } else {
                            pending = result
                            val i = Intent(Intent.ACTION_OPEN_DOCUMENT)
                            i.addCategory(Intent.CATEGORY_OPENABLE)
                            i.type = "*/*"
                            startActivityForResult(i, pickCode)
                        }
                    }
                    else -> result.notImplemented()
                }
            }
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (requestCode != pickCode) {
            super.onActivityResult(requestCode, resultCode, data)
            return
        }
        val res = pending ?: return
        pending = null
        val uri = data?.data
        if (resultCode != Activity.RESULT_OK || uri == null) {
            res.success(null)
            return
        }
        thread {
            try {
                var name = "rom.gba"
                contentResolver.query(uri, null, null, null, null)?.use { c ->
                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0 && c.moveToFirst()) name = c.getString(idx) ?: name
                }
                val out = File(cacheDir, "import_" + System.currentTimeMillis())
                val input = contentResolver.openInputStream(uri) ?: throw IllegalStateException("Não consegui ler o arquivo")
                input.use { i -> out.outputStream().use { o -> i.copyTo(o) } }
                runOnUiThread { res.success(mapOf("path" to out.absolutePath, "name" to name)) }
            } catch (e: Exception) {
                runOnUiThread { res.error("io", e.message ?: "erro ao ler o arquivo", null) }
            }
        }
    }
}
