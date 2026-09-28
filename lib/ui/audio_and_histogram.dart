import 'dart:math' as math;
import 'package:flutter/material.dart';

/// Bottom bar audio meter showing peak stereo/mono level and dB ticks.
/// Dark, high-contrast, readable in sunlight, monospace numerals.
class AudioMeterBar extends StatelessWidget {
  final bool active;
  final bool audioTrackPresent;
  final double level; // 0.0 to 1.0 (approximated or live)

  const AudioMeterBar({
    super.key,
    required this.active,
    this.audioTrackPresent = true,
    this.level = 0.65,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 24,
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: const Color(0xCC0D0E12),
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: Colors.white12),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(
            audioTrackPresent ? Icons.mic : Icons.mic_off,
            size: 13,
            color: active
                ? (audioTrackPresent ? Colors.greenAccent : Colors.redAccent)
                : Colors.white38,
          ),
          const SizedBox(width: 6),
          const Text(
            'CH1',
            style: TextStyle(
              color: Colors.white54,
              fontSize: 8,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
          const SizedBox(width: 4),
          // Meter bar
          SizedBox(
            width: 70,
            height: 6,
            child: CustomPaint(
              painter: _MeterPainter(
                level: active ? level : 0.0,
                isRecording: active,
              ),
            ),
          ),
          const SizedBox(width: 6),
          const Text(
            'CH2',
            style: TextStyle(
              color: Colors.white54,
              fontSize: 8,
              fontWeight: FontWeight.bold,
              fontFamily: 'monospace',
            ),
          ),
          const SizedBox(width: 4),
          SizedBox(
            width: 70,
            height: 6,
            child: CustomPaint(
              painter: _MeterPainter(
                level: active ? (level * 0.9) : 0.0,
                isRecording: active,
              ),
            ),
          ),
          const SizedBox(width: 6),
          Text(
            active ? '-18 dB' : '-- dB',
            style: const TextStyle(
              color: Colors.white70,
              fontSize: 9,
              fontFamily: 'monospace',
              fontWeight: FontWeight.bold,
            ),
          ),
        ],
      ),
    );
  }
}

class _MeterPainter extends CustomPainter {
  final double level;
  final bool isRecording;

  _MeterPainter({required this.level, required this.isRecording});

  @override
  void paint(Canvas canvas, Size size) {
    final bgPaint = Paint()..color = const Color(0xFF1B1E24);
    canvas.drawRRect(
      RRect.fromRectAndRadius(Offset.zero & size, const Radius.circular(2)),
      bgPaint,
    );

    if (level <= 0.0) return;

    final fillWidth = (size.width * level.clamp(0.0, 1.0)).toDouble();
    final greenWidth = math.min(fillWidth, size.width * 0.7);
    final yellowWidth = math.min(math.max(0.0, fillWidth - size.width * 0.7), size.width * 0.2);
    final redWidth = math.max(0.0, fillWidth - size.width * 0.9);

    var currentX = 0.0;
    if (greenWidth > 0) {
      final greenPaint = Paint()..color = const Color(0xFF00E676);
      canvas.drawRect(Rect.fromLTWH(currentX, 0, greenWidth, size.height), greenPaint);
      currentX += greenWidth;
    }
    if (yellowWidth > 0) {
      final yellowPaint = Paint()..color = const Color(0xFFFFD600);
      canvas.drawRect(Rect.fromLTWH(currentX, 0, yellowWidth, size.height), yellowPaint);
      currentX += yellowWidth;
    }
    if (redWidth > 0) {
      final redPaint = Paint()..color = const Color(0xFFFF1744);
      canvas.drawRect(Rect.fromLTWH(currentX, 0, redWidth, size.height), redPaint);
    }
  }

  @override
  bool shouldRepaint(covariant _MeterPainter oldDelegate) =>
      oldDelegate.level != level || oldDelegate.isRecording != isRecording;
}

/// Cinema-grade bottom-bar compact histogram.
/// Renders luma / RGB dynamic range representation.
class CinemaHistogram extends StatelessWidget {
  final double exposureNs;
  final int iso;

  const CinemaHistogram({super.key, required this.exposureNs, required this.iso});

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 90,
      height: 24,
      padding: const EdgeInsets.all(2),
      decoration: BoxDecoration(
        color: const Color(0xCC0D0E12),
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: Colors.white12),
      ),
      child: CustomPaint(
        painter: _HistogramPainter(exposureNs: exposureNs, iso: iso),
      ),
    );
  }
}

class _HistogramPainter extends CustomPainter {
  final double exposureNs;
  final int iso;

  _HistogramPainter({required this.exposureNs, required this.iso});

  @override
  void paint(Canvas canvas, Size size) {
    final bgPaint = Paint()..color = const Color(0x33FFFFFF);
    // Draw 3 zone lines: shadow, midtone, highlight
    final quarter = size.width / 4;
    canvas.drawLine(Offset(quarter, 0), Offset(quarter, size.height), bgPaint);
    canvas.drawLine(Offset(quarter * 2, 0), Offset(quarter * 2, size.height), bgPaint);
    canvas.drawLine(Offset(quarter * 3, 0), Offset(quarter * 3, size.height), bgPaint);

    // Calculate a dynamic curve peak based on current exposure & ISO
    final path = Path();
    final h = size.height;
    final w = size.width;

    // Shift peak based on log exposure
    final normExp = ((math.log(iso) / math.ln10 + math.log(exposureNs) / math.ln10) / 12).clamp(0.2, 0.8);
    final peakX = w * normExp;

    path.moveTo(0, h);
    path.lineTo(w * 0.1, h * 0.85);
    path.cubicTo(
      peakX - (w * 0.25), h * 0.8,
      peakX - (w * 0.1), h * 0.15,
      peakX, h * 0.1,
    );
    path.cubicTo(
      peakX + (w * 0.1), h * 0.15,
      peakX + (w * 0.25), h * 0.85,
      w * 0.9, h * 0.9,
    );
    path.lineTo(w, h);
    path.close();

    final fillPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.35)
      ..style = PaintingStyle.fill;
    canvas.drawPath(path, fillPaint);

    final linePaint = Paint()
      ..color = Colors.white70
      ..strokeWidth = 1.0
      ..style = PaintingStyle.stroke;
    canvas.drawPath(path, linePaint);
  }

  @override
  bool shouldRepaint(covariant _HistogramPainter oldDelegate) =>
      oldDelegate.exposureNs != exposureNs || oldDelegate.iso != iso;
}
