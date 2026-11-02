import 'dart:convert';

import 'vesper_native.dart';

/// Device check (0.27.0): is this phone able to run Vesper Cine, and at which
/// frame rates and quality? Everything here is plain Dart without platform
/// calls, so it can be unit-tested: the static checks (camera, GPU, encoder),
/// the frame-rate classification, the result file and the rules for showing
/// frame rates. The live measurement loop is [runLiveTest]; the camera screen
/// supplies the engine through [LiveEnv].

/// Bump when the check itself changes in a way old results can't be trusted
/// (new tests, different thresholds): every phone then re-runs it once.
const int kDeviceCheckVersion = 1;
const String kDeviceCheckFormat = 'vesper-device-check/1';

/// The frame rates measured live (16:9 mode); other rates take the class of
/// the nearest measured one.
const List<int> kCheckFps = [24, 30, 48, 60];

/// A frame rate fits at full quality below this share of its frame time
/// (the same line as the GPU guard).
const double kBudgetShare = 0.85;

/// Dropped frames tolerated in a short measurement window with the guard on
/// (one stray frame is a hiccup, not a limit).
const int kGuardDropTolerance = 1;

enum CheckLevel { ok, info, warn, fail }

enum FpsClass { full, reduced, unsupported }

/// Developer switch (Settings › Developer): produce a fake result to see the screens.
enum Pretend { off, unsupported, slow }

/// One line of the result screen.
class CheckItem {
  final String id;
  final CheckLevel level;
  final String title; // short, e.g. "Camera"
  final String text; // plain English
  const CheckItem(this.id, this.level, this.title, this.text);

  Map<String, dynamic> toJson() => {'id': id, 'level': level.name, 'title': title, 'text': text};

  factory CheckItem.fromJson(Map<String, dynamic> j) => CheckItem(
    j['id'] as String? ?? '',
    CheckLevel.values.firstWhere((l) => l.name == j['level'], orElse: () => CheckLevel.info),
    j['title'] as String? ?? '',
    j['text'] as String? ?? '',
  );
}

/// Measurement at one frame rate.
class FpsResult {
  final int fps;
  final double gpuMs; // average GPU time per frame, guard off (0 = the GPU gave no timing)
  final double budgetMs;
  final int drops; // dropped frames in the guard-off window
  final int? guardDrops; // ... with the guard on (null: not needed, it already fits)
  final FpsClass cls;
  const FpsResult(this.fps, this.gpuMs, this.budgetMs, this.drops, this.guardDrops, this.cls);

  Map<String, dynamic> toJson() => {
    'fps': fps,
    'gpuMs': double.parse(gpuMs.toStringAsFixed(2)),
    'budgetMs': double.parse(budgetMs.toStringAsFixed(2)),
    'drops': drops,
    'guardDrops': guardDrops,
    'class': cls.name,
  };

  factory FpsResult.fromJson(Map<String, dynamic> j) => FpsResult(
    (j['fps'] as num).toInt(),
    (j['gpuMs'] as num?)?.toDouble() ?? 0,
    (j['budgetMs'] as num?)?.toDouble() ?? 0,
    (j['drops'] as num?)?.toInt() ?? 0,
    (j['guardDrops'] as num?)?.toInt(),
    FpsClass.values.firstWhere((c) => c.name == j['class'], orElse: () => FpsClass.unsupported),
  );

  /// The log line the owner reads back from the app log.
  String logLine() =>
      'Device check: $fps fps: GPU ${gpuMs.toStringAsFixed(1)} ms (budget ${budgetMs.toStringAsFixed(1)}), '
      'drops $drops, guard-on drops ${guardDrops ?? '-'} -> ${cls.name}';
}

/// Whole result, saved as `vesper_device_check.json`.
class DeviceCheckResult {
  final int checkVersion;
  final String model;
  final String androidVersion; // e.g. "16 (SDK 36)"
  final String appVersion;
  final String appBuild;
  final String cameraId;
  final String timestamp;
  final List<CheckItem> items;
  final List<FpsResult> fps;
  final bool liveRan;
  final bool measuredWarm;
  final Pretend pretend;

