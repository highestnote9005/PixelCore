// Tela do emulador: imagem, controles, teclado/gamepad, saves e menu.
import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

import 'controls.dart';
import 'native.dart';
import 'platform.dart';
import 'storage.dart';

enum _MenuAction { resume, exit }

class EmulatorPage extends StatefulWidget {
  const EmulatorPage({super.key, required this.store, required this.game});

  final Store store;
  final Game game;

  @override
  State<EmulatorPage> createState() => _EmulatorPageState();
}

class _EmulatorPageState extends State<EmulatorPage> with SingleTickerProviderStateMixin, WidgetsBindingObserver {
  late final GbaNative _n;
  Ticker? _ticker;
  Timer? _saveTimer;
  final ValueNotifier<ui.Image?> _frame = ValueNotifier<ui.Image?>(null);

  int _touchKeys = 0;
  int _kbKeys = 0;
  int _lastId = -1;
  int _speed = 1;
  bool _decoding = false;
  bool _ready = false;
  bool _menuOpen = false;
  String? _error;

  AppSettings get s => widget.store.settings;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    HostPlatform.keepScreenOn(true);
    _start();
  }

  Future<void> _start() async {
    try {
      _n = GbaNative.instance;
      final rom = await widget.store.readRom(widget.game);
      final rc = _n.loadRom(rom);
      if (rc != 0) throw 'ROM inválida (código $rc)';
      final sf = widget.store.saveFile(widget.game);
      if (await sf.exists()) _n.loadSave(await sf.readAsBytes());
      if (!mounted) return;
      _n.setVolume(s.volume);
      _n.setSpeed(1);
      _n.setKeys(0);
      _n.setPaused(false);
      _ticker = createTicker(_onTick)..start();
      _saveTimer = Timer.periodic(const Duration(seconds: 2), (_) => _flushSave(false));
      setState(() => _ready = true);
    } catch (e) {
      var msg = '$e';
      if (msg.contains('libpocketgba')) {
        msg += '\n\nO núcleo nativo não entrou no APK. Rode "dart run tool/setup_android.dart" '
            'na raiz do projeto (ele precisa ligar native/CMakeLists.txt ao Gradle) e gere o APK de novo.';
      }
      if (mounted) setState(() => _error = msg);
    }
  }

  void _onTick(Duration _) {
    if (_decoding) return;
    final id = _n.frameId;
    if (id == _lastId) return;
    _lastId = _n.copyFrame();
    _decoding = true;
    ui.decodeImageFromPixels(_n.frameBytes, gbaWidth, gbaHeight, ui.PixelFormat.rgba8888, (img) {
      _decoding = false;
      if (!mounted) {
        img.dispose();
        return;
      }
      final old = _frame.value;
      _frame.value = img;
      old?.dispose();
    });
  }

  Future<void> _flushSave(bool force) async {
    if (!_ready) return;
    final Uint8List? data = force ? _n.copySave() : _n.takeSave();
    if (data == null) return;
    try {
      final f = widget.store.saveFile(widget.game);
      final tmp = File('${f.path}.tmp');
      await tmp.writeAsBytes(data, flush: true);
      await tmp.rename(f.path);
    } catch (_) {}
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (!_ready) return;
    if (state == AppLifecycleState.resumed) {
      if (!_menuOpen) _n.setPaused(false);
    } else {
      _n.setKeys(0);
      _n.setPaused(true);
      _flushSave(true);
    }
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _ticker?.dispose();
    _saveTimer?.cancel();
    if (_ready) {
      _n.setKeys(0);
      _n.setPaused(true);
      _flushSave(true);
    }
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.edgeToEdge);
    HostPlatform.keepScreenOn(false);
    _frame.value?.dispose();
    super.dispose();
  }

  // ---------------------------------------------------------------- entrada

  void _pushKeys() => _n.setKeys(_touchKeys | _kbKeys);

  void _onTouchKeys(int m) {
    final added = m & ~_touchKeys;
    _touchKeys = m;
    if (added != 0 && s.haptics) HapticFeedback.selectionClick();
    if (_ready) _pushKeys();
  }

  int _keyBit(LogicalKeyboardKey k) {
    if (k == LogicalKeyboardKey.arrowUp) return Keys.up;
    if (k == LogicalKeyboardKey.arrowDown) return Keys.down;
    if (k == LogicalKeyboardKey.arrowLeft) return Keys.left;
    if (k == LogicalKeyboardKey.arrowRight) return Keys.right;
    if (k == LogicalKeyboardKey.keyZ || k == LogicalKeyboardKey.gameButtonA) return Keys.a;
    if (k == LogicalKeyboardKey.keyX || k == LogicalKeyboardKey.gameButtonB) return Keys.b;
    if (k == LogicalKeyboardKey.keyA || k == LogicalKeyboardKey.gameButtonLeft1) return Keys.l;
    if (k == LogicalKeyboardKey.keyS || k == LogicalKeyboardKey.gameButtonRight1) return Keys.r;
    if (k == LogicalKeyboardKey.enter || k == LogicalKeyboardKey.gameButtonStart) return Keys.start;
    if (k == LogicalKeyboardKey.backspace ||
        k == LogicalKeyboardKey.shiftRight ||
        k == LogicalKeyboardKey.gameButtonSelect) {
      return Keys.select;
    }
    return 0;
  }

  KeyEventResult _onKey(FocusNode node, KeyEvent event) {
    final bit = _keyBit(event.logicalKey);
    if (bit == 0) return KeyEventResult.ignored;
    if (event is KeyDownEvent) {
      _kbKeys |= bit;
    } else if (event is KeyUpEvent) {
      _kbKeys &= ~bit;
    }
    if (_ready) _pushKeys();
    return KeyEventResult.handled;
  }

  // ------------------------------------------------------------------ menu

  void _applySettings() {
    if (_ready) _n.setVolume(s.volume);
    widget.store.saveSettings();
    if (mounted) setState(() {});
  }

  void _setSpeed(int v) {
    _speed = v;
    _n.setSpeed(v);
  }

  Future<String> _saveSlot(int slot) async {
    try {
      final data = _n.saveState();
      await widget.store.stateFile(widget.game, slot).writeAsBytes(data, flush: true);
      return 'Estado $slot salvo';
    } catch (e) {
      return 'Falha ao salvar: $e';
    }
  }

  Future<String> _loadSlot(int slot) async {
    try {
      final f = widget.store.stateFile(widget.game, slot);
      if (!await f.exists()) return 'Slot $slot está vazio';
      final ok = _n.loadState(await f.readAsBytes());
      return ok ? 'Estado $slot carregado' : 'Estado incompatível com esta versão';
    } catch (e) {
      return 'Falha ao carregar: $e';
    }
  }

  Future<void> _openMenu() async {
    if (_menuOpen || !_ready) return;
    _menuOpen = true;
    _n.setPaused(true);
    await _flushSave(true);
    if (!mounted) return;
    final action = await showModalBottomSheet<_MenuAction>(
      context: context,
      isScrollControlled: true,
      useSafeArea: true,
      backgroundColor: const Color(0xFF1B1730),
      builder: (ctx) => _MenuSheet(page: this),
    );
    _menuOpen = false;
    if (!mounted) return;
    if (action == _MenuAction.exit) {
      Navigator.of(context).pop();
      return;
    }
    _n.setPaused(false);
  }

  // ----------------------------------------------------------------- build

  @override
  Widget build(BuildContext context) {
    if (_error != null) {
      return Scaffold(
        appBar: AppBar(title: const Text('Erro')),
        body: Center(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Text(_error!, textAlign: TextAlign.center),
          ),
        ),
      );
    }
    if (!_ready) {
      return const Scaffold(backgroundColor: Colors.black, body: Center(child: CircularProgressIndicator()));
    }

    final screen = RepaintBoundary(
      child: CustomPaint(
        painter: _ScreenPainter(_frame, s.smooth, s.integerScale),
        size: Size.infinite,
      ),
    );

    final menuButton = SafeArea(
      child: Align(
        alignment: Alignment.topCenter,
        child: Padding(
          padding: const EdgeInsets.only(top: 4),
          child: Material(
            color: const Color(0x55000000),
            shape: const CircleBorder(),
            child: InkWell(
              customBorder: const CircleBorder(),
              onTap: _openMenu,
              child: const Padding(
                padding: EdgeInsets.all(8),
                child: Icon(Icons.menu, color: Colors.white70, size: 22),
              ),
            ),
          ),
        ),
      ),
    );

    return PopScope(
      canPop: false,
      onPopInvokedWithResult: (didPop, result) {
        if (!didPop) _openMenu();
      },
      child: Scaffold(
        backgroundColor: Colors.black,
        body: Focus(
          autofocus: true,
          onKeyEvent: _onKey,
          child: LayoutBuilder(builder: (context, cons) {
            final landscape = cons.maxWidth > cons.maxHeight;
            if (landscape) {
              return Stack(children: [
                Positioned.fill(
                  child: Center(child: AspectRatio(aspectRatio: gbaWidth / gbaHeight, child: screen)),
                ),
                Positioned.fill(
                  child: SafeArea(
                    child: TouchControls(landscape: true, opacity: s.controlsOpacity, onKeys: _onTouchKeys),
                  ),
                ),
                Positioned.fill(child: menuButton),
              ]);
            }
            return Stack(children: [
              Column(children: [
                SafeArea(
                  bottom: false,
                  child: AspectRatio(aspectRatio: gbaWidth / gbaHeight, child: screen),
                ),
                Expanded(
                  child: TouchControls(landscape: false, opacity: s.controlsOpacity, onKeys: _onTouchKeys),
                ),
              ]),
              Positioned.fill(child: menuButton),
            ]);
          }),
        ),
      ),
    );
  }
}

