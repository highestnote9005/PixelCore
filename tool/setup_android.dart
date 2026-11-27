// Prepara a pasta android/ do projeto:
//   1. gera o esqueleto com `flutter create` (se ainda não existir),
//   2. liga o núcleo em C (native/CMakeLists.txt) ao Gradle,
//   3. ajusta o nome do app e copia os ícones.
//
// Uso (na raiz do projeto):  dart run tool/setup_android.dart
import 'dart:io';

Future<void> main() async {
  if (!File('pubspec.yaml').existsSync() || !Directory('native').existsSync()) {
    stderr.writeln('Rode este script na raiz do projeto PocketGBA (onde está o pubspec.yaml).');
    exit(1);
  }

  if (!Directory('android').existsSync()) {
    stdout.writeln('> flutter create (gerando a pasta android)...');
    final r = await Process.run(
      'flutter',
      ['create', '--platforms=android', '--project-name=pocketgba', '--org=app.pocketgba', '.'],
      runInShell: true,
    );
    stdout.write(r.stdout);
    stderr.write(r.stderr);
    if (r.exitCode != 0) {
      stderr.writeln('flutter create falhou (código ${r.exitCode}).');
      exit(r.exitCode);
    }
  }

  // O teste de exemplo que o flutter create cria referencia um app que não existe aqui.
  final sample = File('test/widget_test.dart');
  if (sample.existsSync() && RegExp(r'MyApp|pixelcore|path_provider').hasMatch(sample.readAsStringSync())) {
    sample.deleteSync();
  }

  _patchGradle();
  _patchManifest();
  _writeMainActivity();
  _copyIcons();
  _verify();
  stdout.writeln('\nPronto. Agora rode:  flutter pub get && flutter build apk --release');
}

void _patchGradle() {
  final kts = File('android/app/build.gradle.kts');
  final groovy = File('android/app/build.gradle');
  final isKts = kts.existsSync();
  final f = isKts ? kts : groovy;
  if (!f.existsSync()) {
    stderr.writeln('Não achei android/app/build.gradle(.kts).');
    exit(1);
  }
  var text = f.readAsStringSync();
  if (text.contains('native/CMakeLists.txt')) {
    stdout.writeln('> Gradle já configurado.');
    return;
  }

  final native = isKts
      ? '    externalNativeBuild {\n'
          '        cmake {\n'
          '            path = file("../../native/CMakeLists.txt")\n'
          '        }\n'
          '    }\n\n'
      : '    externalNativeBuild {\n'
          '        cmake {\n'
          '            path "../../native/CMakeLists.txt"\n'
          '        }\n'
          '    }\n\n';
  final abi = isKts
      ? '        ndk {\n            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")\n        }\n'
      : '        ndk {\n            abiFilters "arm64-v8a", "armeabi-v7a", "x86_64"\n        }\n';

  final defaultConfig = RegExp(r'^(\s*)defaultConfig\s*\{[ \t]*$', multiLine: true);
  final m = defaultConfig.firstMatch(text);
  if (m == null) {
    stderr.writeln('Não achei o bloco defaultConfig em ${f.path}. Veja o README para o ajuste manual.');
    exit(1);
  }
  // 1) abiFilters dentro do defaultConfig
  text = text.replaceRange(m.end, m.end, '\n$abi');
  // 2) externalNativeBuild logo antes do defaultConfig
  final m2 = defaultConfig.firstMatch(text)!;
  text = text.replaceRange(m2.start, m2.start, native);
  f.writeAsStringSync(text);
  stdout.writeln('> ${f.path} atualizado (CMake + ABIs).');
}

void _patchManifest() {
  final f = File('android/app/src/main/AndroidManifest.xml');
  if (!f.existsSync()) return;
  var t = f.readAsStringSync();
  t = t.replaceFirst(RegExp(r'android:label="[^"]*"'), 'android:label="PocketGBA"');
  f.writeAsStringSync(t);
  stdout.writeln('> Nome do app definido como PocketGBA.');
}

void _copyIcons() {
  for (final dpi in ['mdpi', 'hdpi', 'xhdpi', 'xxhdpi', 'xxxhdpi']) {
    final src = File('tool/icons/$dpi/ic_launcher.png');
    final dstDir = Directory('android/app/src/main/res/mipmap-$dpi');
    if (!src.existsSync() || !dstDir.existsSync()) continue;
    src.copySync('${dstDir.path}/ic_launcher.png');
  }
  stdout.writeln('> Ícones copiados.');
}

const _mainActivity = r"""package __PKG__

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
""";

void _writeMainActivity() {
  final root = Directory('android/app/src/main');
  File? existing;
  if (root.existsSync()) {
    for (final e in root.listSync(recursive: true)) {
      if (e is File && (e.path.endsWith('/MainActivity.kt') || e.path.endsWith('\\MainActivity.kt') ||
          e.path.endsWith('/MainActivity.java') || e.path.endsWith('\\MainActivity.java'))) {
        existing = e;
        break;
      }
    }
  }
  var pkg = 'app.pocketgba.pocketgba';
  if (existing != null) {
    final m = RegExp(r'^\s*package\s+([\w.]+)', multiLine: true).firstMatch(existing.readAsStringSync());
    if (m != null) pkg = m.group(1)!;
  }
  final target = File('android/app/src/main/kotlin/${pkg.replaceAll('.', '/')}/MainActivity.kt');
  target.parent.createSync(recursive: true);
  target.writeAsStringSync(_mainActivity.replaceAll('__PKG__', pkg));
  if (existing != null && existing.path != target.path) existing.deleteSync();
  stdout.writeln('> MainActivity.kt escrito ($pkg).');
}

void _verify() {
  final kts = File('android/app/build.gradle.kts');
  final f = kts.existsSync() ? kts : File('android/app/build.gradle');
  final ok = f.existsSync() && f.readAsStringSync().contains('native/CMakeLists.txt');
  if (!ok) {
    stderr.writeln('\nERRO: o Gradle NÃO está ligado ao núcleo em C. O app abriria, mas sem emulador.');
    stderr.writeln('Adicione manualmente em ${f.path}, dentro de android { }:');
    stderr.writeln('  externalNativeBuild { cmake { path = file("../../native/CMakeLists.txt") } }');
    exit(1);
  }
  stdout.writeln('> Verificado: Gradle aponta para native/CMakeLists.txt.');
}