  const DeviceCheckResult({
    this.checkVersion = kDeviceCheckVersion,
    required this.model,
    required this.androidVersion,
    this.appVersion = '',
    this.appBuild = '',
    required this.cameraId,
    required this.timestamp,
    required this.items,
    this.fps = const [],
    this.liveRan = false,
    this.measuredWarm = false,
    this.pretend = Pretend.off,
  });

  /// No blocking problem: static checks passed and the GPU can do at least one frame rate.
  bool get supported =>
      !items.any((i) => i.level == CheckLevel.fail) &&
      (!liveRan || fps.isEmpty || fps.any((f) => f.cls != FpsClass.unsupported));

  List<CheckItem> get problems => items.where((i) => i.level == CheckLevel.fail).toList();
  List<CheckItem> get warnings => items.where((i) => i.level == CheckLevel.warn).toList();

  /// Class of any frame rate: the nearest measured one (ties go to the higher, the safer
  /// side); null before the live test or when nothing was measured.
  FpsClass? classFor(double f) {
    if (fps.isEmpty) return null;
    FpsResult best = fps.first;
    for (final r in fps) {
      final d = (r.fps - f).abs(), bd = (best.fps - f).abs();
      if (d < bd || (d == bd && r.fps > best.fps)) best = r;
    }
    return best.cls;
  }

  Map<String, dynamic> toJson() => {
    'format': kDeviceCheckFormat,
    'checkVersion': checkVersion,
    'model': model,
    'androidVersion': androidVersion,
    'appVersion': appVersion,
    'appBuild': appBuild,
    'cameraId': cameraId,
    'timestamp': timestamp,
    'static': {
      'supported': items.every((i) => i.level != CheckLevel.fail),
      'checks': [for (final i in items) i.toJson()],
    },
    'fps': [for (final f in fps) f.toJson()],
    'liveRan': liveRan,
    'measuredWarm': measuredWarm,
    'pretend': pretend.name,
  };

  String encode() => const JsonEncoder.withIndent('  ').convert(toJson());

  /// Null when [text] isn't a result file of this format.
  static DeviceCheckResult? decode(String text) {
    try {
      final j = jsonDecode(text) as Map<String, dynamic>;
      if (j['format'] != kDeviceCheckFormat) return null;
      final st = (j['static'] as Map<String, dynamic>?) ?? const {};
      return DeviceCheckResult(
        checkVersion: (j['checkVersion'] as num?)?.toInt() ?? 0,
        model: j['model'] as String? ?? '',
        androidVersion: j['androidVersion'] as String? ?? '',
        appVersion: j['appVersion'] as String? ?? '',
        appBuild: j['appBuild'] as String? ?? '',
        cameraId: j['cameraId'] as String? ?? '',
        timestamp: j['timestamp'] as String? ?? '',
        items: [for (final c in (st['checks'] as List? ?? const [])) CheckItem.fromJson(c as Map<String, dynamic>)],
        fps: [for (final f in (j['fps'] as List? ?? const [])) FpsResult.fromJson(f as Map<String, dynamic>)],
        liveRan: j['liveRan'] == true,
        measuredWarm: j['measuredWarm'] == true,
        pretend: Pretend.values.firstWhere((p) => p.name == j['pretend'], orElse: () => Pretend.off),
      );
    } catch (_) {
      return null;
    }
  }
}

/// Re-run on launch only when there is no usable result, the check itself was
/// improved ([kDeviceCheckVersion]), or the phone changed underneath it (Android
/// update, other model or camera). NOT on every app update.
bool deviceCheckDue(DeviceCheckResult? saved, {required String model, required String androidVersion, required String cameraId}) {
  if (saved == null) return true;
  if (saved.checkVersion < kDeviceCheckVersion) return true;
  if (saved.pretend != Pretend.off) return true; // never trust a test-only result
  // A saved graphics / camera failure may have been a one-off (driver hiccup): look again.
  if (saved.items.any((i) => i.level == CheckLevel.fail && (i.id == 'vulkan' || i.id == 'camera'))) return true;
  return saved.androidVersion != androidVersion || saved.model != model || saved.cameraId != cameraId;
}