class _ScreenPainter extends CustomPainter {
  _ScreenPainter(this.frame, this.smooth, this.integer) : super(repaint: frame);

  final ValueListenable<ui.Image?> frame;
  final bool smooth;
  final bool integer;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = Colors.black);
    final img = frame.value;
    if (img == null) return;
    var scale = math.min(size.width / gbaWidth, size.height / gbaHeight);
    if (integer && scale >= 1) scale = scale.floorToDouble();
    final w = gbaWidth * scale;
    final h = gbaHeight * scale;
    final dst = Rect.fromLTWH((size.width - w) / 2, (size.height - h) / 2, w, h);
    final paint = Paint()
      ..filterQuality = smooth ? FilterQuality.medium : FilterQuality.none
      ..isAntiAlias = false;
    canvas.drawImageRect(img, const Rect.fromLTWH(0, 0, 240, 160), dst, paint);
  }

  @override
  bool shouldRepaint(_ScreenPainter old) => old.smooth != smooth || old.integer != integer;
}

class _MenuSheet extends StatefulWidget {
  const _MenuSheet({required this.page});

  final _EmulatorPageState page;

  @override
  State<_MenuSheet> createState() => _MenuSheetState();
}

class _MenuSheetState extends State<_MenuSheet> {
  String? _status;

