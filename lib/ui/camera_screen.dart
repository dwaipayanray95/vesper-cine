import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../build_flags.dart';
import '../services/vesper_native.dart';
import 'cine_control_tile.dart';
import 'focus_panel.dart';
import 'scopes_overlay.dart';
import 'settings_sheet.dart';
import 'wb_panel.dart';
import 'wheel_dial.dart';

enum OpenWheelType { none, shutter, iso, fps, wb, focus }

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
  int _monitoringMode = 1; // 0 = Apple Log, 1 = Rec.709 LUT, 2 = False Color, 3 = Peaking, 4 = Zebras
  int _baseLutMode = 1; // Remembers whether user selected Apple Log (0) or Rec.709 (1)
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
  bool _calibratedNoise = true; // Developer: noise model from the sensor profile (off = the camera's own)
  double _temporalNr = 0; // 0 off, 0.5 low, 0.7 medium, 0.85 high
  double _chromaNr = 0; // 0 off, 0.5 low, 1 high
  bool _nrAlignment = true;
  Offset? _focusMark; // last tap-to-focus point (normalised), shown briefly
  int _codec = 0; // 0 HEVC, 1 AV1
  int _recordQuality = 0; // 0 standard, 1 high (2x bitrate), 2 max (3x)
  int _aeGen = 0; // bumps per AE request so an older run stops refining
  ScopeMode _scopeMode = ScopeMode.off;
  Scopes? _scopes;
  bool _gpuGuard = true; // auto-pause optional passes when frames would drop
  bool _oversampling = true; // HQ: full-sensor luma, anti-alias downscaled
  int _sharpening = 1; // detail enhancement 0 off, 1 low, 2 medium, 3 high
  bool _magnify = false; // focus magnifier: viewfinder punched in around the focus point
  Offset _magCenter = const Offset(0.5, 0.5);
  static const _magScale = 3.0;
  bool _wbPickMode = false; // next viewfinder tap picks white balance
  Offset? _wbPickMark; // where WB was last picked (shown briefly)
  bool _tapLocks = false; // tap: AF then hold (AF-L) instead of tracking (AF-C)
  bool _tapSetsExposure = true; // tap also spot-meters exposure at that point
  bool _profileAvailable = false; // a chart calibration exists for this device/camera
  bool _useProfile = true;
  String _profileInfo = '';
  String? _calibrationDir;
  String? _deviceModel;
  String _lastCalibrationSaved = '';
  File? _settingsFile; // persisted user settings (app-private storage)
  String? _filesDir;
  IsoAnalysis? _isoAnalysis; // measured native ISOs of this camera (Settings -> Native ISO Analysis)
  bool _isoSweepRunning = false;
  bool _calSweepRunning = false; // sensor calibration sweep (Settings > Developer)
  bool _calSweepDark = false;
  String _savedSettings = '';

  bool _settingsOpen = false; // settings page covers the viewfinder: processing paused
  bool _benchmarking = false;
  // Recording power saver (Settings › Recording): 0 off, 1 dim screen, 2 dim +
  // viewfinder off. Kicks in 10 s into a take (or after a wake-up tap).
  int _powerSaver = 0;
  bool _saverActive = false;
  Timer? _saverTimer;
  static const _saverDelay = Duration(seconds: 10);
  int _streamGeneration = 0; // +1 per (re)open: a benchmark spanning a camera restart is invalid
  String _benchStep = '';
  bool _permissionDenied = false; // camera permission refused: status text offers a retry
  int? _textureId;
  final GlobalKey _vfKey = GlobalKey();
  Rect? _vfRectSent;
  (bool, Offset)? _zoomSent;

  // Keeps the native viewfinder surface on top of the viewfinder box and
  // mirrors the magnifier state to the GPU (called after every frame; only
  // sends when something changed).
  void _syncViewfinder() {
    if (!mounted) return;
    final box = _vfKey.currentContext?.findRenderObject() as RenderBox?;
    if (box != null && box.hasSize && box.attached) {
      final dpr = MediaQuery.of(context).devicePixelRatio;
      final o = box.localToGlobal(Offset.zero) * dpr;
      final r = Rect.fromLTWH(o.dx.roundToDouble(), o.dy.roundToDouble(),
          (box.size.width * dpr).roundToDouble(), (box.size.height * dpr).roundToDouble());
      if (r != _vfRectSent) {
        _vfRectSent = r;
        _engine.setViewfinderRect(r.left.toInt(), r.top.toInt(), r.width.toInt(), r.height.toInt());
      }
    }
    final zoom = (_magnify, _magCenter);
    if (zoom != _zoomSent) {
      _zoomSent = zoom;
      _engine.setViewfinderZoom(_magCenter.dx, _magCenter.dy, _magnify ? _magScale : 1.0);
    }
  }
  String _statusMessage = 'INITIALIZING SENSOR...';
  EngineStatus? _status;
  bool _overloadWarned = false;
  bool _heatWarned = false; // "very hot, processing reduced" shown for this take
  RecordingFile? _recordingFile;
  bool _stopping = false;
  Timer? _poll;
  late final AnimationController _pulse;

  // Single active floating panel / wheel dial
  OpenWheelType _activeWheel = OpenWheelType.none;

  bool get _recording => _recordingFile != null;

  @override
  void initState() {
    super.initState();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    // The native pipeline assumes Surface.ROTATION_90 (see native_bridge.cpp rotationDegrees()).
    // Both landscapes; the native side follows the display rotation (MainActivity).
    SystemChrome.setPreferredOrientations([DeviceOrientation.landscapeLeft, DeviceOrientation.landscapeRight]);
    WidgetsBinding.instance.addObserver(this);
    _pulse = AnimationController(vsync: this, duration: const Duration(milliseconds: 1000))..repeat(reverse: true);
    _start();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _poll?.cancel();
    _saverTimer?.cancel();
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
    // Wait for the camera permission before touching the camera: on first
    // launch the dialog is still up when we get here.
    if (!await _engine.requestPermissions()) {
      if (mounted) setState(() => _statusMessage = 'CAMERA + MICROPHONE PERMISSION NEEDED — TAP TO ASK AGAIN');
      _permissionDenied = true;
      return;
    }
    _permissionDenied = false;
    final cam = _engine.enumerateCameras().where((c) => c.supportsRaw10).firstOrNull;
    if (cam == null) {
      setState(() => _statusMessage = 'NO RAW10-CAPABLE CAMERA FOUND');
      return;
    }
    _cameraId = cam.id;
    final info = await _engine.deviceInfo();
    _engine.log('Vesper Cine ${info?['version']} (build ${info?['build']}) on ${info?['model']}, Android ${info?['android']}',
        tag: 'Vesper_UI');
    _deviceModel = info?['model'] as String?;
    _calibrationDir = info?['calibrationDir'] as String?;
    final filesDir = info?['filesDir'] as String?;
    _filesDir = filesDir;
    if (filesDir != null) {
      _loadIsoAnalysis();
      _settingsFile = File('$filesDir/vesper_settings.json');
      _loadSettings();
    }
    await _loadColorProfile();
    await _loadSensorProfile();
    final recovered = await _engine.recoverRecordings();
    if (recovered.isNotEmpty && mounted) {
      final (name, complete) = recovered.first;
      final more = recovered.length > 1 ? ' (+${recovered.length - 1} more)' : '';
      _engine.log('Recovered interrupted recordings: ${recovered.map((r) => r.$1).join(', ')}', tag: 'Vesper_UI');
      _toast(complete
          ? 'Recovered a take from the last session: $name$more'
          : 'A take was cut off last session (app closed mid-recording): kept as $name$more, may need repair');
    }
    if (!await _openAndStream(createTexture: true)) return;
    _poll = Timer.periodic(const Duration(milliseconds: 250), (_) => _onPoll());
  }

  // Opens the camera, pushes every setting and starts streaming. Also used
  // when returning from the background (Android revokes camera access there).
  Future<bool> _openAndStream({bool createTexture = false}) async {
    _streamGeneration++;
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
    _engine.setSharpening(_sharpening);
    _engine.setOversampling(_oversampling);
    _engine.setBudgetGuard(_gpuGuard);
    _engine.setNativeIsos(_isoAnalysis?.baseIso ?? 0, _isoAnalysis?.hcgIso ?? 0);
    _engine.useColorProfile(_useProfile);
    _engine.setScopes(_scopeMode != ScopeMode.off);
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
      _saveSettings();
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

  // --- GPU benchmark (developer tool) --------------------------------------
  // Measures the GPU time of each optional pass on this phone: every feature
  // alone, then all together, with the budget guard off. Then, with
  // everything on: each GPU pass (and parts of HQ and alignment) run an extra
  // time per frame — the increase is its cost, since per-pass GPU timestamps
  // are useless on this GPU — and each optimisation experiment on its own.
  // Everything on is measured again at the end and the two runs averaged, so
  // a slow drift (the phone warming up) cancels out. Settings are restored
  // afterwards. Results are shown and printed to the app log (VesperBench).
  // Run at 60 fps for costs: the GPU is then always at full clock.
  static const _benchPasses = ['unpack', 'HQ', 'align', 'NR', 'render'];
  // Partial passes (VulkanEngine::setRepeatPass 5..8).
  static const _benchParts = [
    ('All on, align luma x2', 5),
    ('All on, align search x2', 6),
    ('All on, + HQ load', 7),
    ('All on, + HQ load+demos', 8),
  ];
  // Opt-in experiments: (label, VulkanEngine::kExp* bit). None pending
  // (0.14.1: wide HQ tiles became the default, -1.4 ms).
  static const List<(String, int)> _benchExperiments = [];
  // The guard's first step: motion search every 4th frame instead of every 2nd.
  static const _benchAlignReduced = 'All on, align every 4th';

  Future<void> _runGpuBenchmark() async {
    if (_benchmarking || !_streaming || _recording) return;
    setState(() => _benchmarking = true);
    void apply({bool hq = false, int sharp = 0, double tnr = 0, double cnr = 0, bool align = false,
        int exps = 0, int repeat = -1, int alignEvery = 0}) {
      _engine.setOversampling(hq);
      _engine.setSharpening(sharp);
      _engine.setTemporalNr(tnr);
      _engine.setChromaNr(cnr);
      _engine.setNrAlignment(align);
      _engine.setExperiments(exps);
      _engine.setRepeatPass(repeat);
      _engine.setAlignInterval(alignEvery);
      _engine.setBudgetGuard(false); // again every step: a camera restart re-applies the user's guard setting
    }

    void allOn({int exps = 0, int repeat = -1, int alignEvery = 0}) =>
        apply(hq: true, sharp: 1, tnr: 0.7, cnr: 0.5, align: true, exps: exps, repeat: repeat, alignEvery: alignEvery);

    _engine.setBudgetGuard(false);
    final configs = <(String, void Function())>[
      ('Base (all off)', () => apply()),
      ('Sharpening LOW', () => apply(sharp: 1)),
      ('HQ oversampling', () => apply(hq: true)),
      ('Chroma NR', () => apply(cnr: 0.5)),
      ('Temporal NR', () => apply(tnr: 0.7)),
      ('Temporal NR + align', () => apply(tnr: 0.7, align: true)),
      ('Everything on', () => allOn()),
      for (var p = 0; p < _benchPasses.length; p++) ('All on, ${_benchPasses[p]} x2', () => allOn(repeat: p)),
      for (final (label, probe) in _benchParts) (label, () => allOn(repeat: probe)),
      (_benchAlignReduced, () => allOn(alignEvery: 4)),
      for (final (label, bit) in _benchExperiments) (label, () => allOn(exps: bit)),
      ('Everything on (again)', () => allOn()),
    ];
    final results = <String>[];
    final measured = <String, double>{};
    final stepOf = <String, int>{}; // position in the run, for the warm-up drift correction
    double? base;
    // A camera restart (app left the foreground) re-applies the user's
    // settings, and a stage the guard holds paused makes a step measure less
    // than it says: either makes the numbers meaningless, so stop and say so.
    final generation = _streamGeneration;
    String? interrupted;
    for (var i = 0; i < configs.length && mounted; i++) {
      final (name, set) = configs[i];
      set();
      setState(() => _benchStep = 'GPU BENCHMARK ${i + 1}/${configs.length}: $name');
      await Future<void>.delayed(const Duration(milliseconds: 2500)); // let the smoothed timing settle
      final drops0 = _engine.status()?.cameraDrops ?? 0;
      var sum = 0.0;
      for (var k = 0; k < 8; k++) {
        await Future<void>.delayed(const Duration(milliseconds: 125));
        sum += _engine.status()?.gpuMs ?? 0;
      }
      final ms = sum / 8;
      final st = _engine.status();
      if (_streamGeneration != generation || !_streaming) {
        interrupted = 'Interrupted at "$name": the camera restarted (did the app leave the screen?). Run it again.';
        break;
      }
      if (st != null && (st.alignThrottled || st.alignReduced || st.nrThrottled || (st.hqSupported && !st.hqAvailable))) {
        interrupted = 'Interrupted at "$name": the GPU guard had paused processing. Run it again.';
        break;
      }
      measured[name] = ms;
      stepOf[name] = i;
      final drops = (_engine.status()?.cameraDrops ?? 0) - drops0;
      base ??= ms;
      final line =
          '${name.padRight(24)} ${ms.toStringAsFixed(1).padLeft(5)} ms'
          '${i > 0 ? '  (+${(ms - base).toStringAsFixed(1)})' : ''}${drops > 0 ? '  $drops drops' : ''}';
      results.add(line);
      _engine.log(line, tag: 'VesperBench');
    }
    if (interrupted != null) {
      results.add(interrupted);
      _engine.log(interrupted, tag: 'VesperBench');
    }
    final newA = measured['Everything on'], newB = measured['Everything on (again)'];
    if (interrupted == null && newA != null && newB != null) {
      // The phone warms up during the run (GPU flat out at 60 fps): "Everything
      // on" is measured first and last, and each step is compared with the
      // baseline interpolated to its own position in the run.
      final iA = stepOf['Everything on']!, iB = stepOf['Everything on (again)']!;
      double baseline(String label) => newA + (newB - newA) * (stepOf[label]! - iA) / (iB - iA);
      double? delta(String label) => measured[label] == null ? null : measured[label]! - baseline(label);
      String fmt(double? v) => v == null ? '?' : v.toStringAsFixed(1);
      final hqLoad = _benchParts[2].$1, hqDemosaic = _benchParts[3].$1;
      final dLoad = delta(hqLoad), dDemosaic = delta(hqDemosaic), dHq = delta('All on, HQ x2');
      final summary = [
        'Warm-up drift during the run: ${newB - newA >= 0 ? '+' : ''}${(newB - newA).toStringAsFixed(1)} ms (corrected below)',
        'Pass costs (ms): ${[for (final p in _benchPasses) '$p ${fmt(delta('All on, $p x2'))}'].join(', ')}',
        'Align (ms): 1/4-res luma ${fmt(delta(_benchParts[0].$1))}, search ${fmt(delta(_benchParts[1].$1))}',
        if (delta(_benchAlignReduced) != null)
          'Align every 4th frame instead of 2nd: saves ${fmt(-delta(_benchAlignReduced)!)} ms',
        'HQ (ms): load ${fmt(dLoad)}, demosaic ${fmt(dDemosaic == null || dLoad == null ? null : dDemosaic - dLoad)}, '
            'filter+output ${fmt(dHq == null || dDemosaic == null ? null : dHq - dDemosaic)}',
        for (final (label, _) in _benchExperiments)
          if (delta(label) != null)
            () {
              final saved = -delta(label)!;
              return '${label.substring(5)}: ${saved >= 0 ? 'saves' : 'costs'} ${saved.abs().toStringAsFixed(1)} ms';
            }(),
      ];
      for (final line in summary) {
        results.add(line);
        _engine.log(line, tag: 'VesperBench');
      }
    }
    _endBenchmark();
    if (!mounted) return;
    _showBenchResults('GPU benchmark', results);
  }

  // Back to the user's settings after a benchmark / A/B test.
  void _endBenchmark() {
    _engine.setExperiments(0);
    _engine.setRepeatPass(-1);
    _engine.setAlignInterval(0);
    _engine.setOversampling(_oversampling);
    _engine.setSharpening(_sharpening);
    _engine.setTemporalNr(_temporalNr);
    _engine.setChromaNr(_chromaNr);
    _engine.setNrAlignment(_nrAlignment);
    _engine.setBudgetGuard(_gpuGuard);
    if (!mounted) return;
    setState(() {
      _benchmarking = false;
      _benchStep = '';
    });
  }

  void _showBenchResults(String title, List<String> lines) {
    final budget = 1000 / _fps;
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF14171D),
        scrollable: true, // long result list on a landscape phone
        title: Text(
          '$title · budget ${budget.toStringAsFixed(1)} ms @ ${_fpsLabel(_fps)} fps',
          style: const TextStyle(color: Colors.white, fontSize: 14),
        ),
        content: Text(
          lines.join('\n'),
          style: const TextStyle(color: Colors.white70, fontSize: 11, fontFamily: 'monospace'),
        ),
        actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('OK'))],
      ),
    );
  }

  // --- GPU A/B test (developer tool) ---------------------------------------
  // Each experiment against the normal GPU path, everything on, guard off, in
  // short alternating blocks: normal, exp 1, normal, exp 2, normal, … for 3
  // rounds. Every experiment block is compared with the normal blocks right
  // before and after it, so the phone heating up (which made the long
  // benchmark's experiment numbers unreliable: +14 ms drift on 5 Oct) hits
  // both sides alike. ~35 s at 60 fps.
  Future<void> _runGpuAbTest() async {
    if (_benchmarking || !_streaming || _recording) return;
    if (_benchExperiments.isEmpty) {
      _showBenchResults('GPU A/B test', ['No experiments to test in this build.']);
      return;
    }
    setState(() => _benchmarking = true);
    final generation = _streamGeneration;
    String? interrupted;
    Future<double?> block(int exps, String step) async {
      _engine.setOversampling(true);
      _engine.setSharpening(1);
      _engine.setTemporalNr(0.7);
      _engine.setChromaNr(0.5);
      _engine.setNrAlignment(true);
      _engine.setRepeatPass(-1);
      _engine.setAlignInterval(0);
      _engine.setExperiments(exps);
      _engine.setBudgetGuard(false);
      if (mounted) setState(() => _benchStep = step);
      await Future<void>.delayed(const Duration(milliseconds: 700)); // the smoothed GPU time settles in ~10 frames
      var sum = 0.0;
      for (var k = 0; k < 8; k++) {
        await Future<void>.delayed(const Duration(milliseconds: 100));
        sum += _engine.status()?.gpuMs ?? 0;
      }
      final st = _engine.status();
      if (!mounted || _streamGeneration != generation || !_streaming) {
        interrupted = 'Interrupted: the camera restarted (did the app leave the screen?). Run it again.';
        return null;
      }
      if (st != null && (st.alignThrottled || st.alignReduced || st.nrThrottled || (st.hqSupported && !st.hqAvailable))) {
        interrupted = 'Interrupted: the GPU guard had paused processing. Run it again.';
        return null;
      }
      return sum / 8;
    }

    const rounds = 3;
    final diffs = {for (final (label, _) in _benchExperiments) label: <double>[]};
    final first = await block(0, 'GPU A/B: normal');
    var prev = first;
    outer:
    for (var r = 0; r < rounds && prev != null; r++) {
      for (final (label, bit) in _benchExperiments) {
        final e = await block(bit, 'GPU A/B ${r + 1}/$rounds: ${label.substring(5)}');
        if (e == null) break outer;
        final n = await block(0, 'GPU A/B ${r + 1}/$rounds: normal');
        if (n == null) break outer;
        diffs[label]!.add(e - (prev! + n) / 2);
        prev = n;
      }
    }
    final results = <String>[];
    if (first != null && prev != null) {
      results.add('Everything on, normal: ${first.toStringAsFixed(1)} ms at the start, ${prev.toStringAsFixed(1)} ms at the end');
    }
    for (final (label, _) in _benchExperiments) {
      final d = diffs[label]!;
      if (d.isEmpty) continue;
      final mean = d.reduce((a, b) => a + b) / d.length;
      final runs = d.map((v) => '${v >= 0 ? '+' : ''}${v.toStringAsFixed(1)}').join(' ');
      results.add('${label.substring(5)}: ${mean >= 0 ? 'costs' : 'saves'} ${mean.abs().toStringAsFixed(1)} ms  (rounds: $runs)');
    }
    if (interrupted != null) results.add(interrupted!);
    for (final line in results) {
      _engine.log(line, tag: 'VesperAB');
    }
    _endBenchmark();
    if (!mounted) return;
    _showBenchResults('GPU A/B test', results);
  }

  // --- Native ISO analysis ------------------------------------------------
  String? get _isoAnalysisPath => _filesDir == null ? null : '$_filesDir/vesper_iso_$_cameraId.json';

  void _loadIsoAnalysis() {
    try {
      final path = _isoAnalysisPath;
      if (path == null) return;
      final f = File(path);
      if (!f.existsSync()) return;
      final j = jsonDecode(f.readAsStringSync()) as Map<String, dynamic>;
      if (j['device'] != _deviceModel || '${j['cameraId']}' != _cameraId) return;
      _isoAnalysis = IsoAnalysis.fromJson(j);
    } catch (_) {}
  }

  Future<void> _startIsoAnalysis() async {
    if (_recording || !_streaming || _isoAnalysisPath == null) return;
    final go = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF14171D),
        title: const Text('Native ISO analysis', style: TextStyle(color: Colors.white, fontSize: 15)),
        content: const Text(
          'Cover the lens completely: a lens cap, or a finger pressed flat over the lens with a dark cloth on top. '
          'Keep the phone still.\n\nThe app steps through every ISO and measures the sensor noise (~15 s). '
          'Your exposure settings are restored afterwards.',
          style: TextStyle(color: Colors.white70, fontSize: 12),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('CANCEL')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('START')),
        ],
      ),
    );
    if (go != true || !mounted) return;
    if (_engine.startIsoSweep(_isoAnalysisPath!, _deviceModel ?? 'unknown')) {
      setState(() => _isoSweepRunning = true);
    } else {
      _toast('Could not start the ISO analysis');
    }
  }

  void _onIsoSweepFinished(EngineStatus s) {
    _isoSweepRunning = false;
    if (s.isoSweepError.isNotEmpty) {
      if (s.isoSweepError != 'cancelled') _toast('ISO analysis failed: ${s.isoSweepError}');
      return;
    }
    _loadIsoAnalysis();
    final a = _isoAnalysis;
    if (a != null) {
      _engine.setNativeIsos(a.baseIso, a.hcgIso);
      _toast(a.summary);
    }
  }

  // --- Sensor calibration sweeps (tools/calibration/sensor.py) -------------
  Future<void> _startSensorSweep(bool dark) async {
    final dir = _calibrationDir;
    if (_recording || !_streaming || dir == null || _isoSweepRunning || _calSweepRunning) return;
    final go = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        backgroundColor: const Color(0xFF14171D),
        title: Text(dark ? 'Sensor calibration: dark' : 'Sensor calibration: white',
            style: const TextStyle(color: Colors.white, fontSize: 15)),
        content: Text(
          dark
              ? 'No light may reach the lens: lay the phone face down on a dark cloth (or use a lens cap and '
                  'cover it with a cloth). Don\'t move it until it finishes (~30-60 s).\n\n'
                  'The app steps through every ISO and measures black level, dark noise and hot pixels. '
                  'Your exposure settings are restored afterwards.'
              : 'Hold two layers of plain white printer paper flat against the lens and point the phone at '
                  'bright daylight: a window or the sky. Not a lamp (lamps flicker). Keep it still until it '
                  'finishes (~1-2 min).\n\nThe app picks the exposures itself and measures noise, lens shading, '
                  'the clip point and linearity. Your exposure settings are restored afterwards.',
          style: const TextStyle(color: Colors.white70, fontSize: 12),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('CANCEL')),
          TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('START')),
        ],
      ),
    );
    if (go != true || !mounted) return;
    final stamp = DateTime.now().toIso8601String().substring(0, 19).replaceAll(RegExp(r'[:T]'), '-');
    final base = '$dir/VSENSOR_${dark ? 'dark' : 'white'}_${_cameraId}_$stamp';
    if (_engine.startSensorSweep(dark: dark, basePath: base, deviceModel: _deviceModel ?? 'unknown')) {
      setState(() {
        _calSweepRunning = true;
        _calSweepDark = dark;
      });
    } else {
      _toast('Could not start the sensor calibration');
    }
  }

  void _onSensorSweepFinished(EngineStatus s) {
    _calSweepRunning = false;
    if (s.calSweepError.isNotEmpty) {
      if (s.calSweepError != 'cancelled') _toast('Sensor calibration failed: ${s.calSweepError}');
      return;
    }
    final base = s.calSweepResult;
    if (base.isEmpty) return;
    final name = base.split('/').last;
    final hot = _calSweepDark ? _saveSweepProfile(base) : null;
    _engine.publishCalibration(base).then((ok) async {
      await _engine.publishCalibration('${base}_ref');
      await _engine.publishCalibration('${base}_profile');
      if (!mounted) return;
      final saved = ok ? 'Saved $name.json to Downloads/Vesper Calibration' : 'Saved $name.json (app files only)';
      _toast(hot == null ? saved : 'Phone calibrated: noise per ISO + $hot hot pixels mapped. $saved');
    });
  }

  // Sub-label for the ISO dial: native (star), extended low (L), digital gain (D).
  String? _isoTag(int iso) {
    final a = _isoAnalysis;
    final digitalFrom = a?.digitalFromIso ?? 0;
    final maxAnalog = _caps?.maxAnalogIso ?? 0;
    if (a != null && a.nativeIsos.contains(iso)) return '★';
    if ((digitalFrom > 0 && iso >= digitalFrom) || (maxAnalog > 0 && iso > maxAnalog)) return 'D';
    if (a != null && iso < a.baseIso) return 'L';
    return null;
  }

  // --- Persisted settings -------------------------------------------------
  // Everything the user picks survives an app restart, like other camera apps.
  // Values are re-validated against the camera in _openAndStream().
  Map<String, Object> _settingsJson() => {
    'v': 1,
    'monitoringMode': _monitoringMode,
    'baseLutMode': _baseLutMode,
    'cropMode': _cropMode,
    'speedMode': _speedMode,
    'shutterAngle': _shutterAngle,
    'exposureNs': _exposureNs,
    'fps': _fps,
    'iso': _iso,
    'kelvin': _kelvin,
    'tint': _tint,
    'focus': _focus,
    'ois': _ois,
    'awbAuto': _awbAuto,
    'afContinuous': _afContinuous,
    'faceDetect': _faceDetect,
    'lensCorrection': _lensCorrection,
    'hotPixelFix': _hotPixelFix,
    'calibratedNoise': _calibratedNoise,
    'temporalNr': _temporalNr,
    'chromaNr': _chromaNr,
    'nrAlignment': _nrAlignment,
    'sharpening': _sharpening,
    'oversampling': _oversampling,
    'gpuGuard': _gpuGuard,
    'codec': _codec,
    'recordQuality': _recordQuality,
    'powerSaver': _powerSaver,
    'tapLocks': _tapLocks,
    'tapSetsExposure': _tapSetsExposure,
    'useProfile': _useProfile,
    'scopeMode': _scopeMode.index,
  };

  void _loadSettings() {
    try {
      final f = _settingsFile;
      if (f == null || !f.existsSync()) return;
      final j = jsonDecode(f.readAsStringSync()) as Map<String, dynamic>;
      T get<T>(String k, T fallback) {
        final v = j[k];
        if (v is T) return v;
        if (fallback is double && v is num) return v.toDouble() as T;
        if (fallback is int && v is num) return v.toInt() as T;
        return fallback;
      }

      _monitoringMode = get('monitoringMode', _monitoringMode).clamp(0, 4);
      _baseLutMode = get('baseLutMode', _baseLutMode).clamp(0, 1);
      _cropMode = get('cropMode', _cropMode).clamp(0, 1);
      _speedMode = get('speedMode', _speedMode);
      _shutterAngle = get('shutterAngle', _shutterAngle).clamp(0.5, 360.0);
      _exposureNs = get('exposureNs', _exposureNs).clamp(1000, 1000000000);
      _fps = get('fps', _fps);
      _iso = get('iso', _iso);
      _kelvin = get('kelvin', _kelvin).clamp(2000, 10000);
      _tint = get('tint', _tint).clamp(-50, 50);
      _focus = get('focus', _focus);
      _ois = get('ois', _ois);
      _awbAuto = get('awbAuto', _awbAuto);
      _afContinuous = get('afContinuous', _afContinuous);
      _faceDetect = get('faceDetect', _faceDetect);
      _lensCorrection = get('lensCorrection', _lensCorrection);
      _hotPixelFix = get('hotPixelFix', _hotPixelFix);
      _calibratedNoise = kDevTools ? get('calibratedNoise', _calibratedNoise) : true;
      _temporalNr = get('temporalNr', _temporalNr);
      _chromaNr = get('chromaNr', _chromaNr);
      _nrAlignment = get('nrAlignment', _nrAlignment);
      _sharpening = get('sharpening', _sharpening).clamp(0, 3);
      _oversampling = get('oversampling', _oversampling);
      _gpuGuard = kDevTools ? get('gpuGuard', _gpuGuard) : true; // release: the guard is always on
      _codec = get('codec', _codec).clamp(0, 1);
      _recordQuality = get('recordQuality', _recordQuality).clamp(0, 2);
      _powerSaver = get('powerSaver', _powerSaver).clamp(0, 2);
      _tapLocks = get('tapLocks', _tapLocks);
      _tapSetsExposure = get('tapSetsExposure', _tapSetsExposure);
      _useProfile = get('useProfile', _useProfile);
      _scopeMode = ScopeMode.values[get('scopeMode', _scopeMode.index).clamp(0, ScopeMode.values.length - 1)];
      _savedSettings = jsonEncode(_settingsJson());
    } catch (_) {
      // Corrupt or old file: keep defaults; it's rewritten on the next change.
    }
  }

  // Called from the status poll (4x/s) and when the app goes to the background:
  // writes only when something actually changed.
  void _saveSettings() {
    final f = _settingsFile;
    if (f == null) return;
    final json = jsonEncode(_settingsJson());
    if (json == _savedSettings) return;
    _savedSettings = json;
    try {
      final tmp = File('${f.path}.tmp')..writeAsStringSync(json, flush: true);
      tmp.renameSync(f.path); // atomic: a crash mid-write never loses settings
    } catch (_) {}
  }

  // Chart calibrations ship as assets/color_profiles/*.json (tools/calibration);
  // the one matching this phone model and camera id is used.
  Future<void> _loadColorProfile() async {
    try {
      final manifest = await AssetManifest.loadFromAssetBundle(rootBundle);
      for (final path in manifest.listAssets().where(
        (a) => a.startsWith('assets/color_profiles/') && a.endsWith('.json'),
      )) {
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

  // Sensor profile: dark-noise correction per ISO + static hot-pixel map.
  // Hot pixels differ from phone to phone, so the map only comes from this
  // phone's own Dark sweep (saved in app files, merged over runs). The noise
  // table describes the sensor model: a shipped one
  // (assets/sensor_profiles/*.json, tools/calibration/sensor.py --profile) is
  // used until this phone has measured its own.
  String? get _sensorProfilePath => _filesDir == null ? null : '$_filesDir/vesper_sensor_profile_$_cameraId.json';

  Map<String, dynamic>? _readSensorProfile(String text) {
    final j = jsonDecode(text) as Map<String, dynamic>;
    if (j['format'] != 'vesper-sensor-profile/1') return null;
    if (j['device'] != _deviceModel || '${j['cameraId']}' != _cameraId) return null;
    return j;
  }

  Future<void> _loadSensorProfile() async {
    Map<String, dynamic>? own, shipped;
    try {
      final path = _sensorProfilePath;
      if (path != null && File(path).existsSync()) own = _readSensorProfile(File(path).readAsStringSync());
    } catch (_) {}
    try {
      final manifest = await AssetManifest.loadFromAssetBundle(rootBundle);
      for (final path in manifest.listAssets().where(
        (a) => a.startsWith('assets/sensor_profiles/') && a.endsWith('.json'),
      )) {
        shipped = _readSensorProfile(await rootBundle.loadString(path));
        if (shipped != null) break;
      }
    } catch (_) {}
    List pick(String key) {
      final mine = (own?[key] as List?) ?? const [];
      return mine.isNotEmpty ? mine : ((shipped?[key] as List?) ?? const []);
    }
    _darkNoise = pick('darkNoise');
    _shotNoise = pick('shotNoise');
    _defects = (own?['defects'] as List?) ?? const [];
    _applySensorProfile();
  }

  // Noise tables (dark O, shot S: measured / camera, per ISO) and this phone's hot-pixel map.
  List _darkNoise = const [], _shotNoise = const [], _defects = const [];

  void _applySensorProfile() {
    List<double> col(List t, String k) => [for (final e in t.cast<Map<String, dynamic>>()) (e[k] as num).toDouble()];
    final dark = _calibratedNoise ? _darkNoise : const [], shot = _calibratedNoise ? _shotNoise : const [];
    _engine.setSensorProfile(col(dark, 'iso'), col(dark, 'factor'), _defects.map((e) => (e as num).toInt()).toList());
    _engine.setShotNoiseProfile(col(shot, 'iso'), col(shot, 'factor'));
  }

  // After a Dark sweep: its noise table replaces this phone's, its hot pixels
  // are added to the map (hot pixels don't heal; more runs catch more).
  int? _saveSweepProfile(String sweepBase) {
    try {
      final path = _sensorProfilePath;
      final f = File('${sweepBase}_profile.json');
      if (path == null || !f.existsSync()) return null;
      final fresh = _readSensorProfile(f.readAsStringSync());
      if (fresh == null) return null;
      final pairs = <(int, int)>{};
      void addPairs(List? xy) {
        if (xy == null) return;
        for (var i = 0; i + 1 < xy.length; i += 2) {
          pairs.add(((xy[i] as num).toInt(), (xy[i + 1] as num).toInt()));
        }
      }
      final old = File(path);
      if (old.existsSync()) addPairs(_readSensorProfile(old.readAsStringSync())?['defects'] as List?);
      addPairs(fresh['defects'] as List?);
      final defects = [for (final p in pairs) ...[p.$1, p.$2]];
      fresh['defects'] = defects;
      fresh['defectCount'] = pairs.length;
      old.writeAsStringSync(jsonEncode(fresh));
      final dark = (fresh['darkNoise'] as List?) ?? const [];
      if (dark.isNotEmpty) _darkNoise = dark;
      _defects = defects;
      _applySensorProfile();
      return pairs.length;
    } catch (_) {
      return null;
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
    _saveSettings();
    if (_settingsOpen) {
      // Viewfinder hidden: don't rebuild the camera screen. While recording
      // the status call still runs: it also drives the heat safeguard.
      if (_recording) _engine.status();
      return;
    }
    final s = _engine.status();
    if (s == null || !mounted) return;
    if (_recording && s.heatLevel >= 3 && !_heatWarned) {
      _heatWarned = true;
      _toast('Phone very hot: processing reduced to the minimum so the take can continue');
    }
    // While recording the GPU guard is always on; if even that can't hold
    // the frame rate, say so once per clip instead of silently dropping.
    if (_recording && s.gpuOverloaded && !_overloadWarned) {
      _overloadWarned = true;
      _toast('${_fps.round()} fps can\'t be sustained with these settings: frames are dropping. '
        'Lower the frame rate or turn off HQ / noise reduction.');
    }
    setState(() {
      _status = s;
      if (s.awbAuto) {
        _kelvin = s.kelvin.round();
        _tint = s.tint.round();
      }
      if (_afContinuous || s.focusPulling || s.focusLocked) _focus = s.focusDiopters;
      if (_scopeMode != ScopeMode.off) _scopes = _engine.scopes() ?? _scopes;
    });
    if (s.calibrationSaved.isNotEmpty && s.calibrationSaved != _lastCalibrationSaved) {
      _lastCalibrationSaved = s.calibrationSaved;
      _engine.publishCalibration(s.calibrationSaved).then((ok) {
        if (mounted) _toast(ok ? 'Saved to Downloads/Vesper Calibration' : 'Calibration frame saved (app files only)');
      });
    }
    if (_isoSweepRunning && s.isoSweep < 0) setState(() => _onIsoSweepFinished(s));
    if (_calSweepRunning && s.calSweep < 0) setState(() => _onSensorSweepFinished(s));
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
    RecordingFile? file;
    try {
      _engine.setRecordingQuality(1.0 + _recordQuality);
      file = await _engine.startRecording(codec: _codec);
    } on PlatformException catch (e) {
      _engine.log('Could not create the recording file: ${e.message}', tag: 'Vesper_UI');
    }
    if (!mounted) return;
    if (file == null) {
      _toast('Could not start recording (10-bit encoder unavailable?)');
      return;
    }
    setState(() => _recordingFile = file);
    _engine.lockRotation(true); // a clip never flips orientation mid-take
    _overloadWarned = false;
    _heatWarned = false;
    _armSaver();
  }

  // --- Recording power saver ---------------------------------------------
  // Only what the screen shows changes; the recording is computed as always.
  void _armSaver() {
    _saverTimer?.cancel();
    if (_powerSaver == 0 || !_recording) return;
    _saverTimer = Timer(_saverDelay, () {
      if (!mounted || !_recording || _powerSaver == 0) return;
      setState(() => _saverActive = true);
      _engine.setScreenBrightness(0.0);
      if (_powerSaver == 2) _engine.setViewfinderPaused(true);
    });
  }

  // Tap while the saver is active: back to normal for 10 s (the tap itself
  // does nothing else, so it can't hit a control by accident).
  void _wakeSaver({bool rearm = true}) {
    _saverTimer?.cancel();
    if (_saverActive) {
      _engine.setViewfinderPaused(false);
      _engine.setScreenBrightness(null);
      if (mounted) setState(() => _saverActive = false);
    }
    if (rearm) _armSaver();
  }

  // The black screen's REC text moves to another spot every minute, so a long
  // take can't leave a ghost of it on the OLED panel.
  static const _saverSpots = [
    Alignment(0, 0), Alignment(-0.5, -0.5), Alignment(0.5, 0.4), Alignment(-0.4, 0.5),
    Alignment(0.5, -0.4), Alignment(0, -0.6), Alignment(-0.6, 0), Alignment(0.6, 0.1),
  ];

  Widget _saverOverlay() {
    final viewfinderOff = _powerSaver == 2;
    final minute = (_status?.durationMs ?? 0) ~/ 60000;
    return Positioned.fill(
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: _wakeSaver,
        child: Container(
          color: viewfinderOff ? Colors.black : Colors.transparent,
          alignment: _saverSpots[minute % _saverSpots.length],
          child: viewfinderOff
              ? Text(
                  '●  REC  ${_timecode(_status?.durationMs ?? 0)}\nViewfinder paused to save power · tap to view',
                  textAlign: TextAlign.center,
                  style: const TextStyle(color: Colors.redAccent, fontSize: 14, height: 1.6, fontWeight: FontWeight.bold),
                )
              : null,
        ),
      ),
    );
  }

  Future<void> _finishRecording(String reason) async {
    _stopping = true;
    _wakeSaver(rearm: false);
    final file = _recordingFile;
    var published = true;
    if (file != null) {
      try {
        await _engine.finalizeRecording(file);
      } on PlatformException catch (e) {
        published = false; // recovered as a pending file at the next launch
        _engine.log('Could not publish ${file.name}: ${e.message}', tag: 'Vesper_UI');
      }
    }
    if (!mounted) return;
    setState(() {
      _recordingFile = null;
      _stopping = false;
    });
    _engine.lockRotation(false);
    switch (reason) {
      case 'thermal':
        _toast('Recording stopped: phone critically hot. Saved ${file?.name}');
      case 'storage':
        _toast('Recording stopped: storage almost full. Saved ${file?.name}');
      case final r when r.startsWith('error'):
        _toast('Recording stopped (${r.substring(r.indexOf(':') + 1).trim()}). Saved what was recorded: ${file?.name}');
      default:
        _toast(published ? 'Saved ${file?.name} to Movies/Vesper Cine' : 'Recording saved, but the gallery entry failed: restart the app');
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

  // Eyedropper: after PICK in the WB panel, the next viewfinder tap samples
  // white balance there instead of focusing.
  Future<void> _pickWhiteBalanceAt(Offset p) async {
    setState(() {
      _wbPickMode = false;
      _wbPickMark = p;
    });
    _engine.setAutoWhiteBalance(false);
    final r = await _engine.pickWhiteBalance(p.dx, p.dy);
    if (!mounted) return;
    Future<void>.delayed(const Duration(milliseconds: 1200), () {
      if (mounted) setState(() => _wbPickMark = null);
    });
    if (r == null) {
      _toast("Couldn't read white balance there — pick something white or grey and well lit");
      return;
    }
    setState(() {
      _awbAuto = false;
      _kelvin = r.$1.round();
      _tint = r.$2.round();
    });
    _toast('White balance picked: ${_kelvin}K, tint ${_tint > 0 ? '+' : ''}$_tint');
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
    return {
      minIso,
      ..._isoStops.where((i) => i > minIso && i < maxIso),
      ...?_isoAnalysis?.nativeIsos.where((i) => i >= minIso && i <= maxIso),
      maxIso,
    }.toList()..sort();
  }

  Future<void> _openSettings() async {
    setState(() => _activeWheel = OpenWheelType.none);
    // The viewfinder is hidden behind the page: give the GPU/CPU to the UI.
    _settingsOpen = true;
    _engine.setProcessingPaused(true);
    await Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => SettingsScreen(
          codec: _codec,
          onCodecChanged: (c) => setState(() => _codec = c),
          recordQuality: _recordQuality,
          fps: _fps,
          onRecordQualityChanged: (q) => setState(() => _recordQuality = q),
          cropMode: _cropMode,
          onCropModeChanged: (m) async => await _toggleCrop(),
          isRecording: _recording,
          powerSaver: _powerSaver,
          onPowerSaverChanged: (v) {
            setState(() => _powerSaver = v);
            if (_recording) {
              _wakeSaver(); // apply the new choice from now on
            }
          },
          lensCorrection: _lensCorrection,
          onLensCorrectionChanged: (val) {
            setState(() => _lensCorrection = val);
            _engine.setLensCorrection(val);
          },
          hotPixelFix: _hotPixelFix,
          onHotPixelFixChanged: (val) {
            setState(() => _hotPixelFix = val);
            _engine.setHotPixelFix(val);
          },
          temporalNr: _temporalNr,
          onTemporalNrChanged: (val) {
            setState(() => _temporalNr = val);
            _engine.setTemporalNr(val);
          },
          nrAlignment: _nrAlignment,
          onNrAlignmentChanged: (val) {
            setState(() => _nrAlignment = val);
            _engine.setNrAlignment(val);
          },
          calibratedNoise: _calibratedNoise,
          onCalibratedNoiseChanged: (val) {
            setState(() => _calibratedNoise = val);
            _applySensorProfile();
          },
          gpuGuard: _gpuGuard,
          onGpuGuardChanged: (val) {
            setState(() => _gpuGuard = val);
            _engine.setBudgetGuard(val);
          },
          oversampling: _oversampling,
          onOversamplingChanged: (val) {
            setState(() => _oversampling = val);
            _engine.setOversampling(val);
          },
          sharpening: _sharpening,
          onSharpeningChanged: (val) {
            setState(() => _sharpening = val);
            _engine.setSharpening(val);
          },
          chromaNr: _chromaNr,
          onChromaNrChanged: (val) {
            setState(() => _chromaNr = val);
            _engine.setChromaNr(val);
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
          },
          onCaptureCalibration: _streaming && _calibrationDir != null ? _captureCalibration : null,
          onRunGpuBenchmark: _streaming && !_recording
              ? () {
                  Navigator.of(context).pop();
                  _runGpuBenchmark();
                }
              : null,
          onRunGpuAbTest: _streaming && !_recording
              ? () {
                  Navigator.of(context).pop();
                  _runGpuAbTest();
                }
              : null,
          tapLocks: _tapLocks,
          onTapLocksChanged: (val) => setState(() => _tapLocks = val),
          tapSetsExposure: _tapSetsExposure,
          onTapSetsExposureChanged: (val) => setState(() => _tapSetsExposure = val),
          isoAnalysisSummary: _isoAnalysis?.summary ?? '',
          onAnalyzeIso: _streaming && !_recording
              ? () {
                  Navigator.of(context).pop();
                  _startIsoAnalysis();
                }
              : null,
          onSensorSweep: _streaming && !_recording && _calibrationDir != null
              ? (dark) {
                  Navigator.of(context).pop();
                  _startSensorSweep(dark);
                }
              : null,
          faceDetect: _faceDetect,
          onFaceDetectChanged: (val) {
            setState(() => _faceDetect = val);
            _engine.setFaceDetection(val);
          },
        ),
      ),
    );
    _settingsOpen = false;
    _engine.setProcessingPaused(false);
  }

  // Bottom-bar status line. Paused stages say why: the GPU guard paused them
  // (they resume automatically) or the GPU can't run them at all.
  String _telemetry(EngineStatus s) {
    final parts = <String>['${s.fps.toStringAsFixed(_recording ? 0 : 1)} FPS'];
    if (s.gpuMs > 0) parts.add('GPU ${s.gpuMs.toStringAsFixed(1)}ms');
    if (_oversampling) {
      parts.add(!s.hqSupported ? 'HQ N/A' : (s.hqAvailable ? 'HQ' : 'HQ PAUSED'));
    }
    if (_temporalNr > 0 && _nrAlignment && s.alignThrottled) parts.add('ALIGN PAUSED');
    if (_temporalNr > 0 && _nrAlignment && s.alignReduced) parts.add('ALIGN REDUCED');
    if ((_temporalNr > 0 || _chromaNr > 0) && s.nrThrottled) parts.add('NR PAUSED');
    if (s.gpuOverloaded) parts.add('FPS NOT SUSTAINABLE');
    final drops = s.cameraDrops + s.framesDropped;
    if (drops > 0) parts.add('$drops DROP');
    return parts.join(' · ');
  }

  // Focus magnifier geometry: the viewfinder is scaled by _magScale about
  // _magCenter (normalised). Taps map back to the real frame position.
  Offset _unmagnify(Offset p) => _magnify ? _magCenter + (p - _magCenter) / _magScale : p;
  Offset _magnifyPoint(Offset p) => _magnify ? _magCenter + (p - _magCenter) * _magScale : p;

  // Tap: hardware AF (PDAF + laser) on the region, either tracking (AF-C) or one
  // scan that the HAL then holds (AF-L). Optionally also spot-meters exposure there.
  void _tapToFocus(Offset p, {bool lock = false}) {
    if (!_streaming || _isoSweepRunning || _calSweepRunning || _benchmarking) return; // a measurement owns the camera
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
    final gen = ++_aeGen;
    (int, int)? r;
    for (var pass = 0; pass < 3; pass++) {
      (int, int)? next;
      for (var i = 0; i < 10 && next == null; i++) {
        next = _engine.autoExpose(keepShutter: keepShutter, clean: true);
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

  void _toggleWheel(OpenWheelType type) {
    setState(() {
      _activeWheel = (_activeWheel == type) ? OpenWheelType.none : type;
    });
  }

  @override
  Widget build(BuildContext context) => Stack(
    fit: StackFit.expand,
    children: [_buildCamera(context), if (_saverActive) _saverOverlay()],
  );

  Widget _buildCamera(BuildContext context) {
    final s = _status;
    final heat = s?.heatLevel ?? 0;
    final speedList = _buildSpeedList();
    final angleList = _buildAngleList();
    final isoList = _buildIsoList();
    final fpsList = _fpsOptions;
    final maxD = _caps?.minFocusDiopters ?? 10.0;

    WidgetsBinding.instance.addPostFrameCallback((_) => _syncViewfinder());
    return Scaffold(
      // Transparent: the viewfinder is a native surface underneath the Flutter
      // UI (the window behind is black), shown through the hole at _vfKey.
      backgroundColor: Colors.transparent,
      body: GestureDetector(
        behavior: HitTestBehavior.translucent,
        onTap: () {
          if (_activeWheel != OpenWheelType.none) {
            setState(() => _activeWheel = OpenWheelType.none);
          }
        },
        child: Stack(
          children: [
            // 1. Center Viewfinder: exact output aspect (16:9 or 4:3 open gate)
            Center(
              child: AspectRatio(
                aspectRatio: _cropMode == 0 ? 16 / 9 : 4 / 3,
                child: Container(
                  decoration: BoxDecoration(
                    // Same width recording or not: a width change resizes the
                    // viewfinder box, and resizing the native swapchain stalls
                    // the camera for ~80 ms (a dropped frame as recording starts).
                    border: Border.all(
                      color: _recording ? Colors.redAccent : Colors.white12,
                      width: 2.5,
                    ),
                  ),
                  child: _textureId != null
                      ? LayoutBuilder(
                          builder: (ctx, box) => GestureDetector(
                            behavior: HitTestBehavior.opaque,
                            onTapUp: (d) {
                              final p = _unmagnify(
                                Offset(d.localPosition.dx / box.maxWidth, d.localPosition.dy / box.maxHeight),
                              );
                              if (_wbPickMode) {
                                _pickWhiteBalanceAt(p);
                              } else if (_activeWheel != OpenWheelType.none) {
                                setState(() => _activeWheel = OpenWheelType.none);
                              } else {
                                _tapToFocus(p);
                              }
                            },
                            onLongPressStart: (d) {
                              final p = _unmagnify(
                                Offset(d.localPosition.dx / box.maxWidth, d.localPosition.dy / box.maxHeight),
                              );
                              _wbPickMode ? _pickWhiteBalanceAt(p) : _tapToFocus(p, lock: true);
                            },
                            child: Stack(
                              children: [
                                // Native viewfinder shows through here (magnifier applied natively).
                                Positioned.fill(child: SizedBox.expand(key: _vfKey)),
                                if (_magnify)
                                  const Positioned(
                                    left: 8,
                                    top: 8,
                                    child: Text(
                                      'MAG 3×',
                                      style: TextStyle(color: Colors.amber, fontSize: 10, fontWeight: FontWeight.bold),
                                    ),
                                  ),
                                if (_faceDetect && !_magnify && (s?.face[2] ?? 0) > 0)
                                  Positioned(
                                    left: s!.face[0] * box.maxWidth,
                                    top: s.face[1] * box.maxHeight,
                                    width: s.face[2] * box.maxWidth,
                                    height: s.face[3] * box.maxHeight,
                                    child: Container(
                                      decoration: BoxDecoration(border: Border.all(color: Colors.amber, width: 1.5)),
                                    ),
                                  ),
                                if (_scopeMode != ScopeMode.off)
                                  Positioned(
                                    right: 8,
                                    bottom: 8,
                                    child: ScopesOverlay(scopes: _scopes, mode: _scopeMode),
                                  ),
                                if (_benchmarking)
                                  Positioned(
                                    left: 0,
                                    right: 0,
                                    top: 12,
                                    child: Center(
                                      child: Container(
                                        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                                        color: const Color(0xCC000000),
                                        child: Text(
                                          _benchStep,
                                          style: const TextStyle(
                                            color: Colors.amber,
                                            fontSize: 10,
                                            fontWeight: FontWeight.bold,
                                          ),
                                        ),
                                      ),
                                    ),
                                  ),
                                if (_calSweepRunning)
                                  Positioned.fill(
                                    child: Container(
                                      color: const Color(0x99000000),
                                      alignment: Alignment.center,
                                      child: Column(
                                        mainAxisSize: MainAxisSize.min,
                                        children: [
                                          Text(
                                            'SENSOR CALIBRATION (${_calSweepDark ? 'DARK' : 'WHITE'})… '
                                            '${(((s?.calSweep ?? 0).clamp(0.0, 1.0)) * 100).round()}%',
                                            style: const TextStyle(
                                              color: Colors.amber,
                                              fontSize: 12,
                                              fontWeight: FontWeight.bold,
                                            ),
                                          ),
                                          const SizedBox(height: 4),
                                          Text(
                                            s?.calSweepStage ?? '',
                                            style: const TextStyle(color: Colors.white, fontSize: 10),
                                          ),
                                          Text(
                                            _calSweepDark
                                                ? 'Keep the lens covered and the phone still'
                                                : 'Keep the paper on the lens, aimed at daylight, phone still',
                                            style: const TextStyle(color: Colors.white70, fontSize: 10),
                                          ),
                                          TextButton(
                                            onPressed: _engine.cancelSensorSweep,
                                            child: const Text('CANCEL', style: TextStyle(color: Colors.white)),
                                          ),
                                        ],
                                      ),
                                    ),
                                  ),
                                if (_isoSweepRunning)
                                  Positioned.fill(
                                    child: Container(
                                      color: const Color(0x99000000),
                                      alignment: Alignment.center,
                                      child: Column(
                                        mainAxisSize: MainAxisSize.min,
                                        children: [
                                          Text(
                                            'ANALYZING SENSOR ISO… ${(((s?.isoSweep ?? 0).clamp(0.0, 1.0)) * 100).round()}%',
                                            style: const TextStyle(
                                              color: Colors.amber,
                                              fontSize: 12,
                                              fontWeight: FontWeight.bold,
                                            ),
                                          ),
                                          const SizedBox(height: 4),
                                          const Text(
                                            'Keep the lens covered and the phone still',
                                            style: TextStyle(color: Colors.white70, fontSize: 10),
                                          ),
                                          TextButton(
                                            onPressed: _engine.cancelIsoSweep,
                                            child: const Text('CANCEL', style: TextStyle(color: Colors.white)),
                                          ),
                                        ],
                                      ),
                                    ),
                                  ),
                                if (_wbPickMode)
                                  Positioned(
                                    left: 0,
                                    right: 0,
                                    top: 12,
                                    child: Center(
                                      child: GestureDetector(
                                        onTap: () => setState(() => _wbPickMode = false),
                                        child: Container(
                                          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
                                          decoration: BoxDecoration(
                                            color: const Color(0xCC000000),
                                            borderRadius: BorderRadius.circular(4),
                                            border: Border.all(color: Colors.amber),
                                          ),
                                          child: const Text(
                                            'TAP SOMETHING WHITE OR GREY TO SET WB  ·  ✕',
                                            style: TextStyle(
                                              color: Colors.amber,
                                              fontSize: 10,
                                              fontWeight: FontWeight.bold,
                                            ),
                                          ),
                                        ),
                                      ),
                                    ),
                                  ),
                                if (_wbPickMark != null)
                                  Positioned(
                                    left: _wbPickMark!.dx * box.maxWidth - 14,
                                    top: _wbPickMark!.dy * box.maxHeight - 14,
                                    child: const Icon(Icons.colorize_rounded, color: Colors.amber, size: 28),
                                  ),
                                if (_focusMark != null)
                                  Positioned(
                                    left: _magnifyPoint(_focusMark!).dx * box.maxWidth - 30,
                                    top: _magnifyPoint(_focusMark!).dy * box.maxHeight - 30,
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
                          child: GestureDetector(
                            onTap: _permissionDenied ? _start : null,
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
            ),

            // Central crosshair overlay
            const Center(
              child: SizedBox(width: 16, height: 16, child: CustomPaint(painter: CrosshairPainter())),
            ),

            // 2. TOP BAR (shifted rightwards beyond left rack to give left rack full headroom)
            Positioned(
              left: 88,
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
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
                    child: Row(
                      children: [
                        // Left group scrolls sideways if the bar gets too narrow.
                        Flexible(
                          child: SingleChildScrollView(
                            scrollDirection: Axis.horizontal,
                            child: Row(
                              mainAxisSize: MainAxisSize.min,
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
                                  _baseLutMode == 1 ? 'REC.709 LUT' : 'LOG VIEW',
                                  active: _baseLutMode == 1,
                                  onTap: () {
                                    setState(() {
                                      _baseLutMode = (_baseLutMode == 1) ? 0 : 1;
                                      _monitoringMode = _baseLutMode;
                                    });
                                    _engine.setMonitoringMode(_monitoringMode);
                                  },
                                ),
                                const SizedBox(width: 10),

                                // Separate exposure/focus assistance denoters:
                                // FC (False Color = 2), PEAK (Peaking = 3), ZEBRA (Zebras = 4)
                                // When toggled off, correctly returns to _baseLutMode (0 = Log, 1 = Rec.709)
                                _toolDenoter('FC', _monitoringMode == 2, () {
                                  setState(() {
                                    _monitoringMode = (_monitoringMode == 2) ? _baseLutMode : 2;
                                  });
                                  _engine.setMonitoringMode(_monitoringMode);
                                }),
                                const SizedBox(width: 4),
                                _toolDenoter('PEAK', _monitoringMode == 3, () {
                                  setState(() {
                                    _monitoringMode = (_monitoringMode == 3) ? _baseLutMode : 3;
                                  });
                                  _engine.setMonitoringMode(_monitoringMode);
                                }),
                                const SizedBox(width: 4),
                                _toolDenoter('MAG', _magnify, () {
                                  setState(() {
                                    _magnify = !_magnify;
                                    if (_magnify) _magCenter = _focusMark ?? const Offset(0.5, 0.5);
                                  });
                                }),
                                const SizedBox(width: 4),
                                _toolDenoter('ZEBRA', _monitoringMode == 4, () {
                                  setState(() {
                                    _monitoringMode = (_monitoringMode == 4) ? _baseLutMode : 4;
                                  });
                                  _engine.setMonitoringMode(_monitoringMode);
                                }),
                              ],
                            ),
                          ),
                        ),
                        const SizedBox(width: 8),

                        // Hardware / thermal warning
                        if (heat >= 1) ...[
                          _badge(const ['', 'WARM', 'HOT', 'VERY HOT'][heat.clamp(0, 3)],
                              heat >= 3 ? Colors.redAccent : Colors.orangeAccent),
                          const SizedBox(width: 6),
                        ],

                        // Recording status & timecode
                        if (_recording) ...[
                          FadeTransition(
                            opacity: _pulse,
                            child: Container(
                              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                              decoration: BoxDecoration(
                                color: Colors.red.shade900,
                                borderRadius: BorderRadius.circular(4),
                                border: Border.all(color: Colors.redAccent),
                              ),
                              child: Row(
                                mainAxisSize: MainAxisSize.min,
                                children: [
                                  Container(
                                    width: 6,
                                    height: 6,
                                    decoration: const BoxDecoration(color: Colors.white, shape: BoxShape.circle),
                                  ),
                                  const SizedBox(width: 5),
                                  Text(
                                    _timecode(s?.durationMs ?? 0),
                                    style: const TextStyle(
                                      color: Colors.white,
                                      fontFamily: 'monospace',
                                      fontWeight: FontWeight.bold,
                                      fontSize: 11,
                                    ),
                                  ),
                                ],
                              ),
                            ),
                          ),
                          const SizedBox(width: 4),
                        ],

                        // Settings button
                        IconButton(
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints(minWidth: 32, minHeight: 32),
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

            // 3. LEFT CONTROL RACK (utilizing top headroom now that top bar starts at left: 92)
            Positioned(
              left: 12,
              top: 10,
              bottom: 34,
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
                      onTap: () => _toggleWheel(OpenWheelType.fps),
                    ),

                    // SHUTTER control
                    CineControlTile(
                      label: 'SHUTTER',
                      value: _speedMode ? _speedLabel(_exposureNs) : _angleLabel(_shutterAngle),
                      subtitle: _speedMode ? 'SPD' : 'ANG',
                      active: _activeWheel == OpenWheelType.shutter,
                      onTap: () => _toggleWheel(OpenWheelType.shutter),
                      onLongPress: () {
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

                    // ISO control
                    CineControlTile(
                      label: 'ISO',
                      value: '$_iso',
                      subtitle: switch (_isoTag(_iso)) {
                        '★' => '★ NATIVE',
                        'D' => 'DIGITAL',
                        'L' => 'EXT LOW',
                        _ => null,
                      },
                      active: _activeWheel == OpenWheelType.iso,
                      onTap: () => _toggleWheel(OpenWheelType.iso),
                    ),

                    // WHITE BALANCE control (opens panel with Google AWB switch, Kelvin wheel & Tint slider)
                    CineControlTile(
                      label: 'WB',
                      value: '${_kelvin}K',
                      subtitle: _awbAuto ? 'AUTO' : '${_tint > 0 ? '+' : ''}$_tint',
                      active: _activeWheel == OpenWheelType.wb,
                      onTap: () => _toggleWheel(OpenWheelType.wb),
                    ),

                    // FOCUS control (opens panel with Manual/AF-C, Faces, Tap locks & distance slider)
                    CineControlTile(
                      label: 'FOCUS',
                      value: _focusLabel,
                      subtitle: _afContinuous ? 'AF-C' : 'MAN',
                      active: _activeWheel == OpenWheelType.focus,
                      accentColor: _afLanded ? Colors.greenAccent : null,
                      onTap: (_caps?.minFocusDiopters ?? 0) > 0 ? () => _toggleWheel(OpenWheelType.focus) : null,
                    ),

                    // SCOPES: cycles OFF -> HIST -> WAVE -> H + W (overlay, never blocks taps)
                    CineControlTile(
                      label: 'SCOPE',
                      value: _scopeMode.label,
                      active: _scopeMode != ScopeMode.off,
                      onTap: () {
                        setState(() {
                          _scopeMode = ScopeMode.values[(_scopeMode.index + 1) % ScopeMode.values.length];
                          if (_scopeMode == ScopeMode.off) _scopes = null;
                        });
                        _engine.setScopes(_scopeMode != ScopeMode.off);
                      },
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
                  ],
                ),
              ),
            ),

            // 4. FLOATING DOCK PANEL / WHEEL DIAL (Only ONE visible at a time)
            if (_activeWheel != OpenWheelType.none)
              Positioned(
                left: 92,
                top: _activeWheel == OpenWheelType.fps
                    ? 18
                    : _activeWheel == OpenWheelType.shutter
                    ? 50
                    : _activeWheel == OpenWheelType.iso
                    ? 85
                    : _activeWheel == OpenWheelType.wb
                    ? 110
                    : 140,
                child: _buildActiveWheelDial(speedList, angleList, isoList, fpsList, maxD),
              ),

            // 5. RIGHT SIDE RECORD BUTTON
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
                      border: Border.all(color: _recording ? Colors.redAccent : Colors.white70, width: 3.5),
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

            // 6. BOTTOM BAR (subtle cinematic gradient so 16:9/open-gate frame remains visible, with safe area padding for curved corners)
            Positioned(
              left: 0,
              right: 0,
              bottom: 0,
              child: Container(
                decoration: const BoxDecoration(
                  gradient: LinearGradient(
                    begin: Alignment.bottomCenter,
                    end: Alignment.topCenter,
                    colors: [Color(0x99000000), Color(0x00000000)],
                  ),
                ),
                child: SafeArea(
                  top: false,
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 28, vertical: 6),
                    child: Row(
                      children: [
                        Text(
                          '${_codec == 0 ? 'HEVC 10-BIT' : 'AV1 10-BIT'} · ${_cropMode == 0 ? '16:9' : '4:3 OPEN GATE'}',
                          style: const TextStyle(
                            color: Colors.white70,
                            fontSize: 10,
                            fontFamily: 'monospace',
                            fontWeight: FontWeight.w600,
                            letterSpacing: 0.8,
                          ),
                        ),
                        const SizedBox(width: 10),
                        if (_useProfile && _profileAvailable)
                          const Text(
                            '· CAL ACTIVE',
                            style: TextStyle(
                              color: Colors.amber,
                              fontSize: 10,
                              fontFamily: 'monospace',
                              fontWeight: FontWeight.bold,
                            ),
                          ),
                        // Live engine telemetry (moved here from the top bar so it's never cut off).
                        Expanded(
                          child: s == null
                              ? const SizedBox()
                              : Text(
                                  _telemetry(s),
                                  textAlign: TextAlign.center,
                                  maxLines: 1,
                                  overflow: TextOverflow.ellipsis,
                                  style: TextStyle(
                                    color: s.cameraDrops + s.framesDropped > 0 ? Colors.orangeAccent : Colors.white60,
                                    fontSize: 10,
                                    fontFamily: 'monospace',
                                  ),
                                ),
                        ),
                        if (s != null)
                          Text(
                            'AUDIO ${s.audio ? 'OK' : 'OFF'}',
                            style: TextStyle(
                              color: s.audio ? Colors.greenAccent : Colors.white38,
                              fontSize: 10,
                              fontFamily: 'monospace',
                              fontWeight: FontWeight.bold,
                            ),
                          ),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildActiveWheelDial(
    List<int> speedList,
    List<double> angleList,
    List<int> isoList,
    List<double> fpsList,
    double maxD,
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
          subLabel: _isoTag,
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

      case OpenWheelType.wb:
        return CineWbPanel(
          kelvin: _kelvin,
          tint: _tint,
          awbAuto: _awbAuto,
          onKelvinChanged: (k) {
            setState(() {
              _kelvin = k;
              _awbAuto = false;
            });
            _engine.setAutoWhiteBalance(false);
            _engine.setKelvinTint(_kelvin, _tint);
          },
          onTintChanged: (t) {
            setState(() {
              _tint = t;
              _awbAuto = false;
            });
            _engine.setAutoWhiteBalance(false);
            _engine.setKelvinTint(_kelvin, _tint);
          },
          onAwbAutoChanged: (auto) {
            setState(() => _awbAuto = auto);
            _engine.setAutoWhiteBalance(_awbAuto);
            if (!_awbAuto) _engine.setKelvinTint(_kelvin, _tint);
          },
          onClose: () => setState(() => _activeWheel = OpenWheelType.none),
          onPick: _streaming
              ? () => setState(() {
                  _activeWheel = OpenWheelType.none;
                  _wbPickMode = true;
                })
              : null,
        );

      case OpenWheelType.focus:
        return CineFocusPanel(
          focus: _focus,
          maxFocus: maxD,
          afContinuous: _afContinuous,
          faceDetect: _faceDetect,
          tapLocks: _tapLocks,
          tapSetsExposure: _tapSetsExposure,
          distanceLabel: _distanceLabel,
          onFocusChanged: (f) {
            setState(() {
              _focus = f;
              _afContinuous = false;
            });
            _engine.setContinuousFocus(false);
            _engine.setFocus(_focus);
          },
          onContinuousChanged: (c) {
            setState(() => _afContinuous = c);
            _engine.setContinuousFocus(_afContinuous);
          },
          onFaceDetectChanged: (faces) {
            setState(() => _faceDetect = faces);
            _engine.setFaceDetection(_faceDetect);
          },
          onTapLocksChanged: (lock) => setState(() => _tapLocks = lock),
          onTapSetsExposureChanged: (exp) => setState(() => _tapSetsExposure = exp),
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