// ---------------------------------------------------------------------------
// Static checks
// ---------------------------------------------------------------------------

/// The camera the app would use: the first back camera with RAW10 (the native
/// enumeration already sorts back RAW cameras first).
DetectedCamera? pickRawCamera(List<DetectedCamera> cameras) {
  for (final c in cameras) {
    if (c.isBack && c.supportsRaw10) return c;
  }
  return null;
}

String hardwareLevelName(int level) => switch (level) {
  0 => 'limited',
  1 => 'full',
  2 => 'legacy',
  3 => 'level 3',
  4 => 'external',
  _ => 'unknown',
};

/// Widest RAW10 mode among those that are usable at all, and the best frame
/// rate per aspect ratio among the modes wide enough for 1080p.
class RawSummary {
  final int widestWidth;
  final int widestHeight;
  final double maxFps169;
  final double maxFps43;
  const RawSummary(this.widestWidth, this.widestHeight, this.maxFps169, this.maxFps43);

  static RawSummary of(DetectedCamera cam) {
    if (cam.modes.isEmpty) return RawSummary(cam.rawWidth, cam.rawHeight, 0, 0);
    var widest = cam.modes.first;
    for (final m in cam.modes) {
      if (m.width > widest.width) widest = m;
    }
    final minWidth = widest.width < 3840 ? widest.width * 0.9 : 3840;
    double best(double aspect) {
      double b = 0;
      for (final m in cam.modes) {
        if (m.width < minWidth || (m.aspect - aspect).abs() > 0.05) continue;
        if (m.maxFps > b) b = m.maxFps;
      }
      return b;
    }

    return RawSummary(widest.width, widest.height, best(16 / 9), best(4 / 3));
  }
}

String _fpsText(double f) => f == f.roundToDouble() ? f.round().toString() : f.toStringAsFixed(1);

