// Controles de toque multitoque: D-pad, A, B, L, R, Start e Select.
// Uma única área captura todos os dedos, então dá para deslizar de A para B,
// apertar vários botões ao mesmo tempo, etc.
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import 'native.dart';

class ControlLayout {
  ControlLayout._({
    required this.dpadC,
    required this.dpadR,
    required this.aC,
    required this.bC,
    required this.btnR,
    required this.lRect,
    required this.rRect,
    required this.startRect,
    required this.selectRect,
  });

  factory ControlLayout(Size size, bool landscape) {
    final w = size.width;
    final h = size.height;
    if (landscape) {
      final dpadR = h * 0.21;
      final btnR = h * 0.10;
      final dx = dpadR * 1.25 > w * 0.12 ? dpadR * 1.25 : w * 0.12;
      final ax = w - (btnR * 1.5 > w * 0.07 ? btnR * 1.5 : w * 0.07);
      final aC = Offset(ax, h * 0.50);
      return ControlLayout._(
        dpadC: Offset(dx, h * 0.60),
        dpadR: dpadR,
        aC: aC,
        bC: Offset(aC.dx - btnR * 2.4, h * 0.68),
        btnR: btnR,
        lRect: Rect.fromLTWH(w * 0.02, h * 0.04, w * 0.13, h * 0.12),
        rRect: Rect.fromLTWH(w * 0.85, h * 0.04, w * 0.13, h * 0.12),
        selectRect: Rect.fromCenter(center: Offset(w * 0.10, h * 0.93), width: w * 0.10, height: h * 0.07),
        startRect: Rect.fromCenter(center: Offset(w * 0.90, h * 0.93), width: w * 0.10, height: h * 0.07),
      );
    }
    final dpadR = (w * 0.20 < h * 0.30) ? w * 0.20 : h * 0.30;
    final btnR = (w * 0.09 < h * 0.14) ? w * 0.09 : h * 0.14;
    return ControlLayout._(
      dpadC: Offset(w * 0.25, h * 0.55),
      dpadR: dpadR,
      aC: Offset(w * 0.86, h * 0.46),
      bC: Offset(w * 0.67, h * 0.62),
      btnR: btnR,
      lRect: Rect.fromLTWH(w * 0.03, h * 0.02, w * 0.28, h * 0.12),
      rRect: Rect.fromLTWH(w * 0.69, h * 0.02, w * 0.28, h * 0.12),
      selectRect: Rect.fromCenter(center: Offset(w * 0.40, h * 0.90), width: w * 0.17, height: h * 0.07),
      startRect: Rect.fromCenter(center: Offset(w * 0.60, h * 0.90), width: w * 0.17, height: h * 0.07),
    );
  }

  final Offset dpadC;
  final double dpadR;
  final Offset aC;
  final Offset bC;
  final double btnR;
  final Rect lRect;
  final Rect rRect;
  final Rect startRect;
  final Rect selectRect;

  /// Teclas ativas para um dedo em [p].
  int maskAt(Offset p) {
    var m = 0;
    final d = p - dpadC;
    final dist = d.distance;
    if (dist <= dpadR * 1.3 && dist > dpadR * 0.18) {
      final t = dist * 0.38;
      if (d.dx > t) m |= Keys.right;
      if (d.dx < -t) m |= Keys.left;
      if (d.dy > t) m |= Keys.down;
      if (d.dy < -t) m |= Keys.up;
    }
    if ((p - aC).distance <= btnR * 1.2) m |= Keys.a;
    if ((p - bC).distance <= btnR * 1.2) m |= Keys.b;
    final pad = btnR * 0.25;
    if (lRect.inflate(pad).contains(p)) m |= Keys.l;
    if (rRect.inflate(pad).contains(p)) m |= Keys.r;
    if (startRect.inflate(pad).contains(p)) m |= Keys.start;
    if (selectRect.inflate(pad).contains(p)) m |= Keys.select;
    return m;
  }
}

class TouchControls extends StatefulWidget {
  const TouchControls({
    super.key,
    required this.landscape,
    required this.opacity,
    required this.onKeys,
  });

  final bool landscape;
  final double opacity;
  final ValueChanged<int> onKeys;

  @override
  State<TouchControls> createState() => _TouchControlsState();
}

class _TouchControlsState extends State<TouchControls> {
  final Map<int, Offset> _pointers = {};
  final ValueNotifier<int> _mask = ValueNotifier<int>(0);
  ControlLayout? _layout;

