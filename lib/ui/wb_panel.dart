import 'package:flutter/material.dart';
import 'value_picker.dart';

/// Interactive cinema white balance panel that docks next to the left rack.
/// Allows toggling Auto WB (Google AWB) vs Manual Kelvin wheel + Tint slider.
class CineWbPanel extends StatefulWidget {
  final int kelvin;
  final int tint;
  final bool awbAuto;
  final ValueChanged<int> onKelvinChanged;
  final ValueChanged<int> onTintChanged;
  final ValueChanged<bool> onAwbAutoChanged;
  final VoidCallback onClose;
  final VoidCallback? onPick; // eyedropper: next viewfinder tap samples WB there

  const CineWbPanel({
    super.key,
    required this.kelvin,
    required this.tint,
    required this.awbAuto,
    required this.onKelvinChanged,
    required this.onTintChanged,
    required this.onAwbAutoChanged,
    required this.onClose,
    this.onPick,
  });

  static final List<int> kelvinStops = [for (var k = 2000; k <= 10000; k += 100) k];

  @override
  State<CineWbPanel> createState() => _CineWbPanelState();
}

class _CineWbPanelState extends State<CineWbPanel> {
  static List<int> get kelvinStops => CineWbPanel.kelvinStops;
  // Owned here: a controller created in build() is replaced on every status
  // poll (4x/s), which resets the wheel mid-scroll.
  late final FixedExtentScrollController _kelvinController = FixedExtentScrollController(
    initialItem: kelvinStops.indexOf((widget.kelvin / 100).round() * 100).clamp(0, kelvinStops.length - 1),
  );

  int get kelvin => widget.kelvin;
  int get tint => widget.tint;
  bool get awbAuto => widget.awbAuto;

  @override
  void dispose() {
    _kelvinController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 230,
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 8),
      decoration: BoxDecoration(
        color: const Color(0xF4101216),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: Colors.white24, width: 1),
        boxShadow: const [BoxShadow(color: Colors.black87, blurRadius: 16, offset: Offset(2, 4))],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          // Header
          Align(
            alignment: Alignment.centerLeft,
            child: Text(
              'WB · ${kelvin}K ${tint >= 0 ? "+$tint" : "$tint"}',
              style: const TextStyle(
                color: Colors.amber,
                fontSize: 10,
                fontWeight: FontWeight.w700,
                letterSpacing: 1.0,
                fontFamily: 'monospace',
              ),
            ),
          ),
          const SizedBox(height: 6),

          // Auto / Manual toggle + eyedropper
          Row(
            children: [
              Expanded(
                child: Segmented(
                  options: const ['MANUAL', 'GOOGLE AWB'],
                  selected: awbAuto ? 1 : 0,
                  onSelected: (i) => widget.onAwbAutoChanged(i == 1),
                ),
              ),
              const SizedBox(width: 6),
              GestureDetector(
                onTap: widget.onPick,
                child: Container(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
                  decoration: BoxDecoration(
                    color: Colors.white10,
                    borderRadius: BorderRadius.circular(4),
                    border: Border.all(color: widget.onPick == null ? Colors.white12 : Colors.white38),
                  ),
                  child: Icon(
                    Icons.colorize_rounded,
                    size: 15,
                    color: widget.onPick == null ? Colors.white24 : Colors.white,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 6),

          if (awbAuto)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 20),
              child: Text(
                'Following Google AWB live.\nSwitch to MANUAL to lock value.',
                textAlign: TextAlign.center,
                style: TextStyle(color: Colors.white54, fontSize: 10),
              ),
            ),

          if (!awbAuto) ...[
            // Vertical wheel for Kelvin
            SizedBox(
              height: 120,
              child: Stack(
                alignment: Alignment.center,
                children: [
                  Container(
                    height: 32,
                    decoration: BoxDecoration(
                      color: Colors.amber.withValues(alpha: 0.12),
                      border: Border.symmetric(
                        horizontal: BorderSide(color: Colors.amber.withValues(alpha: 0.8), width: 1.2),
                      ),
                    ),
                  ),
                  ListWheelScrollView.useDelegate(
                    itemExtent: 32,
                    diameterRatio: 2.0,
                    perspective: 0.003,
                    physics: const FixedExtentScrollPhysics(),
                    controller: _kelvinController,
                    onSelectedItemChanged: (i) => widget.onKelvinChanged(kelvinStops[i]),
                    childDelegate: ListWheelChildBuilderDelegate(
                      childCount: kelvinStops.length,
                      builder: (ctx, i) {
                        final k = kelvinStops[i];
                        final isSel = (k - kelvin).abs() < 50;
                        return Center(
                          child: Text(
                            '${k}K',
                            style: TextStyle(
                              color: isSel ? Colors.amber : Colors.white60,
                              fontSize: isSel ? 14 : 11,
                              fontWeight: isSel ? FontWeight.bold : FontWeight.w500,
                              fontFamily: 'monospace',
                            ),
                          ),
                        );
                      },
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 4),

            // Tint Slider
            Row(
              children: [
                const Text(
                  'G',
                  style: TextStyle(color: Colors.greenAccent, fontSize: 10, fontWeight: FontWeight.bold),
                ),
                Expanded(
                  child: SliderTheme(
                    data: SliderTheme.of(context).copyWith(
                      trackHeight: 2,
                      thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 6),
                      activeTrackColor: Colors.amber,
                      inactiveTrackColor: Colors.white24,
                      thumbColor: Colors.amber,
                    ),
                    child: Slider(
                      value: tint.toDouble().clamp(-50, 50),
                      min: -50,
                      max: 50,
                      divisions: 100,
                      onChanged: (t) => widget.onTintChanged(t.round()),
                    ),
                  ),
                ),
                const Text(
                  'M',
                  style: TextStyle(color: Colors.pinkAccent, fontSize: 10, fontWeight: FontWeight.bold),
                ),
              ],
            ),
          ],
        ],
      ),
    );
  }
}