/// Plain-English static checks. `cameras` is the native enumeration; `gpu` and
/// `encoders` come from the engine and the Kotlin side. [pretend] fakes problems
/// (developer switch).
List<CheckItem> evaluateStatic({
  required List<DetectedCamera> cameras,
  required GpuInfo gpu,
  required List<EncoderCaps> encoders,
  Pretend pretend = Pretend.off,
}) {
  final items = <CheckItem>[];

  // -- Vulkan ---------------------------------------------------------------
  if (!gpu.vulkanOk) {
    items.add(const CheckItem('vulkan', CheckLevel.fail, 'Graphics (Vulkan)',
        'This phone\'s graphics driver could not start Vulkan, which Vesper Cine needs to develop the raw image.'));
  } else {
    items.add(CheckItem('vulkan', CheckLevel.ok, 'Graphics (Vulkan)',
        'Works${gpu.device.isEmpty ? '' : ': ${gpu.device}'}.'));
    items.add(CheckItem('fp16', gpu.fp16 ? CheckLevel.ok : CheckLevel.info, '16-bit maths',
        gpu.fp16 ? 'Yes: the sharpest (HQ) processing runs at full speed.' : 'No: the HQ processing runs slower on this GPU.'));
    items.add(switch (gpu.hqFormat) {
      'R16F' => const CheckItem('hq', CheckLevel.ok, 'HQ oversampling', 'Supported.'),
      'RGBA16F' => const CheckItem('hq', CheckLevel.warn, 'HQ oversampling',
          'Supported, but this GPU needs 4x the memory for it, so it may be slower.'),
      _ => const CheckItem('hq', CheckLevel.warn, 'HQ oversampling',
          'Not supported by this GPU: the image will be a little less sharp.'),
    });
  }

  // -- Camera ---------------------------------------------------------------
  final cam = pickRawCamera(cameras);
  if (cam == null) {
    final back16 = cameras.any((c) => c.isBack && c.raw16);
    items.add(CheckItem(
      'camera',
      CheckLevel.fail,
      'Raw camera',
      back16
          ? 'This phone\'s camera does not offer RAW10 video-speed raw. It only has 16-bit raw (RAW_SENSOR), which is for stills and too slow for video.'
          : cameras.isEmpty
              ? 'No camera was found.'
              : 'No back camera with raw output was found. Vesper Cine needs RAW10 raw from the back camera.',
    ));
  } else {
    items.add(CheckItem('camera', CheckLevel.ok, 'Raw camera', 'Back camera ${cam.id} has RAW10 (hardware level ${hardwareLevelName(cam.hwLevel)}).'));
    items.add(cam.manualSensor
        ? const CheckItem('manual', CheckLevel.ok, 'Manual control', 'The camera allows manual ISO and shutter.')
        : const CheckItem('manual', CheckLevel.fail, 'Manual control',
            'The camera does not allow manual ISO and shutter (MANUAL_SENSOR is missing), which a cinema camera app needs.'));
    items.add(cam.rawCapability
        ? const CheckItem('rawcap', CheckLevel.ok, 'Raw capability', 'The camera declares raw support.')
        : const CheckItem('rawcap', CheckLevel.fail, 'Raw capability', 'The camera does not declare raw support (RAW capability is missing).'));

    final raw = RawSummary.of(cam);
    final maxAny = raw.maxFps169 > raw.maxFps43 ? raw.maxFps169 : raw.maxFps43;
    final modeText = StringBuffer('Widest raw mode ${raw.widestWidth}x${raw.widestHeight}');
    modeText.write(raw.maxFps169 > 0 ? '; 16:9 up to ${_fpsText(raw.maxFps169)} fps' : '; no 16:9 mode (it is cropped from 4:3)');
    modeText.write(raw.maxFps43 > 0 ? ', 4:3 up to ${_fpsText(raw.maxFps43)} fps.' : '.');
    if (maxAny > 0 && maxAny < 23.9) {
      items.add(CheckItem('rawfps', CheckLevel.fail, 'Raw speed',
          '$modeText The raw readout is too slow for video: 24 fps is the minimum.'));
    } else {
      items.add(CheckItem('rawfps', CheckLevel.ok, 'Raw modes', modeText.toString()));
    }
    if (raw.widestWidth > 0 && raw.widestWidth < 3840) {
      items.add(CheckItem('rawwidth', CheckLevel.warn, 'Raw resolution',
          '1080p will be slightly soft (raw is only ${raw.widestWidth} px wide).'));
    }
    items.add(cam.tsRealtime
        ? const CheckItem('timestamps', CheckLevel.ok, 'Frame timestamps', 'On the system clock (good for Gyroflow stabilisation later).')
        : const CheckItem('timestamps', CheckLevel.info, 'Frame timestamps',
            'On the camera\'s own clock (UNKNOWN source): Gyroflow stabilisation will need an extra sync step.'));
  }

  // -- Encoder (never a blocker) ---------------------------------------------
  final tenBitHw = encoders.where((e) => e.hardware && e.main10 && e.fpsOk.isNotEmpty).toList();
  if (tenBitHw.isNotEmpty) {
    final best = tenBitHw.map((e) => e.fpsOk.reduce((a, b) => a > b ? a : b)).reduce((a, b) => a > b ? a : b);
    final codecs = tenBitHw.map((e) => e.codec).toSet().join(' and ');
    items.add(CheckItem('encoder', CheckLevel.ok, 'Video encoder',
        '10-bit hardware $codecs encoder for 1080p, up to $best fps.'));
  } else {
    items.add(const CheckItem('encoder', CheckLevel.warn, 'Video encoder',
        'No 10-bit hardware encoder: recording may be slow or fail.'));
  }

  // -- Developer pretend ----------------------------------------------------
  if (pretend == Pretend.unsupported) {
    items.removeWhere((i) => i.id == 'camera' || i.id == 'rawfps');
    items.insert(0, const CheckItem('camera', CheckLevel.fail, 'Raw camera',
        'TEST: pretending this phone\'s camera does not offer RAW10 video-speed raw.'));
  }
  return items;
}

