// Ponte FFI para o núcleo em C (native/host.c).
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

const int gbaWidth = 240;
const int gbaHeight = 160;
const int _frameBytes = gbaWidth * gbaHeight * 4;
const int _saveCap = 0x20000;

/// Bits das teclas (iguais aos de gba.h).
class Keys {
  static const int a = 1;
  static const int b = 2;
  static const int select = 4;
  static const int start = 8;
  static const int right = 16;
  static const int left = 32;
  static const int up = 64;
  static const int down = 128;
  static const int r = 256;
  static const int l = 512;
}

class GbaNative {
  GbaNative._() {
    final lib = DynamicLibrary.open('libpocketgba.so');
    _create = lib.lookupFunction<Pointer<Void> Function(), Pointer<Void> Function()>('host_create');
    _loadRom = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32),
        int Function(Pointer<Void>, Pointer<Uint8>, int)>('host_load_rom');
    _reset = lib.lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('host_reset');
    _setKeys = lib.lookupFunction<Void Function(Pointer<Void>, Uint32),
        void Function(Pointer<Void>, int)>('host_set_keys');
    _setPaused = lib.lookupFunction<Void Function(Pointer<Void>, Int32),
        void Function(Pointer<Void>, int)>('host_set_paused');
    _setSpeed = lib.lookupFunction<Void Function(Pointer<Void>, Int32),
        void Function(Pointer<Void>, int)>('host_set_speed');
    _setVolume = lib.lookupFunction<Void Function(Pointer<Void>, Int32),
        void Function(Pointer<Void>, int)>('host_set_volume');
    _frameId = lib.lookupFunction<Uint32 Function(Pointer<Void>), int Function(Pointer<Void>)>('host_frame_id');
    _copyFrame = lib.lookupFunction<Uint32 Function(Pointer<Void>, Pointer<Uint8>),
        int Function(Pointer<Void>, Pointer<Uint8>)>('host_copy_frame');
    _saveSize = lib.lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('host_save_size');
    _takeSave = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32),
        int Function(Pointer<Void>, Pointer<Uint8>, int)>('host_take_save');
    _copySave = lib.lookupFunction<Void Function(Pointer<Void>, Pointer<Uint8>, Int32),
        void Function(Pointer<Void>, Pointer<Uint8>, int)>('host_copy_save');
    _loadSave = lib.lookupFunction<Void Function(Pointer<Void>, Pointer<Uint8>, Int32),
        void Function(Pointer<Void>, Pointer<Uint8>, int)>('host_load_save');
    _stateSize = lib.lookupFunction<Int32 Function(), int Function()>('host_state_size');
    _stateSave = lib.lookupFunction<Void Function(Pointer<Void>, Pointer<Uint8>),
        void Function(Pointer<Void>, Pointer<Uint8>)>('host_state_save');
    _stateLoad = lib.lookupFunction<Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32),
        int Function(Pointer<Void>, Pointer<Uint8>, int)>('host_state_load');
    _audioOk = lib.lookupFunction<Int32 Function(Pointer<Void>), int Function(Pointer<Void>)>('host_audio_ok');

    _h = _create();
    if (_h == nullptr) throw StateError('host_create falhou');
    _frame = malloc<Uint8>(_frameBytes);
    _saveBuf = malloc<Uint8>(_saveCap);
  }

  /// Instância única (criada na primeira vez que é usada).
  static final GbaNative instance = GbaNative._();

  late final Pointer<Void> Function() _create;
  late final int Function(Pointer<Void>, Pointer<Uint8>, int) _loadRom;
  late final void Function(Pointer<Void>) _reset;
  late final void Function(Pointer<Void>, int) _setKeys;
  late final void Function(Pointer<Void>, int) _setPaused;
  late final void Function(Pointer<Void>, int) _setSpeed;
  late final void Function(Pointer<Void>, int) _setVolume;
  late final int Function(Pointer<Void>) _frameId;
  late final int Function(Pointer<Void>, Pointer<Uint8>) _copyFrame;
  late final int Function(Pointer<Void>) _saveSize;
  late final int Function(Pointer<Void>, Pointer<Uint8>, int) _takeSave;
  late final void Function(Pointer<Void>, Pointer<Uint8>, int) _copySave;
  late final void Function(Pointer<Void>, Pointer<Uint8>, int) _loadSave;
  late final int Function() _stateSize;
  late final void Function(Pointer<Void>, Pointer<Uint8>) _stateSave;
  late final int Function(Pointer<Void>, Pointer<Uint8>, int) _stateLoad;
  late final int Function(Pointer<Void>) _audioOk;

  late final Pointer<Void> _h;
  late final Pointer<Uint8> _frame;
  late final Pointer<Uint8> _saveBuf;

  /// Carrega a ROM. Retorna 0 em caso de sucesso.
  int loadRom(Uint8List rom) {
    final p = malloc<Uint8>(rom.length);
    try {
      p.asTypedList(rom.length).setAll(0, rom);
      return _loadRom(_h, p, rom.length);
    } finally {
      malloc.free(p);
    }
  }

  void reset() => _reset(_h);
  void setKeys(int mask) => _setKeys(_h, mask);
  void setPaused(bool paused) => _setPaused(_h, paused ? 1 : 0);
  void setSpeed(int speed) => _setSpeed(_h, speed);
  void setVolume(int volume) => _setVolume(_h, volume);
  bool get audioOk => _audioOk(_h) != 0;

  int get frameId => _frameId(_h);

  /// Copia o último quadro (RGBA 240x160) e devolve o id dele.
  int copyFrame() => _copyFrame(_h, _frame);

  /// Visão (sem cópia) do buffer do último quadro copiado.
  Uint8List get frameBytes => _frame.asTypedList(_frameBytes);

  /// Devolve o save da bateria se mudou desde a última chamada; senão null.
  Uint8List? takeSave() {
    if (_takeSave(_h, _saveBuf, _saveCap) == 0) return null;
    final n = _saveSize(_h).clamp(0, _saveCap).toInt();
    return Uint8List.fromList(_saveBuf.asTypedList(n));
  }

  /// Devolve sempre o save atual.
  Uint8List copySave() {
    _copySave(_h, _saveBuf, _saveCap);
    final n = _saveSize(_h).clamp(0, _saveCap).toInt();
    return Uint8List.fromList(_saveBuf.asTypedList(n));
  }

  void loadSave(Uint8List data) {
    final n = data.length > _saveCap ? _saveCap : data.length;
    _saveBuf.asTypedList(n).setAll(0, data.sublist(0, n));
    _loadSave(_h, _saveBuf, n);
  }

  Uint8List saveState() {
    final n = _stateSize();
    final p = malloc<Uint8>(n);
    try {
      _stateSave(_h, p);
      return Uint8List.fromList(p.asTypedList(n));
    } finally {
      malloc.free(p);
    }
  }

  bool loadState(Uint8List data) {
    final p = malloc<Uint8>(data.length);
    try {
      p.asTypedList(data.length).setAll(0, data);
      return _stateLoad(_h, p, data.length) == 0;
    } finally {
      malloc.free(p);
    }
  }
}
