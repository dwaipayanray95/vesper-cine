import 'package:flutter/material.dart';

/// DJI Ronin 4D / Cine camera style vertical dial wheel.
/// Pops open right next to / docked over the tapped control or in a sleek floating overlay.
class CineWheelDial<T> extends StatefulWidget {
  final List<T> values;
  final String Function(T) label;
  final String? Function(T)? subLabel;
  final T selectedValue;
  final ValueChanged<T> onChanged;
  final VoidCallback? onClose;
  final String title;

  const CineWheelDial({
    super.key,
    required this.values,
    required this.label,
    this.subLabel,
    required this.selectedValue,
    required this.onChanged,
    this.onClose,
    required this.title,
  });

  @override
  State<CineWheelDial<T>> createState() => _CineWheelDialState<T>();
}

class _CineWheelDialState<T> extends State<CineWheelDial<T>> {
  late FixedExtentScrollController _controller;
  late int _selectedIndex;

  @override
  void initState() {
    super.initState();
    _selectedIndex = widget.values.indexOf(widget.selectedValue);
    if (_selectedIndex < 0) _selectedIndex = 0;
    _controller = FixedExtentScrollController(initialItem: _selectedIndex);
  }

  @override
  void didUpdateWidget(covariant CineWheelDial<T> oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.values != widget.values || oldWidget.selectedValue != widget.selectedValue) {
      final newIndex = widget.values.indexOf(widget.selectedValue);
      if (newIndex >= 0 && newIndex != _selectedIndex) {
        _selectedIndex = newIndex;
        _controller.jumpToItem(_selectedIndex);
      }
    }
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 140,
      decoration: BoxDecoration(
        color: const Color(0xF0101216),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: Colors.white24, width: 1),
        boxShadow: const [
          BoxShadow(color: Colors.black87, blurRadius: 16, offset: Offset(2, 4)),
        ],
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          // Header with Title & Close button
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
            decoration: const BoxDecoration(
              border: Border(bottom: BorderSide(color: Colors.white12)),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Expanded(
                  child: Text(
                    widget.title.toUpperCase(),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(
                      color: Colors.amber,
                      fontSize: 10,
                      fontWeight: FontWeight.w700,
                      letterSpacing: 0.8,
                    ),
                  ),
                ),
                if (widget.onClose != null)
                  GestureDetector(
                    onTap: widget.onClose,
                    child: Container(
                      padding: const EdgeInsets.all(2),
                      decoration: BoxDecoration(
                        color: Colors.white10,
                        borderRadius: BorderRadius.circular(4),
                      ),
                      child: const Icon(Icons.close, size: 14, color: Colors.white70),
                    ),
                  ),
              ],
            ),
          ),
          // Vertical wheel dial
          SizedBox(
            height: 180,
            child: Stack(
              alignment: Alignment.center,
              children: [
                // Selection indicator marker lines
                Container(
                  height: 38,
                  decoration: BoxDecoration(
                    color: Colors.amber.withValues(alpha: 0.12),
                    border: Border.symmetric(
                      horizontal: BorderSide(color: Colors.amber.withValues(alpha: 0.8), width: 1.5),
                    ),
                  ),
                ),
                // Wheel list
                ListWheelScrollView.useDelegate(
                  controller: _controller,
                  itemExtent: 38,
                  diameterRatio: 2.2,
                  perspective: 0.003,
                  physics: const FixedExtentScrollPhysics(),
                  onSelectedItemChanged: (i) {
                    setState(() => _selectedIndex = i);
                    widget.onChanged(widget.values[i]);
                  },
                  childDelegate: ListWheelChildBuilderDelegate(
                    childCount: widget.values.length,
                    builder: (context, i) {
                      final isSelected = i == _selectedIndex;
                      final item = widget.values[i];
                      final sub = widget.subLabel?.call(item);
                      return Center(
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Text(
                              widget.label(item),
                              style: TextStyle(
                                color: isSelected ? Colors.amber : Colors.white60,
                                fontSize: isSelected ? 15 : 12,
                                fontWeight: isSelected ? FontWeight.bold : FontWeight.w500,
                                fontFamily: 'monospace',
                              ),
                            ),
                            if (sub != null) ...[
                              const SizedBox(width: 4),
                              Text(
                                sub,
                                style: TextStyle(
                                  color: isSelected ? Colors.amber.withValues(alpha: 0.7) : Colors.white30,
                                  fontSize: 9,
                                  fontFamily: 'monospace',
                                ),
                              ),
                            ],
                          ],
                        ),
                      );
                    },
                  ),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