// ---------------------------------------------------------------------------
// Classification of one frame rate
// ---------------------------------------------------------------------------

/// "full": fits at full quality (GPU below [kBudgetShare] of the frame time, no
/// drops). "reduced": does not fit, but with the guard on (it pauses alignment /
/// HQ / NR) there are no (or negligible) drops. "unsupported": drops even then.
/// [gpuMs] <= 0 means the GPU gave no timing: only drops decide.
FpsClass classifyFps({required int fps, required double gpuMs, required int drops, int? guardDrops}) {
  final budget = 1000.0 / fps;
  final fits = (gpuMs <= 0 || gpuMs < kBudgetShare * budget) && drops == 0;
  if (fits) return FpsClass.full;
  if (guardDrops != null && guardDrops <= kGuardDropTolerance) return FpsClass.reduced;
  return FpsClass.unsupported;
}

// ---------------------------------------------------------------------------
// What the result means for the frame-rate picker
// ---------------------------------------------------------------------------

/// Frame rates to offer: "unsupported" ones are hidden. Never everything: if all
/// would go, the slowest stays (the owner chose "Try anyway").
List<double> visibleFps(List<double> options, DeviceCheckResult? r) {
  if (r == null || r.fps.isEmpty || options.isEmpty) return options;
  final kept = options.where((f) => r.classFor(f) != FpsClass.unsupported).toList();
  if (kept.isNotEmpty) return kept;
  return [options.reduce((a, b) => a < b ? a : b)];
}

/// Sub-label for the picker: "RQ" (reduced quality) for frame rates that only fit
/// with the guard's reductions. Null = full, or no result.
String? fpsTag(double f, DeviceCheckResult? r) => r?.classFor(f) == FpsClass.reduced ? 'RQ' : null;

// ---------------------------------------------------------------------------
// Result wording
// ---------------------------------------------------------------------------

String _rangeText(List<int> v) {
  if (v.isEmpty) return '';
  v = [...v]..sort();
  return v.length == 1 ? '${v.first} fps' : '${v.first}–${v.last} fps';
}

/// One or two plain sentences for the top of the result screen.
String summarize(DeviceCheckResult r) {
  if (!r.supported) {
    return r.problems.isEmpty
        ? 'Not supported because the graphics processor cannot keep up with 1080p raw video, even at the lowest frame rate.'
        : 'Not supported. ${r.problems.map((p) => p.text).join(' ')}';
  }
  if (!r.liveRan || r.fps.isEmpty) {
    return 'Supported. The speed test was not run, so frame rates are not limited.';
  }
  final full = [for (final f in r.fps) if (f.cls == FpsClass.full) f.fps];
  final reduced = [for (final f in r.fps) if (f.cls == FpsClass.reduced) f.fps];
  final none = [for (final f in r.fps) if (f.cls == FpsClass.unsupported) f.fps];
  final parts = <String>[];
  if (full.isNotEmpty) {
    parts.add('1080p up to ${full.reduce((a, b) => a > b ? a : b)} fps at full quality');
  }
  if (reduced.isNotEmpty) {
    parts.add('${_rangeText(reduced)} with reduced processing');
  }
  var s = 'Supported: ${parts.join('; ')}.';
  if (none.isNotEmpty) s += ' ${_rangeText(none)} is too much for this phone and is hidden.';
  if (r.measuredWarm) {
    s += ' Measured while the phone was warm, so this may be pessimistic: re-check later in Settings › Info.';
  }
  return s;
}

// ---------------------------------------------------------------------------
// Live test
// ---------------------------------------------------------------------------

/// The engine as the live test sees it. The camera screen implements these with
/// the real engine; tests use a simulated one.
class LiveEnv {
  /// Latest engine status (null: engine gone).
  final EngineStatus? Function() status;

  /// "Everything on" as the GPU benchmark means it: HQ, TNR + alignment, chroma NR, sharpening 1,
  /// TNR 0.7, chroma NR 0.5 (the benchmark's levels), 16:9.
  final void Function() applyFullQuality;
  final void Function(int fps) setFps;
  final void Function(bool on) setGuard;

