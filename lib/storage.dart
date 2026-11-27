// Biblioteca de jogos, saves e configurações (tudo em arquivos na pasta do app).
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:archive/archive.dart';

import 'platform.dart';

class Game {
  Game({
    required this.id,
    required this.title,
    required this.code,
    required this.size,
    required this.lastPlayed,
  });

  final String id;
  String title;
  String code;
  int size;
  int lastPlayed; // millisecondsSinceEpoch

  Map<String, dynamic> toJson() => {
        'id': id,
        'title': title,
        'code': code,
        'size': size,
        'lastPlayed': lastPlayed,
      };

  factory Game.fromJson(Map<String, dynamic> j) => Game(
        id: j['id'] as String,
        title: j['title'] as String? ?? 'Jogo',
        code: j['code'] as String? ?? '',
        size: (j['size'] as num?)?.toInt() ?? 0,
        lastPlayed: (j['lastPlayed'] as num?)?.toInt() ?? 0,
      );
}

class AppSettings {
  double controlsOpacity = 0.6;
  bool smooth = false;
  bool integerScale = false;
  bool haptics = true;
  int volume = 100;

  Map<String, dynamic> toJson() => {
        'controlsOpacity': controlsOpacity,
        'smooth': smooth,
        'integerScale': integerScale,
        'haptics': haptics,
        'volume': volume,
      };

  void applyJson(Map<String, dynamic> j) {
    controlsOpacity = ((j['controlsOpacity'] as num?)?.toDouble() ?? controlsOpacity).clamp(0.1, 1.0).toDouble();
    smooth = j['smooth'] as bool? ?? smooth;
    integerScale = j['integerScale'] as bool? ?? integerScale;
    haptics = j['haptics'] as bool? ?? haptics;
    volume = ((j['volume'] as num?)?.toInt() ?? volume).clamp(0, 100).toInt();
  }
}

class Store {
  Store._(this.root);

  final Directory root;
  final List<Game> games = [];
  final AppSettings settings = AppSettings();

  static Future<Store> open() async {
    final base = Directory(await HostPlatform.filesDir());
    final s = Store._(base);
    for (final d in ['roms', 'saves', 'states']) {
      await Directory('${base.path}/$d').create(recursive: true);
    }
    await s._load();
    return s;
  }

  File get _libFile => File('${root.path}/library.json');
  File get _settingsFile => File('${root.path}/settings.json');

  Future<void> _load() async {
    try {
      if (await _libFile.exists()) {
        final list = jsonDecode(await _libFile.readAsString()) as List<dynamic>;
        for (final e in list) {
          final g = Game.fromJson(e as Map<String, dynamic>);
          if (await romFile(g).exists()) games.add(g);
        }
      }
    } catch (_) {}
    try {
      if (await _settingsFile.exists()) {
        settings.applyJson(jsonDecode(await _settingsFile.readAsString()) as Map<String, dynamic>);
      }
    } catch (_) {}
  }

  Future<void> _writeAtomic(File f, String text) async {
    final tmp = File('${f.path}.tmp');
    await tmp.writeAsString(text, flush: true);
    await tmp.rename(f.path);
  }

  Future<void> saveLibrary() => _writeAtomic(_libFile, jsonEncode(games.map((g) => g.toJson()).toList()));
  Future<void> saveSettings() => _writeAtomic(_settingsFile, jsonEncode(settings.toJson()));

  File romFile(Game g) => File('${root.path}/roms/${g.id}.gba');
  File saveFile(Game g) => File('${root.path}/saves/${g.id}.sav');
  File stateFile(Game g, int slot) => File('${root.path}/states/${g.id}.s$slot');

  Future<Uint8List> readRom(Game g) => romFile(g).readAsBytes();

  List<Game> get sorted {
    final l = List<Game>.of(games);
    l.sort((a, b) => b.lastPlayed.compareTo(a.lastPlayed));
    return l;
  }

  Future<void> touch(Game g) async {
    g.lastPlayed = DateTime.now().millisecondsSinceEpoch;
    await saveLibrary();
  }

  Future<void> remove(Game g) async {
    games.remove(g);
    for (final f in [romFile(g), saveFile(g), stateFile(g, 1), stateFile(g, 2), stateFile(g, 3)]) {
      try {
        if (await f.exists()) await f.delete();
      } catch (_) {}
    }
    await saveLibrary();
  }

  /// Importa uma ROM (.gba/.agb/.bin ou .zip que contenha uma). Lança FormatException se inválida.
  Future<Game> importRom(String path, {String? name}) async {
    var bytes = Uint8List.fromList(await File(path).readAsBytes());
    var fileName = name ?? path.split(RegExp(r'[\\/]')).last;

    final isZip = bytes.length > 4 && bytes[0] == 0x50 && bytes[1] == 0x4B;
    if (isZip) {
      final archive = ZipDecoder().decodeBytes(bytes);
      ArchiveFile? found;
      for (final f in archive.files) {
        final n = f.name.toLowerCase();
        if (f.isFile && (n.endsWith('.gba') || n.endsWith('.agb') || n.endsWith('.bin'))) {
          found = f;
          break;
        }
      }
      if (found == null) throw const FormatException('Não encontrei nenhuma ROM .gba dentro do zip.');
      final dynamic content = found.content;
      bytes = Uint8List.fromList(List<int>.from(content as List));
      fileName = found.name.split('/').last;
    }

    if (bytes.length < 0xC0) throw const FormatException('Arquivo pequeno demais para ser uma ROM de GBA.');
    if (bytes.length > 0x2000000) throw const FormatException('ROM maior que 32 MB.');

    // id = hash FNV-1a do conteúdo + tamanho
    var h = 0x811C9DC5;
    for (var i = 0; i < bytes.length; i++) {
      h ^= bytes[i];
      h = (h * 0x01000193) & 0xFFFFFFFF;
    }
    final id = '${h.toRadixString(16).padLeft(8, '0')}${bytes.length.toRadixString(16)}';

    for (final g in games) {
      if (g.id == id) return g; // já existe
    }

    String ascii(int from, int to) {
      final sb = StringBuffer();
      for (var i = from; i < to; i++) {
        final c = bytes[i];
        if (c == 0) break;
        if (c >= 32 && c < 127) sb.writeCharCode(c);
      }
      return sb.toString().trim();
    }

    var title = ascii(0xA0, 0xAC);
    if (title.length < 3) {
      title = fileName.replaceFirst(RegExp(r'\.[^.]*$'), '');
    }
    final game = Game(
      id: id,
      title: title,
      code: ascii(0xAC, 0xB0),
      size: bytes.length,
      lastPlayed: DateTime.now().millisecondsSinceEpoch,
    );
    await romFile(game).writeAsBytes(bytes, flush: true);
    games.add(game);
    await saveLibrary();
    return game;
  }
}
