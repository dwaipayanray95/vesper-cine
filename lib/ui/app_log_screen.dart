import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/vesper_native.dart';

/// Developer tool: the engine's log since app launch (camera, GPU, guard,
/// recorder, auto-exposure, benchmark lines), readable and copyable on the
/// phone, so testing doesn't depend on adb / logcat.
class AppLogScreen extends StatefulWidget {
  const AppLogScreen({super.key});

  @override
  State<AppLogScreen> createState() => _AppLogScreenState();
}

class _AppLogScreenState extends State<AppLogScreen> {
  final _scroll = ScrollController();
  String _text = '';
  bool _vesperOnly = false; // hide the per-5 s frame/GPU lines
  static final _statLine = RegExp(r'Frame \d+: avg wait|GPU per frame:|Camera: [\d.]+ fps measured');

  @override
  void initState() {
    super.initState();
    _reload();
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  void _reload() {
    setState(() => _text = VesperNative.instance.appLog());
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
  }

  String get _shown {
    if (!_vesperOnly) return _text;
    return _text
        .split('\n')
        .where((l) => !_statLine.hasMatch(l))
        .join('\n');
  }

  Future<void> _copy() async {
    await Clipboard.setData(ClipboardData(text: _text));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('Copied ${_text.split('\n').length} lines — paste them into the chat')),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF0E1013),
      appBar: AppBar(
        backgroundColor: const Color(0xFF14171D),
        foregroundColor: Colors.white,
        title: const Text('App log', style: TextStyle(fontSize: 16)),
        actions: [
          IconButton(
            tooltip: _vesperOnly ? 'Show all lines' : 'Hide per-frame stats',
            icon: Icon(_vesperOnly ? Icons.filter_alt : Icons.filter_alt_outlined),
            onPressed: () => setState(() => _vesperOnly = !_vesperOnly),
          ),
          IconButton(tooltip: 'Refresh', icon: const Icon(Icons.refresh), onPressed: _reload),
          IconButton(tooltip: 'Copy all', icon: const Icon(Icons.copy), onPressed: _copy),
          IconButton(
            tooltip: 'Clear',
            icon: const Icon(Icons.delete_outline),
            onPressed: () {
              VesperNative.instance.clearAppLog();
              _reload();
            },
          ),
        ],
      ),
      body: Scrollbar(
        controller: _scroll,
        child: SingleChildScrollView(
          controller: _scroll,
          padding: const EdgeInsets.all(12),
          child: SelectableText(
            _shown.isEmpty ? '(empty)' : _shown,
            style: const TextStyle(color: Colors.white70, fontSize: 11, fontFamily: 'monospace', height: 1.35),
          ),
        ),
      ),
    );
  }
}