  /// True when the camera restarted / the app left the screen / a take began: results are void.
  final bool Function() interrupted;
  final void Function(String line) log;
  final Future<void> Function(Duration d) delay;

  const LiveEnv({
    required this.status,
    required this.applyFullQuality,
    required this.setFps,
    required this.setGuard,
    required this.interrupted,
    required this.log,
    required this.delay,
  });
}

/// Timings of the live test (shortened in tests).
class LiveTiming {
  final Duration poll;
  final Duration settle;
  final Duration measure;
  final Duration guardStable; // guard flags unchanged this long = the guard has finished stepping
  final Duration guardCap; // ... but never wait longer than this
  final Duration guardMeasure;
  final Duration warmPoll;
  final Duration warmGiveUp;
  const LiveTiming({
    this.poll = const Duration(milliseconds: 250),
    this.settle = const Duration(milliseconds: 1000),
    this.measure = const Duration(milliseconds: 3500),
    this.guardStable = const Duration(milliseconds: 1500),
    this.guardCap = const Duration(milliseconds: 8000),
    this.guardMeasure = const Duration(milliseconds: 2500),
    this.warmPoll = const Duration(seconds: 3),
    this.warmGiveUp = const Duration(minutes: 5),
  });
}

/// Warm = engine heat level 2 (hot) or Android's forecast >= 0.9 of "severe". Level 1 / 0.8-0.9 is
/// normal for a Pixel 10 at app start and is not waited for.
bool isWarm(EngineStatus? s) => s != null && (s.heatLevel >= 2 || s.heatHeadroom >= 0.9);

class LiveOutcome {
  final List<FpsResult> fps;
  final bool measuredWarm;
  final bool aborted;
  const LiveOutcome(this.fps, this.measuredWarm, this.aborted);
}

class _Window {
  final double avgGpuMs;
  final int drops;
  final int maxHeat;
  final bool aborted;
  const _Window(this.avgGpuMs, this.drops, this.maxHeat, this.aborted);
}

/// Waits until the phone has cooled down (or [skip] says to stop waiting, or
/// [LiveTiming.warmGiveUp] passes). Returns true if it is still warm.
Future<bool> waitUntilCool(
  LiveEnv env, {
  required bool Function() skip,
  void Function(EngineStatus? s)? onWaiting,
  LiveTiming timing = const LiveTiming(),
}) async {
  var waited = Duration.zero;
  var warm = isWarm(env.status());
  while (warm && !skip() && !env.interrupted() && waited < timing.warmGiveUp) {
    env.log('Device check: phone is warm, waiting (${waited.inSeconds} s)');
    onWaiting?.call(env.status());
    await env.delay(timing.warmPoll);
    waited += timing.warmPoll;
    warm = isWarm(env.status());
  }
  return warm;
}

Future<_Window> _measure(LiveEnv env, Duration settle, Duration measure, LiveTiming t) async {
  await env.delay(settle);
  var gpuSum = 0.0, gpuN = 0, drops = 0, heat = 0;
  int? prev;
  final steps = (measure.inMilliseconds / t.poll.inMilliseconds).round().clamp(1, 1 << 20);
  for (var i = 0; i < steps; i++) {
    await env.delay(t.poll);
    if (env.interrupted()) return _Window(0, 0, heat, true);
    final s = env.status();
    if (s == null) continue;
    if (s.gpuMs > 0) {
      gpuSum += s.gpuMs;
      gpuN++;
    }
    // The drop counter can be reset by the engine (e.g. Settings): count what it shows then.
    if (prev != null) drops += s.cameraDrops >= prev ? s.cameraDrops - prev : s.cameraDrops;
    prev = s.cameraDrops;
    if (s.heatLevel > heat) heat = s.heatLevel;
  }
  return _Window(gpuN == 0 ? 0 : gpuSum / gpuN, drops, heat, false);
}

