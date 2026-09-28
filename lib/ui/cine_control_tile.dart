import 'package:flutter/material.dart';

/// Clean cine-style control tile for the left vertical rack.
/// Displays label, primary value (monospace, high contrast), and optional subtitle or state indicator.
class CineControlTile extends StatelessWidget {
  final String label;
  final String value;
  final String? subtitle;
  final VoidCallback? onTap;
  final VoidCallback? onLongPress;
  final bool active;
  final bool enabled;
  final Color? accentColor;
  final Widget? trailing;

  const CineControlTile({
    super.key,
    required this.label,
    required this.value,
    this.subtitle,
    this.onTap,
    this.onLongPress,
    this.active = false,
    this.enabled = true,
    this.accentColor,
    this.trailing,
  });

  @override
  Widget build(BuildContext context) {
    final effectiveAccent = accentColor ?? (active ? Colors.amber : Colors.white12);
    return GestureDetector(
      onTap: enabled ? onTap : null,
      onLongPress: enabled ? onLongPress : null,
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 150),
        width: 74,
        margin: const EdgeInsets.symmetric(vertical: 2.5),
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 5),
        decoration: BoxDecoration(
          color: active
              ? Colors.amber.withValues(alpha: 0.16)
              : const Color(0xE0101216),
          borderRadius: BorderRadius.circular(6),
          border: Border.all(
            color: effectiveAccent,
            width: active ? 1.5 : 1.0,
          ),
          boxShadow: active
              ? [
                  BoxShadow(
                    color: Colors.amber.withValues(alpha: 0.2),
                    blurRadius: 8,
                    offset: const Offset(0, 0),
                  ),
                ]
              : null,
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Expanded(
                  child: Text(
                    label.toUpperCase(),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      color: active ? Colors.amber : Colors.white54,
                      fontSize: 8,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 0.8,
                    ),
                  ),
                ),
                ...?trailing == null ? null : [trailing!],
              ],
            ),
            const SizedBox(height: 2),
            Text(
              value,
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: TextStyle(
                color: enabled ? (active ? Colors.amber : Colors.white) : Colors.white30,
                fontSize: 12,
                fontWeight: FontWeight.w700,
                fontFamily: 'monospace',
              ),
            ),
            if (subtitle != null) ...[
              const SizedBox(height: 1),
              Text(
                subtitle!,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: TextStyle(
                  color: active ? Colors.amber.withValues(alpha: 0.8) : Colors.white38,
                  fontSize: 8,
                  fontFamily: 'monospace',
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}
