import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/vesper_native.dart';
import 'value_picker.dart';

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> with SingleTickerProviderStateMixin, WidgetsBindingObserver {
  final VesperNative _engine = VesperNative.instance;

  static const _monitoringLabels = ['APPLE LOG', 'REC.709 LUT', 'FALSE COLOR', 'PEAKING', 'ZEBRAS'];
  static const _angles = [360.0, 270.0, 180.0, 172.8, 144.0, 90.0, 45.0, 22.5, 11.25, 5.625, 2.8, 1.4, 0.7];
  // Shutter-speed denominators in 1/3 stops plus the cinema standards.
  static const _speedDenominators = [
    1.0,
    1.3,
    1.6,
    2.0,
    2.5,
    3.0,
    4.0,
    5.0,
    6.0,
    8.0,
    10.0,
    13.0,
    15.0,
    20.0,
    24.0,
    25.0,
    30.0,
    40.0,
    48.0,
    50.0,
    60.0,
    80.0,
    96.0,
    100.0,
    120.0,
    125.0,
    160.0,
    200.0,
    250.0,
    320.0,
    400.0,
    500.0,
    640.0,
    800.0,
    1000.0,
    1250.0,
    1600.0,
    2000.0,
    2500.0,
    3200.0,
    4000.0,
    5000.0,
    6400.0,
    8000.0,
    10000.0,
    12800.0,
    16000.0,
    20000.0,
    25600.0,
    32000.0,
    40000.0,
    51200.0,
    64000.0,
    80000.0,
    100000.0,
  ];
  static const _isoStops = [
    25,
    32,
    40,
    50,
    64,
    80,
    100,
    125,
    160,
    200,
    250,
    320,
    400,
    500,
    640,
    800,
    1000,
    1250,
    1600,
    2000,
    2500,
    3200,
    4000,
    5000,
    6400,
    8000,
    10000,
    12800,
  ];
  static const _allFps = [23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 60.0];

  CameraCapabilities? _caps;
  String? _cameraId;
  bool _streaming = false;
  int _monitoringMode = 1;
  int _cropMode = 0; // 0 = 16:9, 1 = 4:3 open gate
  bool _speedMode = false; // shutter shown/set as 1/x instead of an angle
  double _shutterAngle = 180;
  int _exposureNs = 20833333; // used in speed mode
  double _fps = 24;
  int _iso = 100;
  int _kelvin = 5600;
  int _tint = 0;
  double _focus = 0;
  bool _ois = true;
  bool _awbAuto = false;
  bool _afContinuous = false;
  bool _faceDetect = false;
  bool _lensCorrection = true;
  bool _hotPixelFix = true;
  double _temporalNr = 0; // 0 off, 0.5 low, 0.7 medium, 0.85 high
  double _chromaNr = 0; // 0 off, 0.5 low, 1 high
  bool _nrAlignment = true;
  Offset? _focusMark; // last tap-to-focus point (normalised), shown briefly
  Timer? _focusMarkTimer;
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
    WidgetsBinding.instance.addObserver(this);
    _pulse = AnimationController(vsync: this, duration: const Duration(milliseconds: 1000))..repeat(reverse: true);
    _start();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
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
    final cam = _engine.enumerateCameras().where((c) => c.supportsRaw10).firstOrNull;
    if (cam == null) {
      setState(() => _statusMessage = 'NO RAW10-CAPABLE CAMERA FOUND');
      return;
    }
    _cameraId = cam.id;
    if (!await _openAndStream(createTexture: true)) return;
    _poll = Timer.periodic(const Duration(milliseconds: 250), (_) => _onPoll());
  }

  // Opens the camera, pushes every setting and starts streaming. Also used
  // when returning from the background (Android revokes camera access there).
  Future<bool> _openAndStream({bool createTexture = false}) async {
    if (!_engine.openCamera(_cameraId!)) {
      setState(() => _statusMessage = 'FAILED TO OPEN CAMERA $_cameraId');
      return false;
    }
    _caps = _engine.capabilities();
    if (!_fpsOptions.contains(_fps)) _fps = _fpsOptions.last;
    _iso = _iso.clamp(_caps?.minIso ?? 50, _caps?.maxIso ?? 3200);

    _engine.setCropMode(_cropMode);
    _engine.setMonitoringMode(_monitoringMode);
    _engine.setKelvinTint(_kelvin, _tint);
    _engine.setAutoWhiteBalance(_awbAuto);
    _engine.setOis(_ois);
    _engine.setFocus(_focus);
    _engine.setContinuousFocus(_afContinuous);
    _engine.setFaceDetection(_faceDetect);
    _engine.setLensCorrection(_lensCorrection);
    _engine.setHotPixelFix(_hotPixelFix);
    _engine.setTemporalNr(_temporalNr);
    _engine.setChromaNr(_chromaNr);
    _engine.setNrAlignment(_nrAlignment);
    _engine.setFrameRate(_fps);
    _applyShutter();

    if (createTexture) {
      final (w, h) = _engine.outputSize();
      _textureId = await _engine.createViewfinderTexture(w, h);
    }
    final ok = _engine.startStream();
    if (!mounted) return ok;
    setState(() {
      _streaming = ok;
      _statusMessage = ok ? 'STREAMING' : 'FAILED TO START RAW STREAM';
    });
    return ok;
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (_cameraId == null) return;
    if (state == AppLifecycleState.paused || state == AppLifecycleState.hidden) {
      if (_recording && !_stopping) {
        _stopping = true;
        _engine.stopRecording();
        _finishRecording('user');
      }
      if (_streaming) {
        _engine.closeCamera();
        _streaming = false;
      }
    } else if (state == AppLifecycleState.resumed && !_streaming) {
      _openAndStream();
    }
  }

  List<double> get _fpsOptions {
    final maxFps = _caps?.maxFpsFor(_cropMode) ?? 30;
    return _allFps.where((f) => f <= maxFps + 0.01).toList();
  }

  int get _frameNs => (1e9 / _fps).round();
  int get _angleNs => (_shutterAngle / 360 * _frameNs).round();
  int get _currentExposureNs => _speedMode ? _exposureNs : _angleNs;

  void _applyShutter() {
    if (_speedMode) {
      _engine.setExposureTime(_exposureNs, _iso);
    } else {
      _engine.setShutterAngle(_shutterAngle, _iso);
    }
  }

  String _speedLabel(int ns) {
    final d = 1e9 / ns;
    if (d < 1.0) return '${(ns / 1e9).toStringAsFixed(1)}s';
    return d >= 10 ? '1/${d.round()}' : '1/${d.toStringAsFixed(1)}';
  }

  String _angleLabel(double a) => '${a >= 10 ? a.toStringAsFixed(a % 1 == 0 ? 0 : 1) : a.toStringAsFixed(2)}°';

  void _onPoll() {
    final s = _engine.status();
    if (s == null || !mounted) return;
    setState(() {
      _status = s;
      if (s.awbAuto) {
        _kelvin = s.kelvin.round();
        _tint = s.tint.round();
      }
      if (_afContinuous) _focus = s.focusDiopters;
    });
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

  void _cycleMonitoring() {
    setState(() => _monitoringMode = (_monitoringMode + 1) % _monitoringLabels.length);
    _engine.setMonitoringMode(_monitoringMode);
  }

  Future<void> _toggleCrop() async {
    if (_recording) return;
    setState(() {
      _cropMode = 1 - _cropMode;
      if (!_fpsOptions.contains(_fps)) _fps = _fpsOptions.last; // e.g. 60 fps is 16:9-only
    });
    _engine.setCropMode(_cropMode);
    _engine.setFrameRate(_fps);
    _applyShutter();
    final (w, h) = _engine.outputSize();
    await _engine.resizeViewfinderTexture(w, h);
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

  void _pickFps() {
    if (_recording) return;
    final opts = _fpsOptions;
    showWheelPicker<double>(
      context: context,
      title: 'FRAME RATE (max ${(_caps?.maxFpsFor(_cropMode) ?? 30).round()} at ${_cropMode == 0 ? '16:9' : '4:3'})',
      values: opts,
      label: _fpsLabel,
      initialIndex: opts.indexOf(_fps),
      onChanged: (f) {
        setState(() => _fps = f);
        _engine.setFrameRate(f);
        _applyShutter();
      },
    );
  }

  String _fpsLabel(double f) => f % 1 == 0 ? f.toStringAsFixed(0) : f.toStringAsFixed(f * 1000 % 10 == 0 ? 2 : 3);

  void _pickShutter() {
    final minNs = _caps?.minExposureNs ?? 10000;
    final maxNs = [_caps?.maxExposureNs ?? _frameNs, _frameNs].reduce((a, b) => a < b ? a : b);
    final speeds = _speedDenominators.map((d) => (1e9 / d).round()).where((ns) => ns >= minNs && ns <= maxNs).toList()
      ..add(maxNs)
      ..add(minNs);
    final speedList = speeds.toSet().toList()..sort((a, b) => b.compareTo(a));
    final angleList = _angles.where((a) => a / 360 * _frameNs >= minNs).toList();

    showModalBottomSheet(
      context: context,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.transparent,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) {
          int nearest(List<int> l, int v) {
            var best = 0;
            for (var i = 0; i < l.length; i++) {
              if ((l[i] - v).abs() < (l[best] - v).abs()) best = i;
            }
            return best;
          }

          return SafeArea(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 12),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(
                    'SHUTTER  (sensor ${_speedLabel(maxNs)} … ${_speedLabel(minNs)})',
                    style: const TextStyle(
                      color: Colors.white54,
                      fontSize: 11,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 1.5,
                    ),
                  ),
                  const SizedBox(height: 8),
                  Segmented(
                    options: const ['ANGLE', 'SPEED'],
                    selected: _speedMode ? 1 : 0,
                    onSelected: (i) {
                      setState(() {
                        if (i == 1 && !_speedMode) _exposureNs = _angleNs;
                        if (i == 0 && _speedMode) {
                          _shutterAngle =
                              angleList[nearest(
                                angleList.map((a) => (a / 360 * _frameNs).round()).toList(),
                                _exposureNs,
                              )];
                        }
                        _speedMode = i == 1;
                      });
                      setSheet(() {});
                      _applyShutter();
                    },
                  ),
                  const SizedBox(height: 6),
                  if (_speedMode)
                    WheelSelector<int>(
                      key: const ValueKey('speed'),
                      values: speedList,
                      label: _speedLabel,
                      initialIndex: nearest(speedList, _exposureNs),
                      onChanged: (ns) {
                        setState(() => _exposureNs = ns);
                        _applyShutter();
                      },
                    )
                  else
                    WheelSelector<double>(
                      key: const ValueKey('angle'),
                      values: angleList,
                      label: _angleLabel,
                      initialIndex: angleList.indexOf(_shutterAngle).clamp(0, angleList.length - 1),
                      onChanged: (a) {
                        setState(() => _shutterAngle = a);
                        _applyShutter();
                      },
                    ),
                ],
              ),
            ),
          );
        },
      ),
    );
  }

  void _pickIso() {
    final minIso = _caps?.minIso ?? 50, maxIso = _caps?.maxIso ?? 3200;
    final isos = {minIso, ..._isoStops.where((i) => i > minIso && i < maxIso), maxIso}.toList()..sort();
    var initial = 0;
    for (var i = 0; i < isos.length; i++) {
      if ((isos[i] - _iso).abs() < (isos[initial] - _iso).abs()) initial = i;
    }
    showWheelPicker<int>(
      context: context,
      title: 'ISO  (native range $minIso–$maxIso)',
      values: isos,
      label: (i) => '$i',
      initialIndex: initial,
      onChanged: (i) {
        setState(() => _iso = i);
        _applyShutter();
      },
    );
  }

  void _pickWhiteBalance() {
    final kelvins = [for (var k = 2000; k <= 10000; k += 100) k];
    showModalBottomSheet(
      context: context,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.transparent,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) => SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 10, 16, 12),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Text(
                  'WHITE BALANCE',
                  style: TextStyle(
                    color: Colors.white54,
                    fontSize: 11,
                    fontWeight: FontWeight.bold,
                    letterSpacing: 1.5,
                  ),
                ),
                const SizedBox(height: 8),
                Segmented(
                  options: const ['MANUAL', 'AUTO (GOOGLE AWB)'],
                  selected: _awbAuto ? 1 : 0,
                  onSelected: (i) {
                    setState(() => _awbAuto = i == 1);
                    setSheet(() {});
                    _engine.setAutoWhiteBalance(_awbAuto);
                    // Leaving AUTO keeps the last Google estimate as the manual setting.
                    if (!_awbAuto) _engine.setKelvinTint(_kelvin, _tint);
                  },
                ),
                if (_awbAuto)
                  const Padding(
                    padding: EdgeInsets.symmetric(vertical: 18),
                    child: Text(
                      "Following Google's AWB. It never alters the RAW — switch to MANUAL to lock the current value.",
                      style: TextStyle(color: Colors.white54, fontSize: 11),
                    ),
                  ),
                if (!_awbAuto)
                  WheelSelector<int>(
                    values: kelvins,
                    label: (k) => '${k}K',
                    initialIndex: kelvins.indexOf((_kelvin / 100).round() * 100).clamp(0, kelvins.length - 1),
                    onChanged: (k) {
                      setState(() => _kelvin = k);
                      _engine.setKelvinTint(_kelvin, _tint);
                    },
                  ),
                if (!_awbAuto)
                  Row(
                    children: [
                      const Text(
                        'G',
                        style: TextStyle(color: Colors.greenAccent, fontWeight: FontWeight.bold),
                      ),
                      Expanded(
                        child: Slider(
                          value: _tint.toDouble().clamp(-50, 50),
                          min: -50,
                          max: 50,
                          divisions: 100,
                          activeColor: Colors.amber,
                          label: 'TINT $_tint',
                          onChanged: (t) {
                            setState(() => _tint = t.round());
                            setSheet(() {});
                            _engine.setKelvinTint(_kelvin, _tint);
                          },
                        ),
                      ),
                      const Text(
                        'M',
                        style: TextStyle(color: Colors.pinkAccent, fontWeight: FontWeight.bold),
                      ),
                    ],
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  void _pickFocus() {
    final maxD = _caps?.minFocusDiopters ?? 0;
    if (maxD <= 0) return;
    showModalBottomSheet(
      context: context,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.transparent,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) => SafeArea(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 10, 16, 12),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  'FOCUS  ${_distanceLabel(_focus)}   ·   tap the viewfinder to focus there',
                  style: const TextStyle(color: Colors.white54, fontSize: 11, fontWeight: FontWeight.bold),
                ),
                const SizedBox(height: 8),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    Segmented(
                      options: const ['MANUAL / LOCK', 'CONTINUOUS AF'],
                      selected: _afContinuous ? 1 : 0,
                      onSelected: (i) {
                        setState(() => _afContinuous = i == 1);
                        setSheet(() {});
                        _engine.setContinuousFocus(_afContinuous);
                      },
                    ),
                    const SizedBox(width: 12),
                    Segmented(
                      options: const ['FACES'],
                      selected: _faceDetect ? 0 : -1,
                      onSelected: (_) {
                        setState(() => _faceDetect = !_faceDetect);
                        setSheet(() {});
                        _engine.setFaceDetection(_faceDetect);
                      },
                    ),
                  ],
                ),
                if (!_afContinuous)
                  // Slider runs on sqrt(diopters) so the far range isn't crammed into a sliver.
                  Slider(
                    value: math.sqrt((_focus / maxD).clamp(0.0, 1.0)),
                    activeColor: Colors.amber,
                    onChanged: (v) {
                      setState(() => _focus = v * v * maxD);
                      setSheet(() {});
                      _engine.setFocus(_focus);
                    },
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }

  void _tapToFocus(Offset normalised) {
    if (!_streaming) return;
    _engine.setFocusPoint(normalised.dx, normalised.dy);
    _focusMarkTimer?.cancel();
    setState(() {
      _afContinuous = true;
      _focusMark = normalised;
    });
    _focusMarkTimer = Timer(const Duration(milliseconds: 1500), () {
      if (mounted) setState(() => _focusMark = null);
    });
  }

  void _pickProcessing() {
    Widget row(String label, Widget control) => Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Row(
        children: [
          SizedBox(
            width: 170,
            child: Text(label, style: const TextStyle(color: Colors.white70, fontSize: 12)),
          ),
          control,
        ],
      ),
    );
    const nrLevels = [0.0, 0.5, 0.7, 0.85];
    const chromaLevels = [0.0, 0.5, 1.0];
    showModalBottomSheet(
      context: context,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.transparent,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) {
          void update(VoidCallback f) {
            setState(f);
            setSheet(() {});
          }

          return SafeArea(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 12),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  const Text(
                    'PROCESSING (applied to recording and viewfinder)',
                    style: TextStyle(color: Colors.white54, fontSize: 11, fontWeight: FontWeight.bold),
                  ),
                  const SizedBox(height: 8),
                  row(
                    'Lens distortion correction',
                    Segmented(
                      options: const ['OFF', 'ON'],
                      selected: _lensCorrection ? 1 : 0,
                      onSelected: (i) {
                        update(() => _lensCorrection = i == 1);
                        _engine.setLensCorrection(_lensCorrection);
                      },
                    ),
                  ),
                  row(
                    'Hot / dead pixel repair',
                    Segmented(
                      options: const ['OFF', 'ON'],
                      selected: _hotPixelFix ? 1 : 0,
                      onSelected: (i) {
                        update(() => _hotPixelFix = i == 1);
                        _engine.setHotPixelFix(_hotPixelFix);
                      },
                    ),
                  ),
                  row(
                    'Temporal noise reduction',
                    Segmented(
                      options: const ['OFF', 'LOW', 'MED', 'HIGH'],
                      selected: nrLevels.indexOf(_temporalNr),
                      onSelected: (i) {
                        update(() => _temporalNr = nrLevels[i]);
                        _engine.setTemporalNr(_temporalNr);
                      },
                    ),
                  ),
                  row(
                    '  ↳ motion alignment',
                    Segmented(
                      options: const ['OFF', 'ON'],
                      selected: _nrAlignment ? 1 : 0,
                      onSelected: (i) {
                        update(() => _nrAlignment = i == 1);
                        _engine.setNrAlignment(_nrAlignment);
                      },
                    ),
                  ),
                  row(
                    'Chroma noise reduction',
                    Segmented(
                      options: const ['OFF', 'LOW', 'HIGH'],
                      selected: chromaLevels.indexOf(_chromaNr),
                      onSelected: (i) {
                        update(() => _chromaNr = chromaLevels[i]);
                        _engine.setChromaNr(_chromaNr);
                      },
                    ),
                  ),
                ],
              ),
            ),
          );
        },
      ),
    );
  }

  String _distanceLabel(double d) => d < 0.01
      ? '∞'
      : d > 1
      ? '${(100 / d).round()}cm'
      : '${(1 / d).toStringAsFixed(1)}m';
  String get _focusLabel => _afContinuous ? 'AF-C' : _distanceLabel(_focus);

  // One-shot auto exposure: two metering passes (the second one refines very
  // over/under-exposed starts). Tap keeps the shutter, long-press keeps ISO.
  Future<void> _autoExpose({required bool keepShutter}) async {
    (int, int)? r;
    for (var pass = 0; pass < 3; pass++) {
      final next = _engine.autoExpose(keepShutter: keepShutter);
      if (next != null) r = next;
      await Future<void>.delayed(const Duration(milliseconds: 350));
    }
    if (!mounted) return;
    if (r == null) {
      _toast('Auto-exposure: no metering data yet');
      return;
    }
    final (ns, iso) = r;
    setState(() {
      _iso = iso;
      if ((ns - _angleNs).abs() > _angleNs / 50) {
        _speedMode = true;
        _exposureNs = ns;
      }
    });
    _toast('Exposure set: ${_speedLabel(ns)}, ISO $iso${keepShutter ? '' : ' (ISO priority)'}');
  }

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
                    ? LayoutBuilder(
                        builder: (ctx, box) => GestureDetector(
                          onTapUp: (d) => _tapToFocus(
                            Offset(d.localPosition.dx / box.maxWidth, d.localPosition.dy / box.maxHeight),
                          ),
                          child: Stack(
                            children: [
                              Positioned.fill(
                                child: ClipRect(child: Texture(textureId: _textureId!)),
                              ),
                              if (_faceDetect && (s?.face[2] ?? 0) > 0)
                                Positioned(
                                  left: s!.face[0] * box.maxWidth,
                                  top: s.face[1] * box.maxHeight,
                                  width: s.face[2] * box.maxWidth,
                                  height: s.face[3] * box.maxHeight,
                                  child: Container(
                                    decoration: BoxDecoration(border: Border.all(color: Colors.amber, width: 1.5)),
                                  ),
                                ),
                              if (_focusMark != null)
                                Positioned(
                                  left: _focusMark!.dx * box.maxWidth - 30,
                                  top: _focusMark!.dy * box.maxHeight - 30,
                                  child: Container(
                                    width: 60,
                                    height: 60,
                                    decoration: BoxDecoration(
                                      border: Border.all(
                                        color: (s?.afState ?? 0) == 2 || (s?.afState ?? 0) == 4
                                            ? Colors.greenAccent
                                            : Colors.white,
                                        width: 1.5,
                                      ),
                                    ),
                                  ),
                                ),
                            ],
                          ),
                        ),
                      )
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
          const Center(
            child: SizedBox(width: 16, height: 16, child: CustomPaint(painter: CrosshairPainter())),
          ),

          // Top HUD (dark backing so it stays readable over bright frames)
          Positioned(
            left: 0,
            right: 0,
            top: 0,
            child: Container(
              decoration: const BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [Color(0xCC000000), Color(0x00000000)],
                ),
              ),
              child: SafeArea(
                bottom: false,
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
                      _chip('PROCESSING', onTap: _pickProcessing),
                      const SizedBox(width: 8),
                      _chip(
                        _codec == 0 ? 'HEVC 10-BIT' : 'AV1 10-BIT',
                        onTap: _recording ? null : () => setState(() => _codec = 1 - _codec),
                      ),
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
                            decoration: BoxDecoration(
                              color: Colors.red.shade900,
                              borderRadius: BorderRadius.circular(4),
                            ),
                            child: Text(
                              _timecode(s?.durationMs ?? 0),
                              style: const TextStyle(
                                color: Colors.white,
                                fontFamily: 'monospace',
                                fontWeight: FontWeight.bold,
                                fontSize: 13,
                              ),
                            ),
                          ),
                        ),
                      if (_recording) ...[
                        const SizedBox(width: 8),
                        Icon(
                          s?.audio == true ? Icons.mic : Icons.mic_off,
                          color: s?.audio == true ? Colors.greenAccent : Colors.white38,
                          size: 16,
                        ),
                      ],
                    ],
                  ),
                ),
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
                  _pill('FPS', _fpsLabel(_fps), onTap: _recording ? null : _pickFps),
                  _pill(
                    'SHUTTER',
                    _speedMode ? _speedLabel(_exposureNs) : _angleLabel(_shutterAngle),
                    subtitle: _speedMode ? _angleLabel(_exposureNs / _frameNs * 360) : _speedLabel(_currentExposureNs),
                    onTap: _pickShutter,
                  ),
                  _pill('ISO', '$_iso', onTap: _pickIso),
                  _pill(
                    'WB',
                    '${_kelvin}K',
                    subtitle: _awbAuto ? 'GOOGLE AWB' : 'TINT ${_tint > 0 ? '+' : ''}$_tint',
                    onTap: _pickWhiteBalance,
                  ),
                  GestureDetector(
                    onTap: _streaming ? () => _autoExpose(keepShutter: true) : null,
                    onLongPress: _streaming ? () => _autoExpose(keepShutter: false) : null,
                    child: _pill('AUTO', 'AE', subtitle: 'hold: ISO', onTap: null, enabled: _streaming),
                  ),
                  IconButton(
                    icon: const Icon(Icons.colorize_rounded, color: Colors.white70, size: 20),
                    tooltip: 'Meter white balance from centre',
                    onPressed: _streaming ? _lockWhiteBalance : null,
                  ),
                  _pill('FOCUS', _focusLabel, onTap: (_caps?.minFocusDiopters ?? 0) > 0 ? _pickFocus : null),
                  _toggle('OIS', _ois, Colors.greenAccent, () {
                    setState(() => _ois = !_ois);
                    _engine.setOis(_ois);
                  }),
                  GestureDetector(
                    onTap: _streaming ? _toggleRecording : null,
                    child: Container(
                      width: 54,
                      height: 54,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        border: Border.all(color: Colors.white, width: 3),
                      ),
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
    child: Text(
      text,
      style: TextStyle(color: color, fontWeight: FontWeight.bold, fontSize: 11),
    ),
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
      child: Text(
        label,
        style: TextStyle(color: on ? color : Colors.white54, fontWeight: FontWeight.bold, fontSize: 11),
      ),
    ),
  );

  Widget _pill(String label, String value, {String? subtitle, VoidCallback? onTap, bool? enabled}) => GestureDetector(
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
          Text(
            label,
            style: TextStyle(color: Colors.white.withValues(alpha: 0.5), fontSize: 9, fontWeight: FontWeight.bold),
          ),
          const SizedBox(height: 2),
          Text(
            value,
            style: TextStyle(
              color: (enabled ?? onTap != null) ? Colors.white : Colors.white38,
              fontSize: 13,
              fontWeight: FontWeight.bold,
            ),
          ),
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