/// The guard pauses one stage per ~36 frames and restores them on its own timetable, so after
/// switching it on the pauses are only complete once its flags stop changing. Waits for that
/// (capped), then returns the highest heat level seen.
Future<int> _waitGuardSettled(LiveEnv env, LiveTiming t) async {
  String sig(EngineStatus s) => '${s.alignThrottled}${s.alignReduced}${s.nrThrottled}${s.hqAvailable}';
  var waited = Duration.zero, stable = Duration.zero, heat = 0;
  String? last;
  while (waited < t.guardCap && stable < t.guardStable) {
    await env.delay(t.poll);
    waited += t.poll;
    if (env.interrupted()) return heat;
    final s = env.status();
    if (s == null) continue;
    if (s.heatLevel > heat) heat = s.heatLevel;
    final now = sig(s);
    if (now == last) {
      stable += t.poll;
    } else {
      stable = Duration.zero;
      last = now;
    }
  }
  return heat;
}

/// Measures each frame rate in [fpsList]: guard off at full quality, and, where that
/// does not fit, once more with the guard on (after it has finished stepping down).
/// [onProgress] gets (fps, 0..1). The caller restores the user's settings afterwards,
/// also when the result is `aborted`.
Future<LiveOutcome> runLiveTest(
  LiveEnv env,
  List<int> fpsList, {
  required bool startedWarm,
  void Function(int fps, double fraction)? onProgress,
  LiveTiming timing = const LiveTiming(),
}) async {
  final results = <FpsResult>[];
  var maxHeat = 0;
  bool warm() => startedWarm || maxHeat >= 2;
  if (env.interrupted()) return LiveOutcome(results, warm(), true);
  env.applyFullQuality();
  for (var i = 0; i < fpsList.length; i++) {
    if (env.interrupted()) return LiveOutcome(results, warm(), true);
    final fps = fpsList[i];
    onProgress?.call(fps, i / fpsList.length);
    env.setFps(fps);
    env.applyFullQuality(); // a camera restart re-applies the user's settings
    env.setGuard(false);
    final off = await _measure(env, timing.settle, timing.measure, timing);
    if (off.maxHeat > maxHeat) maxHeat = off.maxHeat;
    if (off.aborted || env.interrupted()) return LiveOutcome(results, warm(), true);
    final budget = 1000.0 / fps;
    int? guardDrops;
    if (classifyFps(fps: fps, gpuMs: off.avgGpuMs, drops: off.drops) != FpsClass.full) {
      env.setGuard(true);
      final h = await _waitGuardSettled(env, timing);
      if (h > maxHeat) maxHeat = h;
      final on = env.interrupted() ? null : await _measure(env, Duration.zero, timing.guardMeasure, timing);
      env.setGuard(false);
      if (on == null || on.aborted || env.interrupted()) return LiveOutcome(results, warm(), true);
      if (on.maxHeat > maxHeat) maxHeat = on.maxHeat;
      guardDrops = on.drops;
    }
    final cls = classifyFps(fps: fps, gpuMs: off.avgGpuMs, drops: off.drops, guardDrops: guardDrops);
    final r = FpsResult(fps, off.avgGpuMs, budget, off.drops, guardDrops, cls);
    env.log(r.logLine());
    results.add(r);
  }
  onProgress?.call(fpsList.isEmpty ? 0 : fpsList.last, 1);
  return LiveOutcome(results, warm(), false);
}

/// Fake live results for the developer's "Pretend slow phone": 24 full, 30 reduced,
/// everything faster unsupported.
List<FpsResult> pretendSlowResults(List<int> fpsList) => [
  for (final f in fpsList)
    FpsResult(
      f,
      f <= 24 ? 24.0 : 38.0 + f / 4,
      1000.0 / f,
      f <= 30 ? 0 : 40,
      f <= 24 ? null : (f <= 30 ? 0 : 25),
      f <= 24 ? FpsClass.full : (f <= 30 ? FpsClass.reduced : FpsClass.unsupported),
    ),
];

/// Frame rates of [kCheckFps] the camera's 16:9 modes can reach.
List<int> reachableCheckFps(double maxFps169) => [for (final f in kCheckFps) if (f <= maxFps169 + 0.5) f];
