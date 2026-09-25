import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/vesper_native.dart';

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> with SingleTickerProviderStateMixin {
  final VesperNative _engine = VesperNative.instance;

  static const _monitoringLabels = ['APPLE LOG', 'REC.709 LUT', 'FALSE COLOR', 'PEAKING', 'ZEBRAS'];
  static const _kelvinPresets = [2800, 3200, 4300, 5000, 5600, 6500, 7500];
  // Focus stops in diopters (1/metres); 0 = infinity.
  static const _focusStops = [0.0, 0.2, 0.5, 1.0, 2.0, 4.0, 8.0];

  bool _streaming = false;
  int _monitoringMode = 1;
  int _cropMode = 0; // 0 = 16:9, 1 = 4:3 open gate
  double _shutterAngle = 180;
  double _fps = 24;
  List<double> _fpsOptions = const [24, 25, 30];
  int _iso = 100;
  int _kelvin = 5600;
  int _tint = 0;
  double _focus = 0;
  double _minFocus = 0;
  bool _ois = true;
  int _codec = 0; // 0 HEVC, 1 AV1

  int? _textureId;
  String _statusMessage = 'INITIALIZING SENSOR...';
  EngineStatus? _status;
  RecordingFile? _recordingFile;
  bool _stopping = false;
  Timer? _poll;
  late final AnimationController _pulse;

  bool get _recording => _recordingFile != null;

  @override
  void initState() {
    super.initState();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    // The native pipeline assumes Surface.ROTATION_90 (see native_bridge.cpp rotationDegrees()).
    SystemChrome.setPreferredOrientations([DeviceOrientation.landscapeLeft]);
    _pulse = AnimationController(vsync: this, duration: const Duration(milliseconds: 1000))..repeat(reverse: true);
    _start();
  }

  @override
  void dispose() {
    _poll?.cancel();
    _pulse.dispose();
    if (_recording) {
      _engine.stopRecording();
      _engine.finalizeRecording(_recordingFile!);
    }
    _engine.destroyViewfinderTexture();
    _engine.close();
    super.dispose();
  }

  Future<void> _start() async {
    if (!_engine.initialize()) {
      setState(() => _statusMessage = 'NATIVE ENGINE UNAVAILABLE (ANDROID ONLY)');
      return;
    }
    final cameras = _engine.enumerateCameras();
    final cam = cameras.where((c) => c.supportsRaw10).firstOrNull;
    if (cam == null) {
      setState(() => _statusMessage = 'NO RAW10-CAPABLE CAMERA FOUND');
      return;
    }
    if (!_engine.openCamera(cam.id)) {
      setState(() => _statusMessage = 'FAILED TO OPEN CAMERA ${cam.id}');
      return;
    }
    _fpsOptions = [24.0, 25.0, 30.0, 60.0].where((f) => f <= cam.maxFps + 0.5).toList();
    if (_fpsOptions.isEmpty) _fpsOptions = [cam.maxFps.floorToDouble()];
    if (!_fpsOptions.contains(_fps)) _fps = _fpsOptions.first;
    _minFocus = _engine.minFocusDiopters;

    _engine.setCropMode(_cropMode);
    _engine.setMonitoringMode(_monitoringMode);
    _engine.setKelvinTint(_kelvin, _tint);
    _engine.setOis(_ois);
    _engine.setFocus(_focus);
    _engine.setFrameRate(_fps);
    _engine.setShutterAngle(_shutterAngle, _iso);

    final (w, h) = _engine.outputSize();
    final textureId = await _engine.createViewfinderTexture(w, h);
    final ok = _engine.startStream();
    if (!mounted) return;
    setState(() {
      _textureId = textureId;
      _streaming = ok;
      _statusMessage = ok ? 'RAW10 ${cam.rawWidth}x${cam.rawHeight}' : 'FAILED TO START RAW STREAM';
    });
    _poll = Timer.periodic(const Duration(milliseconds: 250), (_) => _onPoll());
  }

  void _onPoll() {
    final s = _engine.status();
    if (s == null || !mounted) return;
    setState(() => _status = s);
    // The engine stops on its own on thermal/storage limits.
    if (_recording && !_stopping && !s.recording && s.stopReason.isNotEmpty) {
      _finishRecording(s.stopReason);
    }
  }

  Future<void> _toggleRecording() async {
    if (_stopping) return;
    if (_recording) {
      _stopping = true;
      _engine.stopRecording(); // blocks until the file is finalised
      await _finishRecording('user');
      return;
    }
    HapticFeedback.mediumImpact();
    final file = await _engine.startRecording(codec: _codec);
    if (!mounted) return;
    if (file == null) {
      _toast('Could not start recording (10-bit encoder unavailable?)');
      return;
    }
    setState(() => _recordingFile = file);
  }

  Future<void> _finishRecording(String reason) async {
    _stopping = true;
    final file = _recordingFile;
    if (file != null) await _engine.finalizeRecording(file);
    if (!mounted) return;
    setState(() {
      _recordingFile = null;
      _stopping = false;
    });
    switch (reason) {
      case 'thermal':
        _toast('Recording stopped: phone is too hot. Saved ${file?.name}');
      case 'storage':
        _toast('Recording stopped: storage almost full. Saved ${file?.name}');
      default:
        _toast('Saved ${file?.name} to Movies/Vesper Cine');
    }
  }

  void _toast(String msg) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(msg), duration: const Duration(seconds: 2)));
  }

  T _next<T>(List<T> list, T current) => list[(list.indexOf(current) + 1) % list.length];

  void _cycleMonitoring() {
    setState(() => _monitoringMode = (_monitoringMode + 1) % _monitoringLabels.length);
    _engine.setMonitoringMode(_monitoringMode);
  }

  Future<void> _toggleCrop() async {
    if (_recording) return;
    setState(() => _cropMode = 1 - _cropMode);
    _engine.setCropMode(_cropMode);
    final (w, h) = _engine.outputSize();
    await _engine.resizeViewfinderTexture(w, h);
  }

  void _cycleFps() {
    if (_recording) return;
    setState(() => _fps = _next(_fpsOptions, _fps));
    _engine.setFrameRate(_fps);
  }

  void _cycleShutter() {
    setState(() => _shutterAngle = _next(const [45.0, 90.0, 172.8, 180.0, 270.0, 360.0], _shutterAngle));
    _engine.setShutterAngle(_shutterAngle, _iso);
  }

  void _cycleIso() {
    setState(() => _iso = _next(const [50, 100, 200, 400, 800, 1600, 3200], _iso));
    _engine.setShutterAngle(_shutterAngle, _iso);
  }

  void _cycleKelvin() {
    final i = _kelvinPresets.indexWhere((k) => k > _kelvin);
    setState(() {
      _kelvin = i < 0 ? _kelvinPresets.first : _kelvinPresets[i];
      _tint = 0;
    });
    _engine.setKelvinTint(_kelvin, _tint);
  }

  void _lockWhiteBalance() {
    final r = _engine.lockWhiteBalance();
    if (r == null) {
      _toast("Couldn't meter white balance — fill the centre with something white/grey and brighter");
      return;
    }
    setState(() {
      _kelvin = r.$1.round();
      _tint = r.$2.round();
    });
    _toast('White balance locked: ${_kelvin}K, tint $_tint');
  }

  void _cycleFocus() {
    final stops = _focusStops.where((d) => d <= _minFocus || d == 0).toList();
    setState(() => _focus = _next(stops, _focus));
    _engine.setFocus(_focus);
  }

  String get _focusLabel => _focus == 0 ? '∞' : '${(1 / _focus).toStringAsFixed(_focus >= 1 ? 2 : 1)}m';

  String _timecode(int ms) {
    final totalFrames = (ms * _fps / 1000).floor();
    final fpsInt = _fps.round();
    final f = totalFrames % fpsInt;
    final s = totalFrames ~/ fpsInt;
    String two(int v) => v.toString().padLeft(2, '0');
    return '${two(s ~/ 3600)}:${two((s % 3600) ~/ 60)}:${two(s % 60)}:${two(f)}';
  }

  @override
  Widget build(BuildContext context) {
    final s = _status;
    final hot = (s?.thermal ?? 0) >= 2;
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        children: [
          Center(
            child: AspectRatio(
              aspectRatio: _cropMode == 0 ? 16 / 9 : 4 / 3,
              child: Container(
                decoration: BoxDecoration(
                  color: const Color(0xFF15181C),
                  border: Border.all(
                    color: _recording ? Colors.redAccent : Colors.white12,
                    width: _recording ? 2.5 : 1.0,
                  ),
                ),
                child: _textureId != null
                    ? ClipRect(child: Texture(textureId: _textureId!))
                    : Center(
                        child: Text(
                          _statusMessage,
                          style: TextStyle(
                            color: Colors.white.withValues(alpha: 0.3),
                            fontSize: 11,
                            fontWeight: FontWeight.w600,
                            letterSpacing: 1.5,
                          ),
                        ),
                      ),
              ),
            ),
          ),
          const Center(child: SizedBox(width: 16, height: 16, child: CustomPaint(painter: CrosshairPainter()))),

          // Top HUD
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
              child: Row(
                children: [
                  _badge('APPLE LOG · 2020', Colors.amber),
                  const SizedBox(width: 8),
                  _chip(_cropMode == 0 ? '16:9' : 'OPEN GATE 4:3', onTap: _toggleCrop),
                  const SizedBox(width: 8),
                  _chip(_monitoringLabels[_monitoringMode], onTap: _cycleMonitoring, active: _monitoringMode != 0),
                  const SizedBox(width: 8),
                  _chip(_codec == 0 ? 'HEVC 10-BIT' : 'AV1 10-BIT',
                      onTap: _recording ? null : () => setState(() => _codec = 1 - _codec)),
                  const Spacer(),
                  if (hot) ...[
                    _badge(s!.thermal >= 3 ? 'THERMAL LIMIT' : 'PHONE WARM', Colors.orangeAccent),
                    const SizedBox(width: 8),
                  ],
                  if (s != null)
                    Text(
                      '${s.fps.toStringAsFixed(1)} FPS'
                      '${s.cameraDrops + s.framesDropped > 0 ? ' · ${s.cameraDrops + s.framesDropped} DROP' : ''}',
                      style: TextStyle(
                        color: s.cameraDrops + s.framesDropped > 0 ? Colors.orangeAccent : Colors.white54,
                        fontSize: 10,
                        fontFamily: 'monospace',
                      ),
                    ),
                  const SizedBox(width: 12),
                  if (_recording)
                    FadeTransition(
                      opacity: _pulse,
                      child: Container(
                        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                        decoration: BoxDecoration(color: Colors.red.shade900, borderRadius: BorderRadius.circular(4)),
                        child: Text(
                          _timecode(s?.durationMs ?? 0),
                          style: const TextStyle(color: Colors.white, fontFamily: 'monospace', fontWeight: FontWeight.bold, fontSize: 13),
                        ),
                      ),
                    ),
                  if (_recording) ...[
                    const SizedBox(width: 8),
                    Icon(s?.audio == true ? Icons.mic : Icons.mic_off,
                        color: s?.audio == true ? Colors.greenAccent : Colors.white38, size: 16),
                  ],
                ],
              ),
            ),
          ),

          // Bottom control rack
          Positioned(
            left: 0,
            right: 0,
            bottom: 0,
            child: Container(
              color: Colors.black.withValues(alpha: 0.85),
              padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  _pill('FPS', _fps.toStringAsFixed(_fps % 1 == 0 ? 0 : 3), onTap: _cycleFps),
                  _pill('SHUTTER', '${_shutterAngle.toStringAsFixed(_shutterAngle % 1 == 0 ? 0 : 1)}°',
                      subtitle: '1/${(_fps * 360 / _shutterAngle).round()}s', onTap: _cycleShutter),
                  _pill('ISO', '$_iso', onTap: _cycleIso),
                  _pill('WB', '${_kelvin}K', subtitle: 'TINT ${_tint > 0 ? '+' : ''}$_tint', onTap: _cycleKelvin),
                  IconButton(
                    icon: const Icon(Icons.colorize_rounded, color: Colors.white70, size: 20),
                    tooltip: 'Meter white balance from centre',
                    onPressed: _streaming ? _lockWhiteBalance : null,
                  ),
                  _pill('FOCUS', _focusLabel, onTap: _minFocus > 0 ? _cycleFocus : null),
                  _toggle('OIS', _ois, Colors.greenAccent, () {
                    setState(() => _ois = !_ois);
                    _engine.setOis(_ois);
                  }),
                  GestureDetector(
                    onTap: _streaming ? _toggleRecording : null,
                    child: Container(
                      width: 54,
                      height: 54,
                      decoration: BoxDecoration(shape: BoxShape.circle, border: Border.all(color: Colors.white, width: 3)),
                      child: Center(
                        child: AnimatedContainer(
                          duration: const Duration(milliseconds: 200),
                          width: _recording ? 20 : 40,
                          height: _recording ? 20 : 40,
                          decoration: BoxDecoration(
                            color: _stopping ? Colors.grey : Colors.redAccent,
                            borderRadius: BorderRadius.circular(_recording ? 4 : 20),
                          ),
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _badge(String text, Color color) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
        decoration: BoxDecoration(
          color: color.withValues(alpha: 0.15),
          borderRadius: BorderRadius.circular(4),
          border: Border.all(color: color),
        ),
        child: Text(text, style: TextStyle(color: color, fontWeight: FontWeight.bold, fontSize: 11)),
      );

  Widget _chip(String text, {VoidCallback? onTap, bool active = false}) => GestureDetector(
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
          decoration: BoxDecoration(
            color: active ? Colors.cyan.withValues(alpha: 0.2) : Colors.white10,
            borderRadius: BorderRadius.circular(4),
            border: Border.all(color: active ? Colors.cyan : Colors.white24),
          ),
          child: Text(
            text,
            style: TextStyle(
              color: onTap == null ? Colors.white38 : (active ? Colors.cyanAccent : Colors.white),
              fontSize: 11,
              fontWeight: FontWeight.bold,
            ),
          ),
        ),
      );

  Widget _toggle(String label, bool on, Color color, VoidCallback onTap) => GestureDetector(
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          decoration: BoxDecoration(
            color: on ? color.withValues(alpha: 0.2) : Colors.white10,
            borderRadius: BorderRadius.circular(4),
            border: Border.all(color: on ? color : Colors.white24),
          ),
          child: Text(label,
              style: TextStyle(color: on ? color : Colors.white54, fontWeight: FontWeight.bold, fontSize: 11)),
        ),
      );

  Widget _pill(String label, String value, {String? subtitle, VoidCallback? onTap}) => GestureDetector(
        onTap: onTap,
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
          decoration: BoxDecoration(
            color: Colors.white.withValues(alpha: 0.08),
            borderRadius: BorderRadius.circular(6),
            border: Border.all(color: Colors.white12),
          ),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Text(label,
                  style: TextStyle(color: Colors.white.withValues(alpha: 0.5), fontSize: 9, fontWeight: FontWeight.bold)),
              const SizedBox(height: 2),
              Text(value,
                  style: TextStyle(
                      color: onTap == null ? Colors.white38 : Colors.white, fontSize: 13, fontWeight: FontWeight.bold)),
              if (subtitle != null) ...[
                const SizedBox(height: 1),
                Text(subtitle, style: TextStyle(color: Colors.white.withValues(alpha: 0.4), fontSize: 8)),
              ],
            ],
          ),
        ),
      );
}

class CrosshairPainter extends CustomPainter {
  const CrosshairPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Colors.white.withValues(alpha: 0.3)
      ..strokeWidth = 1.0;
    canvas.drawLine(Offset(0, size.height / 2), Offset(size.width, size.height / 2), paint);
    canvas.drawLine(Offset(size.width / 2, 0), Offset(size.width / 2, size.height), paint);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
