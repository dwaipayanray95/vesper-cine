import 'package:flutter/material.dart';

/// Bottom sheet with a horizontal scroll wheel. [onChanged] fires live while
/// scrolling so the viewfinder updates as you pick.
Future<void> showWheelPicker<T>({
  required BuildContext context,
  required String title,
  required List<T> values,
  required String Function(T) label,
  required int initialIndex,
  required ValueChanged<T> onChanged,
  Widget? header,
}) {
  return showModalBottomSheet(
    context: context,
    backgroundColor: const Color(0xEE101215),
    barrierColor: Colors.transparent,
    builder: (_) => _SheetFrame(
      title: title,
      header: header,
      child: WheelSelector<T>(values: values, label: label, initialIndex: initialIndex, onChanged: onChanged),
    ),
  );
}

class _SheetFrame extends StatelessWidget {
  final String title;
  final Widget? header;
  final Widget child;
  const _SheetFrame({required this.title, required this.child, this.header});

  @override
  Widget build(BuildContext context) => SafeArea(
    child: Padding(
      padding: const EdgeInsets.fromLTRB(16, 10, 16, 12),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Text(
            title,
            style: const TextStyle(
              color: Colors.white54,
              fontSize: 11,
              fontWeight: FontWeight.bold,
              letterSpacing: 1.5,
            ),
          ),
          if (header != null) ...[const SizedBox(height: 8), header!],
          const SizedBox(height: 6),
          child,
        ],
      ),
    ),
  );
}

/// Horizontal wheel (a ListWheelScrollView turned on its side).
class WheelSelector<T> extends StatefulWidget {
  final List<T> values;
  final String Function(T) label;
  final int initialIndex;
  final ValueChanged<T> onChanged;
  const WheelSelector({
    super.key,
    required this.values,
    required this.label,
    required this.initialIndex,
    required this.onChanged,
  });

  @override
  State<WheelSelector<T>> createState() => _WheelSelectorState<T>();
}

class _WheelSelectorState<T> extends State<WheelSelector<T>> {
  late FixedExtentScrollController _controller;
  late int _selected;

  @override
  void initState() {
    super.initState();
    _selected = widget.initialIndex.clamp(0, widget.values.length - 1);
    _controller = FixedExtentScrollController(initialItem: _selected);
  }

  @override
  void didUpdateWidget(covariant WheelSelector<T> old) {
    super.didUpdateWidget(old);
    if (old.values != widget.values) {
      _selected = widget.initialIndex.clamp(0, widget.values.length - 1);
      _controller.dispose();
      _controller = FixedExtentScrollController(initialItem: _selected);
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      height: 64,
      child: Stack(
        alignment: Alignment.center,
        children: [
          Container(
            width: 92,
            height: 44,
            decoration: BoxDecoration(
              border: Border.all(color: Colors.amber.withValues(alpha: 0.7)),
              borderRadius: BorderRadius.circular(6),
            ),
          ),
          RotatedBox(
            quarterTurns: 3,
            child: ListWheelScrollView.useDelegate(
              key: ValueKey(widget.values.length),
              controller: _controller,
              itemExtent: 92,
              diameterRatio: 3.0,
              perspective: 0.002,
              physics: const FixedExtentScrollPhysics(),
              onSelectedItemChanged: (i) {
                setState(() => _selected = i);
                widget.onChanged(widget.values[i]);
              },
              childDelegate: ListWheelChildBuilderDelegate(
                childCount: widget.values.length,
                builder: (_, i) => RotatedBox(
                  quarterTurns: 1,
                  child: Center(
                    child: Text(
                      widget.label(widget.values[i]),
                      style: TextStyle(
                        color: i == _selected ? Colors.amber : Colors.white70,
                        fontSize: i == _selected ? 17 : 14,
                        fontWeight: FontWeight.bold,
                        fontFamily: 'monospace',
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// Segmented two/three-way switch used inside picker headers.
class Segmented extends StatelessWidget {
  final List<String> options;
  final int selected;
  final ValueChanged<int> onSelected;
  const Segmented({super.key, required this.options, required this.selected, required this.onSelected});

  @override
  Widget build(BuildContext context) => Row(
    mainAxisSize: MainAxisSize.min,
    children: [
      for (var i = 0; i < options.length; i++)
        GestureDetector(
          onTap: () => onSelected(i),
          child: Container(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 5),
            margin: const EdgeInsets.symmetric(horizontal: 2),
            decoration: BoxDecoration(
              color: i == selected ? Colors.amber.withValues(alpha: 0.2) : Colors.white10,
              borderRadius: BorderRadius.circular(4),
              border: Border.all(color: i == selected ? Colors.amber : Colors.white24),
            ),
            child: Text(
              options[i],
              style: TextStyle(
                color: i == selected ? Colors.amber : Colors.white70,
                fontSize: 11,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
        ),
    ],
  );
}

/// Bottom sheet with a labelled slider, applied live.
Future<void> showSliderSheet({
  required BuildContext context,
  required String title,
  required double min,
  required double max,
  required double value,
  required String Function(double) label,
  required ValueChanged<double> onChanged,
  int? divisions,
}) {
  return showModalBottomSheet(
    context: context,
    backgroundColor: const Color(0xEE101215),
    barrierColor: Colors.transparent,
    builder: (_) {
      var v = value;
      return StatefulBuilder(
        builder: (ctx, setSheet) => _SheetFrame(
          title: '$title  ${label(v)}',
          child: Slider(
            value: v.clamp(min, max),
            min: min,
            max: max,
            divisions: divisions,
            activeColor: Colors.amber,
            onChanged: (x) {
              setSheet(() => v = x);
              onChanged(x);
            },
          ),
        ),
      );
    },
  );
}
