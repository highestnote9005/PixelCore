// Funções do Android feitas direto no MainActivity (sem plugins de terceiros):
// escolher arquivo, pasta de dados do app e manter a tela ligada.
import 'package:flutter/services.dart';

class PickedFile {
  PickedFile(this.path, this.name);
  final String path;
  final String name;
}

class HostPlatform {
  static const MethodChannel _ch = MethodChannel('pocketgba/platform');

  static Future<String> filesDir() async => (await _ch.invokeMethod<String>('filesDir'))!;

  static Future<PickedFile?> pickFile() async {
    final r = await _ch.invokeMapMethod<String, String>('pickFile');
    if (r == null) return null;
    return PickedFile(r['path']!, r['name'] ?? 'rom.gba');
  }

  static Future<void> keepScreenOn(bool on) async {
    try {
      await _ch.invokeMethod<void>('keepScreenOn', on);
    } catch (_) {}
  }
}
