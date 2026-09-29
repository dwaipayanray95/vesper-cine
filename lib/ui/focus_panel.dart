import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'value_picker.dart';

/// Interactive cinema focus panel that docks neatly next to the left rack.
/// Provides Continuous AF vs Manual toggle, Face detection, Tap behavior,
/// and smooth fine-grained manual focus control with instant meter readout.
class CineFocusPanel extends StatelessWidget {
  final double focus;
  final double maxFocus;
  final bool afContinuous;
  final bool faceDetect;
  final bool tapLocks;
  final bool tapSetsExposure;
  final ValueChanged<double> onFocusChanged;
  final ValueChanged<bool> onContinuousChanged;
  final ValueChanged<bool> onFaceDetectChanged;
  final ValueChanged<bool> onTapLocksChanged;
  final ValueChanged<bool> onTapSetsExposureChanged;
  final VoidCallback onClose;
  final String Function(double) distanceLabel;

  const CineFocusPanel({
    super.key,
    required this.focus,
    required this.maxFocus,
    required this.afContinuous,
    required this.faceDetect,
    required this.tapLocks,
    required this.tapSetsExposure,
    required this.onFocusChanged,
    required this.onContinuousChanged,
    required this.onFaceDetectChanged,
    required this.onTapLocksChanged,
    required this.onTapSetsExposureChanged,
    required this.onClose,
    required this.distanceLabel,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 250,
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
      decoration: BoxDecoration(
        color: const Color(0xF4101216),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: Colors.white24, width: 1),
        boxShadow: const [
          BoxShadow(color: Colors.black87, blurRadius: 16, offset: Offset(2, 4)),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Header
          Align(
            alignment: Alignment.centerLeft,
            child: Text(
              'FOCUS · ${distanceLabel(focus)}',
              style: const TextStyle(
                color: Colors.amber,
                fontSize: 10,
                fontWeight: FontWeight.w700,
                letterSpacing: 1.0,
                fontFamily: 'monospace',
              ),
            ),
          ),
          const SizedBox(height: 8),

          // AF Mode toggles
          Row(
            children: [
              Expanded(
                child: Segmented(
                  options: const ['MANUAL', 'AF-C'],
                  selected: afContinuous ? 1 : 0,
                  onSelected: (i) => onContinuousChanged(i == 1),
                ),
              ),
              const SizedBox(width: 6),
              Segmented(
                options: const ['FACE'],
                selected: faceDetect ? 0 : -1,
                onSelected: (_) => onFaceDetectChanged(!faceDetect),
              ),
            ],
          ),
          const SizedBox(height: 6),

          // Tap Modes
          Wrap(
            spacing: 6,
            runSpacing: 4,
            children: [
              Segmented(
                options: const ['TRACK', 'LOCK'],
                selected: tapLocks ? 1 : 0,
                onSelected: (i) => onTapLocksChanged(i == 1),
              ),
              Segmented(
                options: const ['FOCUS ONLY', 'FOCUS + EXP'],
                selected: tapSetsExposure ? 1 : 0,
                onSelected: (i) => onTapSetsExposureChanged(i == 1),
              ),
            ],
          ),

          if (!afContinuous && maxFocus > 0) ...[
            const SizedBox(height: 10),
            Row(
              children: [
                const Text('∞', style: TextStyle(color: Colors.white70, fontSize: 13, fontWeight: FontWeight.bold)),
                Expanded(
                  child: SliderTheme(
                    data: SliderTheme.of(context).copyWith(
                      trackHeight: 3,
                      thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 7),
                      activeTrackColor: Colors.amber,
                      inactiveTrackColor: Colors.white24,
                      thumbColor: Colors.amber,
                    ),
                    child: Slider(
                      value: math.sqrt((focus / maxFocus).clamp(0.0, 1.0)),
                      onChanged: (v) {
                        onFocusChanged(v * v * maxFocus);
                      },
                    ),
                  ),
                ),
                Text(
                  maxFocus > 1 ? '${(100 / maxFocus).round()}cm' : '${(1 / maxFocus).toStringAsFixed(1)}m',
                  style: const TextStyle(color: Colors.white70, fontSize: 10, fontFamily: 'monospace'),
                ),
              ],
            ),
          ],
        ],
      ),
    );
  }
}