  void _recompute() {
    final lay = _layout;
    if (lay == null) return;
    var m = 0;
    for (final p in _pointers.values) {
      m |= lay.maskAt(p);
    }
    if (m != _mask.value) {
      _mask.value = m;
      widget.onKeys(m);
    }
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, cons) {
      final size = Size(cons.maxWidth, cons.maxHeight);
      final layout = ControlLayout(size, widget.landscape);
      _layout = layout;
      return Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: (e) {
          _pointers[e.pointer] = e.localPosition;
          _recompute();
        },
        onPointerMove: (e) {
          _pointers[e.pointer] = e.localPosition;
          _recompute();
        },
        onPointerUp: (e) {
          _pointers.remove(e.pointer);
          _recompute();
        },
        onPointerCancel: (e) {
          _pointers.remove(e.pointer);
          _recompute();
        },
        child: CustomPaint(
          size: size,
          painter: _ControlsPainter(layout, _mask, widget.opacity),
        ),
      );
    });
  }
}

class _ControlsPainter extends CustomPainter {
  _ControlsPainter(this.l, this.mask, this.opacity) : super(repaint: mask);

  final ControlLayout l;
  final ValueListenable<int> mask;
  final double opacity;

  Color _c(double a) => Color.fromRGBO(255, 255, 255, (a * opacity).clamp(0.0, 1.0).toDouble());

  void _label(Canvas canvas, String text, Offset center, double fontSize) {
    final tp = TextPainter(
      text: TextSpan(
        text: text,
        style: TextStyle(color: _c(0.9), fontSize: fontSize, fontWeight: FontWeight.w700),
      ),
      textDirection: TextDirection.ltr,
    )..layout();
    tp.paint(canvas, center - Offset(tp.width / 2, tp.height / 2));
  }

  @override
  void paint(Canvas canvas, Size size) {
    final m = mask.value;
    final fill = Paint()..color = _c(0.16);
    final on = Paint()..color = _c(0.45);
    final line = Paint()
      ..color = _c(0.45)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2;

    // D-pad
    canvas.drawCircle(l.dpadC, l.dpadR, Paint()
      ..color = _c(0.10)
      ..style = PaintingStyle.fill);
    canvas.drawCircle(l.dpadC, l.dpadR, line);
    final thick = l.dpadR * 0.36;
    final len = l.dpadR * 0.78;
    void arm(Offset dir, bool pressed) {
      final c = l.dpadC + dir * (l.dpadR * 0.62);
      final rect = Rect.fromCenter(
        center: c,
        width: dir.dx != 0 ? len : thick,
        height: dir.dy != 0 ? len : thick,
      );
      canvas.drawRRect(RRect.fromRectAndRadius(rect, Radius.circular(thick * 0.3)), pressed ? on : fill);
    }

    arm(const Offset(0, -1), m & Keys.up != 0);
    arm(const Offset(0, 1), m & Keys.down != 0);
    arm(const Offset(-1, 0), m & Keys.left != 0);
    arm(const Offset(1, 0), m & Keys.right != 0);
    canvas.drawRect(Rect.fromCenter(center: l.dpadC, width: thick, height: thick), fill);

    // A / B
    void round(Offset c, String t, bool pressed) {
      canvas.drawCircle(c, l.btnR, pressed ? on : fill);
      canvas.drawCircle(c, l.btnR, line);
      _label(canvas, t, c, l.btnR * 0.8);
    }

    round(l.aC, 'A', m & Keys.a != 0);
    round(l.bC, 'B', m & Keys.b != 0);

    // L / R / Start / Select
    void pill(Rect r, String t, bool pressed, double fs) {
      final rr = RRect.fromRectAndRadius(r, Radius.circular(r.height * 0.45));
      canvas.drawRRect(rr, pressed ? on : fill);
      canvas.drawRRect(rr, line);
      _label(canvas, t, r.center, fs);
    }

    pill(l.lRect, 'L', m & Keys.l != 0, l.btnR * 0.7);
    pill(l.rRect, 'R', m & Keys.r != 0, l.btnR * 0.7);
    pill(l.selectRect, 'SELECT', m & Keys.select != 0, l.btnR * 0.38);
    pill(l.startRect, 'START', m & Keys.start != 0, l.btnR * 0.38);
  }

  @override
  bool shouldRepaint(_ControlsPainter old) => old.opacity != opacity || !identical(old.l, l);
}
