import 'package:flutter/material.dart';

import '../services/vesper_native.dart';

/// Which scopes are overlaid on the viewfinder.
enum ScopeMode { off, histogram, waveform, both }

extension ScopeModeLabel on ScopeMode {
  String get label => switch (this) {
    ScopeMode.off => 'OFF',
    ScopeMode.histogram => 'HIST',
    ScopeMode.waveform => 'WAVE',
    ScopeMode.both => 'H + W',
  };
  bool get histogram => this == ScopeMode.histogram || this == ScopeMode.both;
  bool get waveform => this == ScopeMode.waveform || this == ScopeMode.both;
}

/// Small translucent luma histogram / waveform of the recorded Apple Log
/// signal. Ignores touches so it never blocks tap-to-focus.
class ScopesOverlay extends StatelessWidget {
  final Scopes? scopes;
  final ScopeMode mode;

  const ScopesOverlay({super.key, required this.scopes, required this.mode});

  @override
  Widget build(BuildContext context) {
    final s = scopes;
    return IgnorePointer(
      child: Row(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.end,
        children: [
          if (mode.histogram) _panel('HIST', CustomPaint(painter: _HistogramPainter(s))),
          if (mode.histogram && mode.waveform) const SizedBox(width: 6),
          if (mode.waveform) _panel('WFM', CustomPaint(painter: _WaveformPainter(s))),
        ],
      ),
    );
  }

  Widget _panel(String title, Widget child) => Container(
    width: 150,
    height: 68,
    decoration: BoxDecoration(
      color: const Color(0x8C000000),
      borderRadius: BorderRadius.circular(4),
      border: Border.all(color: Colors.white12),
    ),
    child: Stack(
      children: [
        Positioned.fill(
          child: Padding(padding: const EdgeInsets.all(3), child: child),
        ),
        Positioned(
          left: 4,
          top: 2,
          child: Text(
            title,
            style: const TextStyle(color: Colors.white38, fontSize: 7, fontFamily: 'monospace'),
          ),
        ),
      ],
    ),
  );
}

// Graticule at 0 / 25 / 50 / 75 / 100% code value.
void _graticule(Canvas canvas, Size size, {required bool vertical}) {
  final p = Paint()
    ..color = Colors.white.withValues(alpha: 0.12)
    ..strokeWidth = 0.5;
  for (var i = 0; i <= 4; i++) {
    final t = i / 4;
    if (vertical) {
      canvas.drawLine(Offset(size.width * t, 0), Offset(size.width * t, size.height), p);
    } else {
      final y = size.height * (1 - t);
      canvas.drawLine(Offset(0, y), Offset(size.width, y), p);
    }
  }
}

class _HistogramPainter extends CustomPainter {
  final Scopes? s;
  _HistogramPainter(this.s);

  @override
  void paint(Canvas canvas, Size size) {
    _graticule(canvas, size, vertical: true);
    final h = s?.histogram;
    if (h == null) return;
    final path = Path()..moveTo(0, size.height);
    for (var i = 0; i < h.length; i++) {
      path.lineTo(size.width * (i + 0.5) / h.length, size.height * (1 - h[i].clamp(0.0, 1.0)));
    }
    path
      ..lineTo(size.width, size.height)
      ..close();
    canvas.drawPath(path, Paint()..color = Colors.white.withValues(alpha: 0.55));
    // Clipping warning: anything in the top bin.
    if (h.last > 0.02) {
      canvas.drawRect(
        Rect.fromLTWH(size.width - 3, 0, 3, size.height),
        Paint()..color = Colors.redAccent.withValues(alpha: 0.8),
      );
    }
  }

  @override
  bool shouldRepaint(_HistogramPainter old) => old.s != s;
}

class _WaveformPainter extends CustomPainter {
  final Scopes? s;
  _WaveformPainter(this.s);

  @override
  void paint(Canvas canvas, Size size) {
    _graticule(canvas, size, vertical: false);
    final w = s?.waveform;
    if (w == null) return;
    const cols = Scopes.waveCols, bins = Scopes.waveBins;
    final cw = size.width / cols, bh = size.height / bins;
    final paint = Paint();
    for (var c = 0; c < cols; c++) {
      for (var b = 0; b < bins; b++) {
        final v = w[c * bins + b];
        if (v <= 0) continue;
        paint.color = const Color(0xFF9CFFB0).withValues(alpha: (v * 8).clamp(0.15, 1.0));
        canvas.drawRect(Rect.fromLTWH(c * cw, size.height - (b + 1) * bh, cw + 0.3, bh + 0.3), paint);
      }
    }
  }

  @override
  bool shouldRepaint(_WaveformPainter old) => old.s != s;
}