  _EmulatorPageState get p => widget.page;

  Future<void> _run(Future<String> Function() job) async {
    final msg = await job();
    if (mounted) setState(() => _status = msg);
  }

  @override
  Widget build(BuildContext context) {
    final s = p.s;
    return SafeArea(
      child: SingleChildScrollView(
        padding: const EdgeInsets.fromLTRB(20, 16, 20, 20),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(p.widget.game.title, style: Theme.of(context).textTheme.titleLarge),
            const SizedBox(height: 4),
            Text(_status ?? 'Jogo pausado', style: Theme.of(context).textTheme.bodySmall),
            const SizedBox(height: 14),
            Row(children: [
              Expanded(
                child: FilledButton.icon(
                  onPressed: () => Navigator.of(context).pop(_MenuAction.resume),
                  icon: const Icon(Icons.play_arrow),
                  label: const Text('Continuar'),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: FilledButton.tonalIcon(
                  onPressed: () {
                    p._n.reset();
                    Navigator.of(context).pop(_MenuAction.resume);
                  },
                  icon: const Icon(Icons.restart_alt),
                  label: const Text('Reiniciar'),
                ),
              ),
            ]),
            const SizedBox(height: 18),
            Text('Estados salvos', style: Theme.of(context).textTheme.titleSmall),
            const SizedBox(height: 8),
            Row(children: [
              for (var i = 1; i <= 3; i++) ...[
                Expanded(
                  child: OutlinedButton(
                    onPressed: () => _run(() => p._saveSlot(i)),
                    child: Text('Salvar $i'),
                  ),
                ),
                if (i < 3) const SizedBox(width: 8),
              ],
            ]),
            const SizedBox(height: 8),
            Row(children: [
              for (var i = 1; i <= 3; i++) ...[
                Expanded(
                  child: OutlinedButton(
                    onPressed: () => _run(() => p._loadSlot(i)),
                    child: Text('Carregar $i'),
                  ),
                ),
                if (i < 3) const SizedBox(width: 8),
              ],
            ]),
            const SizedBox(height: 18),
            Text('Velocidade', style: Theme.of(context).textTheme.titleSmall),
            const SizedBox(height: 8),
            SegmentedButton<int>(
              segments: const [
                ButtonSegment<int>(value: 1, label: Text('1x')),
                ButtonSegment<int>(value: 2, label: Text('2x')),
                ButtonSegment<int>(value: 4, label: Text('4x')),
              ],
              selected: {p._speed},
              onSelectionChanged: (v) {
                p._setSpeed(v.first);
                setState(() {});
              },
            ),
            const SizedBox(height: 14),
            Text('Volume: ${s.volume}%', style: Theme.of(context).textTheme.titleSmall),
            Slider(
              value: s.volume.toDouble(),
              min: 0,
              max: 100,
              divisions: 20,
              onChanged: (v) {
                s.volume = v.round();
                p._applySettings();
                setState(() {});
              },
            ),
            Text('Opacidade dos controles', style: Theme.of(context).textTheme.titleSmall),
            Slider(
              value: s.controlsOpacity,
              min: 0.1,
              max: 1.0,
              onChanged: (v) {
                s.controlsOpacity = v;
                p._applySettings();
                setState(() {});
              },
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Suavizar imagem'),
              value: s.smooth,
              onChanged: (v) {
                s.smooth = v;
                p._applySettings();
                setState(() {});
              },
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Escala inteira (pixels nítidos)'),
              value: s.integerScale,
              onChanged: (v) {
                s.integerScale = v;
                p._applySettings();
                setState(() {});
              },
            ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Vibração nos botões'),
              value: s.haptics,
              onChanged: (v) {
                s.haptics = v;
                p._applySettings();
                setState(() {});
              },
            ),
            const SizedBox(height: 8),
            SizedBox(
              width: double.infinity,
              child: OutlinedButton.icon(
                onPressed: () => Navigator.of(context).pop(_MenuAction.exit),
                icon: const Icon(Icons.logout),
                label: const Text('Sair para a biblioteca'),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
