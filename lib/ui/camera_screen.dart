import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../services/vesper_native.dart';
import 'audio_and_histogram.dart';
import 'cine_control_tile.dart';
import 'settings_sheet.dart';
import 'value_picker.dart';
import 'wheel_dial.dart';

enum OpenWheelType { none, shutter, iso, fps, wb }

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> with SingleTickerProviderStateMixin, WidgetsBindingObserver {
  final VesperNative _engine = VesperNative.instance;

  static const _angles = [360.0, 270.0, 180.0, 172.8, 144.0, 90.0, 45.0, 22.5, 11.25, 5.625, 2.8, 1.4, 0.7];
  // Shutter-speed denominators in 1/3 stops plus the cinema standards.
  static const _speedDenominators = [
    1.0, 1.3, 1.6, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0, 13.0, 15.0, 20.0,
    24.0, 25.0, 30.0, 40.0, 48.0, 50.0, 60.0, 80.0, 96.0, 100.0, 120.0, 125.0,
    160.0, 200.0, 250.0, 320.0, 400.0, 500.0, 640.0, 800.0, 1000.0, 1250.0,
    1600.0, 2000.0, 2500.0, 3200.0, 4000.0, 5000.0, 6400.0, 8000.0, 10000.0,
    12800.0, 16000.0, 20000.0, 25600.0, 32000.0, 40000.0, 51200.0, 64000.0,
    80000.0, 100000.0,
  ];
  static const _isoStops = [
    25, 32, 40, 50, 64, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800,
    1000, 1250, 1600, 2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800,
  ];
  static const _allFps = [23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 60.0];

  CameraCapabilities? _caps;
  String? _cameraId;
  bool _streaming = false;
  int _monitoringMode = 1; // 0 = Apple Log, 1 = Rec.709 LUT, 2 = False Color, 3 = Peaking, 4 = Zebras
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
  int _codec = 0; // 0 HEVC, 1 AV1
  int _aeGen = 0; // bumps per AE request so an older run stops refining
  bool _tapLocks = false; // tap: AF then hold (AF-L) instead of tracking (AF-C)
  bool _tapSetsExposure = true; // tap also spot-meters exposure at that point
  bool _profileAvailable = false; // a chart calibration exists for this device/camera
  bool _useProfile = true;
  String _profileInfo = '';
  String? _calibrationDir;
  String? _deviceModel;
  String _lastCalibrationSaved = '';

  int? _textureId;
  String _statusMessage = 'INITIALIZING SENSOR...';
  EngineStatus? _status;
  RecordingFile? _recordingFile;
  bool _stopping = false;
  Timer? _poll;
  late final AnimationController _pulse;

  // Ronin 4D style interactive floating pop-open wheel dial state
  OpenWheelType _activeWheel = OpenWheelType.none;

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
    final info = await _engine.deviceInfo();
    _deviceModel = info?['model'] as String?;
    _calibrationDir = info?['calibrationDir'] as String?;
    await _loadColorProfile();
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
    _engine.useColorProfile(_useProfile);
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

  // Chart calibrations ship as assets/color_profiles/*.json (tools/calibration);
  // the one matching this phone model and camera id is used.
  Future<void> _loadColorProfile() async {
    try {
      final manifest = await AssetManifest.loadFromAssetBundle(rootBundle);
      for (final path in manifest.listAssets().where((a) => a.startsWith('assets/color_profiles/') && a.endsWith('.json'))) {
        final j = jsonDecode(await rootBundle.loadString(path)) as Map<String, dynamic>;
        if (j['format'] != 'vesper-color-profile/1') continue;
        if (j['device'] != _deviceModel || '${j['cameraId']}' != _cameraId) continue;
        final ill = (j['illuminants'] as List).cast<Map<String, dynamic>>();
        _engine.setColorProfile(
          [for (final i in ill) (i['forwardMatrix'] as List).map((e) => (e as num).toDouble()).toList()],
          [for (final i in ill) (i['cct'] as num).toDouble()],
        );
        _profileAvailable = true;
        _profileInfo = ill.map((i) => '${(i['cct'] as num).round()}K ΔE ${i['meanDeltaE']}').join(', ');
        return;
      }
    } catch (_) {
      // No or malformed profile: factory calibration.
    }
  }

  void _captureCalibration() {
    final dir = _calibrationDir;
    if (dir == null || !_streaming) return;
    final stamp = DateTime.now().toIso8601String().replaceAll(RegExp(r'[:.]'), '-');
    _engine.captureCalibrationFrame('$dir/CAL_${_cameraId}_${_kelvin}K_$stamp', _deviceModel ?? 'unknown');
    _toast('Capturing calibration frame…');
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
      if (_afContinuous || s.focusPulling || s.focusLocked) _focus = s.focusDiopters;
    });
    if (s.calibrationSaved.isNotEmpty && s.calibrationSaved != _lastCalibrationSaved) {
      _lastCalibrationSaved = s.calibrationSaved;
      _engine.publishCalibration(s.calibrationSaved).then((ok) {
        if (mounted) _toast(ok ? 'Saved to Downloads/Vesper Calibration' : 'Calibration frame saved (app files only)');
      });
    }
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

  String _fpsLabel(double f) => f % 1 == 0 ? f.toStringAsFixed(0) : f.toStringAsFixed(f * 1000 % 10 == 0 ? 2 : 3);

  List<int> _buildSpeedList() {
    final minNs = _caps?.minExposureNs ?? 10000;
    final maxNs = [_caps?.maxExposureNs ?? _frameNs, _frameNs].reduce((a, b) => a < b ? a : b);
    final speeds = _speedDenominators.map((d) => (1e9 / d).round()).where((ns) => ns >= minNs && ns <= maxNs).toList()
      ..add(maxNs)
      ..add(minNs);
    return speeds.toSet().toList()..sort((a, b) => b.compareTo(a));
  }

  List<double> _buildAngleList() {
    final minNs = _caps?.minExposureNs ?? 10000;
    return _angles.where((a) => a / 360 * _frameNs >= minNs).toList();
  }

  List<int> _buildIsoList() {
    final minIso = _caps?.minIso ?? 50, maxIso = _caps?.maxIso ?? 3200;
    return {minIso, ..._isoStops.where((i) => i > minIso && i < maxIso), maxIso}.toList()..sort();
  }

  void _pickWhiteBalance() {
    final kelvins = [for (var k = 2000; k <= 10000; k += 100) k];
    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.transparent,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) => SheetBody(
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
      isScrollControlled: true,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.transparent,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) => SheetBody(
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
                const SizedBox(height: 8),
                Wrap(
                  alignment: WrapAlignment.center,
                  spacing: 12,
                  runSpacing: 8,
                  children: [
                    Segmented(
                      options: const ['TAP: TRACK', 'TAP: FOCUS & LOCK'],
                      selected: _tapLocks ? 1 : 0,
                      onSelected: (i) {
                        setState(() => _tapLocks = i == 1);
                        setSheet(() {});
                      },
                    ),
                    Segmented(
                      options: const ['TAP: FOCUS ONLY', 'TAP: FOCUS + EXPOSURE'],
                      selected: _tapSetsExposure ? 1 : 0,
                      onSelected: (i) {
                        setState(() => _tapSetsExposure = i == 1);
                        setSheet(() {});
                      },
                    ),
                  ],
                ),
                const Padding(
                  padding: EdgeInsets.only(top: 6),
                  child: Text(
                    'PDAF + laser AF. TRACK keeps following; FOCUS & LOCK holds once sharp (AF-L). '
                    'Long-press the viewfinder always focuses and locks.',
                    style: TextStyle(color: Colors.white38, fontSize: 10),
                  ),
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

  // Tap: hardware AF (PDAF + laser) on the region, either tracking (AF-C) or one
  // scan that the HAL then holds (AF-L). Optionally also spot-meters exposure there.
  void _tapToFocus(Offset p, {bool lock = false}) {
    if (!_streaming) return;
    lock = lock || _tapLocks;
    _engine.focusAt(p.dx, p.dy, lock: lock);
    if (lock) HapticFeedback.mediumImpact();
    if (_tapSetsExposure) {
      _engine.setMetering(1, p.dx, p.dy);
      _autoExpose(keepShutter: true, quiet: true);
    }
    // The box stays on screen (green once AF has landed) until the next tap.
    setState(() {
      _afContinuous = !lock;
      _focusMark = p;
      _activeWheel = OpenWheelType.none;
    });
  }

  void _openSettings() {
    showModalBottomSheet(
      context: context,
      isScrollControlled: true,
      backgroundColor: const Color(0xEE101215),
      barrierColor: Colors.black54,
      builder: (_) => StatefulBuilder(
        builder: (ctx, setSheet) => SettingsSheet(
          codec: _codec,
          onCodecChanged: (c) {
            setState(() => _codec = c);
            setSheet(() {});
          },
          cropMode: _cropMode,
          onCropModeChanged: (m) async {
            await _toggleCrop();
            setSheet(() {});
          },
          isRecording: _recording,
          lensCorrection: _lensCorrection,
          onLensCorrectionChanged: (val) {
            setState(() => _lensCorrection = val);
            _engine.setLensCorrection(val);
            setSheet(() {});
          },
          hotPixelFix: _hotPixelFix,
          onHotPixelFixChanged: (val) {
            setState(() => _hotPixelFix = val);
            _engine.setHotPixelFix(val);
            setSheet(() {});
          },
          temporalNr: _temporalNr,
          onTemporalNrChanged: (val) {
            setState(() => _temporalNr = val);
            _engine.setTemporalNr(val);
            setSheet(() {});
          },
          nrAlignment: _nrAlignment,
          onNrAlignmentChanged: (val) {
            setState(() => _nrAlignment = val);
            _engine.setNrAlignment(val);
            setSheet(() {});
          },
          chromaNr: _chromaNr,
          onChromaNrChanged: (val) {
            setState(() => _chromaNr = val);
            _engine.setChromaNr(val);
            setSheet(() {});
          },
          profileAvailable: _profileAvailable,
          useProfile: _useProfile,
          profileInfo: _profileInfo,
          onUseProfileChanged: (val) {
            if (!_profileAvailable) {
              _toast('No chart profile for this phone yet (see tools/calibration)');
              return;
            }
            setState(() => _useProfile = val);
            _engine.useColorProfile(val);
            setSheet(() {});
          },
          onCaptureCalibration: _streaming && _calibrationDir != null ? _captureCalibration : null,
          tapLocks: _tapLocks,
          onTapLocksChanged: (val) {
            setState(() => _tapLocks = val);
            setSheet(() {});
          },
          tapSetsExposure: _tapSetsExposure,
          onTapSetsExposureChanged: (val) {
            setState(() => _tapSetsExposure = val);
            setSheet(() {});
          },
          faceDetect: _faceDetect,
          onFaceDetectChanged: (val) {
            setState(() => _faceDetect = val);
            _engine.setFaceDetection(val);
            setSheet(() {});
          },
        ),
      ),
    );
  }

  String _distanceLabel(double d) => d < 0.01
      ? '∞'
      : d > 1
      ? '${(100 / d).round()}cm'
      : '${(1 / d).toStringAsFixed(1)}m';

  // CONTROL_AF_STATE: 0 inactive (manual), 1/3 scanning, 2 passive focused,
  // 4 focused & locked, 5 locked but not in focus, 6 passive unfocused.
  String get _focusLabel {
    final s = _status;
    final st = s?.afState ?? 0;
    if (st == 1 || st == 3) return 'AF…';
    if (_afContinuous) return 'AF-C';
    if (s?.focusLocked ?? false) return st == 5 ? 'AF-L ✕' : 'AF-L ${_distanceLabel(_focus)}';
    return _distanceLabel(_focus);
  }

  // AF has converged (tracking and focused, locked in focus, or held manually).
  bool get _afLanded {
    final s = _status;
    if (s == null) return false;
    if (s.afState == 2 || s.afState == 4) return true;
    return s.focusLocked && !_afContinuous && s.afState == 0;
  }

  // One-shot auto exposure: two metering passes (the second one refines very
  // over/under-exposed starts). Tap keeps the shutter, long-press keeps ISO.
  Future<void> _autoExpose({required bool keepShutter, bool quiet = false}) async {
    // Each pass glides to the new exposure (native ExposureRamp); wait for the
    // glide to finish and a settled frame to be metered before refining.
    final gen = ++_aeGen;
    (int, int)? r;
    for (var pass = 0; pass < 3; pass++) {
      (int, int)? next;
      for (var i = 0; i < 10 && next == null; i++) {
        next = _engine.autoExpose(keepShutter: keepShutter);
        if (next == null) await Future<void>.delayed(const Duration(milliseconds: 100));
      }
      if (gen != _aeGen) return; // superseded by a newer tap
      if (next == null) break;
      r = next;
      for (var i = 0; i < 25 && (_engine.status()?.exposureRamping ?? false); i++) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
      }
      await Future<void>.delayed(const Duration(milliseconds: 250));
      if (gen != _aeGen) return;
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
    if (!quiet) _toast('Exposure set: ${_speedLabel(ns)}, ISO $iso${keepShutter ? '' : ' (ISO priority)'}');
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
    final speedList = _buildSpeedList();
    final angleList = _buildAngleList();
    final isoList = _buildIsoList();
    final fpsList = _fpsOptions;

    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        children: [
          // 1. Center Viewfinder: exact output aspect (16:9 or 4:3 open gate)
          // Texture coordinates normalised 0..1 to this rect strictly preserved!
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
                          behavior: HitTestBehavior.opaque,
                          onTapUp: (d) => _tapToFocus(
                            Offset(d.localPosition.dx / box.maxWidth, d.localPosition.dy / box.maxHeight),
                          ),
                          onLongPressStart: (d) => _tapToFocus(
                            Offset(d.localPosition.dx / box.maxWidth, d.localPosition.dy / box.maxHeight),
                            lock: true,
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
                                        color: _afLanded ? Colors.greenAccent : Colors.white,
                                        width: _afLanded ? 2 : 1.5,
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

          // Central crosshair overlay
          const Center(
            child: SizedBox(width: 16, height: 16, child: CustomPaint(painter: CrosshairPainter())),
          ),

          // 2. TOP BAR
          // Shows: APPLE LOG · 2020 - Resolution selector - Rec.709 LUT toggle - Denoters (False color, Peaking, Zebras)
          // Recording status & duration, telemetry
          Positioned(
            left: 0,
            right: 0,
            top: 0,
            child: Container(
              decoration: const BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topCenter,
                  end: Alignment.bottomCenter,
                  colors: [Color(0xDD000000), Color(0x00000000)],
                ),
              ),
              child: SafeArea(
                bottom: false,
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
                  child: Row(
                    children: [
                      // Apple Log badge
                      _badge('APPLE LOG · 2020', Colors.amber),
                      const SizedBox(width: 8),

                      // Resolution / aspect selector
                      _chip(
                        _cropMode == 0 ? '16:9' : '4:3 OPEN GATE',
                        onTap: _toggleCrop,
                        active: _cropMode == 1,
                      ),
                      const SizedBox(width: 8),

                      // Dedicated LUT toggle: APPLE LOG (native RAW/log) vs REC.709 LUT
                      _chip(
                        _monitoringMode == 1 ? 'REC.709 LUT' : 'LOG VIEW',
                        active: _monitoringMode == 1,
                        onTap: () {
                          // Toggle between 0 (Apple Log) and 1 (Rec.709 LUT)
                          setState(() {
                            _monitoringMode = (_monitoringMode == 1) ? 0 : 1;
                          });
                          _engine.setMonitoringMode(_monitoringMode);
                        },
                      ),
                      const SizedBox(width: 10),

                      // Separate exposure/focus assistance denoters:
                      // FC (False Color = 2), PEAK (Peaking = 3), ZEBRA (Zebras = 4)
                      _toolDenoter('FC', _monitoringMode == 2, () {
                        setState(() {
                          _monitoringMode = (_monitoringMode == 2) ? 1 : 2;
                        });
                        _engine.setMonitoringMode(_monitoringMode);
                      }),
                      const SizedBox(width: 4),
                      _toolDenoter('PEAK', _monitoringMode == 3, () {
                        setState(() {
                          _monitoringMode = (_monitoringMode == 3) ? 1 : 3;
                        });
                        _engine.setMonitoringMode(_monitoringMode);
                      }),
                      const SizedBox(width: 4),
                      _toolDenoter('ZEBRA', _monitoringMode == 4, () {
                        setState(() {
                          _monitoringMode = (_monitoringMode == 4) ? 1 : 4;
                        });
                        _engine.setMonitoringMode(_monitoringMode);
                      }),

                      const Spacer(),

                      // Hardware / thermal warning
                      if (hot) ...[
                        _badge(s!.thermal >= 3 ? 'THERMAL LIMIT' : 'PHONE WARM', Colors.orangeAccent),
                        const SizedBox(width: 8),
                      ],

                      // Live engine telemetry
                      if (s != null)
                        Text(
                          '${s.fps.toStringAsFixed(1)} FPS'
                          '${s.gpuMs > 0 ? ' · GPU ${s.gpuMs.toStringAsFixed(1)}ms' : ''}'
                          '${s.nrThrottled ? ' · NR PAUSED' : s.alignThrottled ? ' · ALIGN OFF' : ''}'
                          '${s.cameraDrops + s.framesDropped > 0 ? ' · ${s.cameraDrops + s.framesDropped} DROP' : ''}',
                          style: TextStyle(
                            color: s.cameraDrops + s.framesDropped > 0 ? Colors.orangeAccent : Colors.white54,
                            fontSize: 10,
                            fontFamily: 'monospace',
                          ),
                        ),
                      const SizedBox(width: 10),

                      // Recording status & timecode
                      if (_recording) ...[
                        FadeTransition(
                          opacity: _pulse,
                          child: Container(
                            padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                            decoration: BoxDecoration(
                              color: Colors.red.shade900,
                              borderRadius: BorderRadius.circular(4),
                              border: Border.all(color: Colors.redAccent),
                            ),
                            child: Row(
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                Container(
                                  width: 7,
                                  height: 7,
                                  decoration: const BoxDecoration(
                                    color: Colors.white,
                                    shape: BoxShape.circle,
                                  ),
                                ),
                                const SizedBox(width: 6),
                                Text(
                                  _timecode(s?.durationMs ?? 0),
                                  style: const TextStyle(
                                    color: Colors.white,
                                    fontFamily: 'monospace',
                                    fontWeight: FontWeight.bold,
                                    fontSize: 12,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),
                      ],

                      // Settings button (pushes full settings sheet)
                      IconButton(
                        icon: const Icon(Icons.settings, color: Colors.white70, size: 20),
                        tooltip: 'Settings',
                        onPressed: _openSettings,
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),

          // 3. LEFT CONTROL RACK (DJI Ronin 4D / Blackmagic layout)
          // Moves all main controls to the left side without blocking camera feed
          Positioned(
            left: 12,
            top: 50,
            bottom: 44,
            child: SingleChildScrollView(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  // FPS control
                  CineControlTile(
                    label: 'FPS',
                    value: _fpsLabel(_fps),
                    active: _activeWheel == OpenWheelType.fps,
                    enabled: !_recording,
                    onTap: () {
                      setState(() {
                        _activeWheel = _activeWheel == OpenWheelType.fps ? OpenWheelType.none : OpenWheelType.fps;
                      });
                    },
                  ),

                  // SHUTTER control (tap toggles Ronin 4D wheel dial, long press switches Angle/Speed)
                  CineControlTile(
                    label: 'SHUTTER',
                    value: _speedMode ? _speedLabel(_exposureNs) : _angleLabel(_shutterAngle),
                    subtitle: _speedMode ? 'SPD' : 'ANG',
                    active: _activeWheel == OpenWheelType.shutter,
                    onTap: () {
                      setState(() {
                        _activeWheel = _activeWheel == OpenWheelType.shutter ? OpenWheelType.none : OpenWheelType.shutter;
                      });
                    },
                    onLongPress: () {
                      // Switch between angle and speed mode
                      setState(() {
                        if (!_speedMode) {
                          _speedMode = true;
                          _exposureNs = _angleNs;
                        } else {
                          _speedMode = false;
                        }
                      });
                      _applyShutter();
                      _toast('Shutter mode: ${_speedMode ? 'Speed (1/s)' : 'Angle (°)'}');
                    },
                  ),

                  // ISO control (tap toggles Ronin 4D wheel dial)
                  CineControlTile(
                    label: 'ISO',
                    value: '$_iso',
                    active: _activeWheel == OpenWheelType.iso,
                    onTap: () {
                      setState(() {
                        _activeWheel = _activeWheel == OpenWheelType.iso ? OpenWheelType.none : OpenWheelType.iso;
                      });
                    },
                  ),

                  // WHITE BALANCE control
                  CineControlTile(
                    label: 'WB',
                    value: '${_kelvin}K',
                    subtitle: _awbAuto ? 'AUTO' : '${_tint > 0 ? '+' : ''}$_tint',
                    onTap: _pickWhiteBalance,
                  ),

                  // FOCUS control
                  CineControlTile(
                    label: 'FOCUS',
                    value: _focusLabel,
                    subtitle: _afContinuous ? 'AF-C' : 'MAN',
                    accentColor: _afLanded ? Colors.greenAccent : null,
                    onTap: (_caps?.minFocusDiopters ?? 0) > 0 ? _pickFocus : null,
                  ),

                  // AUTO-EXPOSURE (AE) trigger
                  CineControlTile(
                    label: 'AUTO',
                    value: 'AE',
                    subtitle: 'HOLD:ISO',
                    enabled: _streaming,
                    onTap: _streaming
                        ? () {
                            _engine.setMetering(0);
                            _autoExpose(keepShutter: true);
                          }
                        : null,
                    onLongPress: _streaming
                        ? () {
                            _engine.setMetering(0);
                            _autoExpose(keepShutter: false);
                          }
                        : null,
                  ),

                  // OIS toggle
                  CineControlTile(
                    label: 'STAB',
                    value: _ois ? 'OIS ON' : 'OIS OFF',
                    active: _ois,
                    accentColor: _ois ? Colors.greenAccent : Colors.white24,
                    onTap: () {
                      setState(() => _ois = !_ois);
                      _engine.setOis(_ois);
                    },
                  ),

                  // WB Spot Color Picker
                  GestureDetector(
                    onTap: _streaming ? _lockWhiteBalance : null,
                    child: Container(
                      width: 74,
                      margin: const EdgeInsets.symmetric(vertical: 2.5),
                      padding: const EdgeInsets.symmetric(vertical: 5),
                      decoration: BoxDecoration(
                        color: const Color(0xE0101216),
                        borderRadius: BorderRadius.circular(6),
                        border: Border.all(color: Colors.white12),
                      ),
                      child: const Center(
                        child: Icon(Icons.colorize_rounded, color: Colors.white70, size: 16),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),

          // 4. FLOATING RONIN 4D STYLE WHEEL DIAL
          // Appears immediately next to the left control rack without blocking viewfinder
          if (_activeWheel != OpenWheelType.none)
            Positioned(
              left: 92,
              top: _activeWheel == OpenWheelType.fps
                  ? 50
                  : _activeWheel == OpenWheelType.shutter
                  ? 85
                  : 125,
              child: _buildActiveWheelDial(speedList, angleList, isoList, fpsList),
            ),

          // 5. RIGHT SIDE RECORD BUTTON (Center-right without overlapping camera feed)
          Positioned(
            right: 18,
            top: 0,
            bottom: 0,
            child: Center(
              child: GestureDetector(
                onTap: _streaming ? _toggleRecording : null,
                child: Container(
                  width: 66,
                  height: 66,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: const Color(0xCC0D0E12),
                    border: Border.all(
                      color: _recording ? Colors.redAccent : Colors.white70,
                      width: 3.5,
                    ),
                    boxShadow: [
                      BoxShadow(
                        color: _recording ? Colors.red.withValues(alpha: 0.4) : Colors.black87,
                        blurRadius: 14,
                        offset: const Offset(0, 0),
                      ),
                    ],
                  ),
                  child: Center(
                    child: AnimatedContainer(
                      duration: const Duration(milliseconds: 200),
                      width: _recording ? 22 : 46,
                      height: _recording ? 22 : 46,
                      decoration: BoxDecoration(
                        color: _stopping ? Colors.grey : Colors.redAccent,
                        borderRadius: BorderRadius.circular(_recording ? 5 : 23),
                      ),
                    ),
                  ),
                ),
              ),
            ),
          ),

          // 6. BOTTOM BAR (Dark, high-contrast, readable in sunlight, monospace numerals)
          // Contains Cinema Histogram + Dual-channel Audio Meter
          Positioned(
            left: 0,
            right: 0,
            bottom: 0,
            child: Container(
              color: const Color(0xEE090B0D),
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 5),
              child: Row(
                children: [
                  // Histogram
                  CinemaHistogram(
                    exposureNs: _currentExposureNs.toDouble(),
                    iso: _iso,
                  ),
                  const SizedBox(width: 14),

                  // Dual Audio Meter
                  AudioMeterBar(
                    active: _recording,
                    audioTrackPresent: s?.audio ?? true,
                    level: _recording ? 0.68 : 0.0,
                  ),

                  const Spacer(),

                  // Quick indicators: Codec & Aspect
                  Text(
                    '${_codec == 0 ? 'HEVC 10b' : 'AV1 10b'} · ${_cropMode == 0 ? '16:9' : '4:3'}',
                    style: const TextStyle(
                      color: Colors.white54,
                      fontSize: 10,
                      fontFamily: 'monospace',
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(width: 8),
                  if (_useProfile && _profileAvailable)
                    const Text(
                      '· CAL',
                      style: TextStyle(
                        color: Colors.amber,
                        fontSize: 10,
                        fontFamily: 'monospace',
                        fontWeight: FontWeight.bold,
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

  Widget _buildActiveWheelDial(
    List<int> speedList,
    List<double> angleList,
    List<int> isoList,
    List<double> fpsList,
  ) {
    switch (_activeWheel) {
      case OpenWheelType.shutter:
        if (_speedMode) {
          int nearestIndex = 0;
          for (var i = 0; i < speedList.length; i++) {
            if ((speedList[i] - _exposureNs).abs() < (speedList[nearestIndex] - _exposureNs).abs()) {
              nearestIndex = i;
            }
          }
          final selectedVal = speedList[nearestIndex];
          return CineWheelDial<int>(
            title: 'SHUTTER SPEED',
            values: speedList,
            label: _speedLabel,
            selectedValue: selectedVal,
            onChanged: (ns) {
              setState(() => _exposureNs = ns);
              _applyShutter();
            },
            onClose: () => setState(() => _activeWheel = OpenWheelType.none),
          );
        } else {
          return CineWheelDial<double>(
            title: 'SHUTTER ANGLE',
            values: angleList,
            label: _angleLabel,
            selectedValue: _shutterAngle,
            onChanged: (a) {
              setState(() => _shutterAngle = a);
              _applyShutter();
            },
            onClose: () => setState(() => _activeWheel = OpenWheelType.none),
          );
        }

      case OpenWheelType.iso:
        int nearestIso = 0;
        for (var i = 0; i < isoList.length; i++) {
          if ((isoList[i] - _iso).abs() < (isoList[nearestIso] - _iso).abs()) {
            nearestIso = i;
          }
        }
        return CineWheelDial<int>(
          title: 'ISO GAIN',
          values: isoList,
          label: (i) => '$i',
          selectedValue: isoList[nearestIso],
          onChanged: (i) {
            setState(() => _iso = i);
            _applyShutter();
          },
          onClose: () => setState(() => _activeWheel = OpenWheelType.none),
        );

      case OpenWheelType.fps:
        return CineWheelDial<double>(
          title: 'FRAME RATE',
          values: fpsList,
          label: _fpsLabel,
          selectedValue: _fps,
          onChanged: (f) {
            setState(() => _fps = f);
            _engine.setFrameRate(f);
            _applyShutter();
          },
          onClose: () => setState(() => _activeWheel = OpenWheelType.none),
        );

      default:
        return const SizedBox.shrink();
    }
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
        color: active ? Colors.amber.withValues(alpha: 0.2) : Colors.white10,
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: active ? Colors.amber : Colors.white24),
      ),
      child: Text(
        text,
        style: TextStyle(
          color: onTap == null ? Colors.white38 : (active ? Colors.amber : Colors.white),
          fontSize: 11,
          fontWeight: FontWeight.bold,
        ),
      ),
    ),
  );

  Widget _toolDenoter(String text, bool active, VoidCallback onTap) => GestureDetector(
    onTap: onTap,
    child: Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 3),
      decoration: BoxDecoration(
        color: active ? Colors.cyanAccent.withValues(alpha: 0.25) : Colors.white10,
        borderRadius: BorderRadius.circular(4),
        border: Border.all(color: active ? Colors.cyanAccent : Colors.white24),
      ),
      child: Text(
        text,
        style: TextStyle(
          color: active ? Colors.cyanAccent : Colors.white54,
          fontSize: 10,
          fontWeight: FontWeight.w700,
          fontFamily: 'monospace',
        ),
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
