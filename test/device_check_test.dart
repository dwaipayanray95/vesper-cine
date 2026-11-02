import 'package:flutter_test/flutter_test.dart';
import 'package:vesper_cine/services/device_check.dart';
import 'package:vesper_cine/services/vesper_native.dart';

// Device check: static checks, frame-rate classification, the result file and
// the live measurement loop (against a simulated engine).
DetectedCamera pixelLike({bool manual = true, bool rawCap = true, int width = 4000, bool raw10 = true, bool raw16 = false}) =>
    DetectedCamera(
      id: '0',
      supportsRaw10: raw10,
      rawWidth: width,
      rawHeight: width * 9 ~/ 16,
      manualSensor: manual,
      rawCapability: rawCap,
      raw16: raw16,
      modes: raw10
          ? [RawMode(width, width * 3 ~/ 4, 30), RawMode(width, width * 9 ~/ 16, 60)]
          : const [],
    );

const goodEncoder = EncoderCaps('c2.google.hevc.encoder', 'video/hevc', true, true, [24, 30, 48, 60]);

EngineStatus status({double gpuMs = 20, int drops = 0, int heat = 0, double headroom = 0.3, bool alignThrottled = false, bool nrThrottled = false, bool hqAvailable = true}) => EngineStatus.fromJson({
  'streaming': true, 'fps': 30.0, 'cameraDrops': drops, 'kelvin': 5600.0, 'tint': 0.0, 'recording': false,
  'durationMs': 0, 'framesEncoded': 0, 'framesDropped': 0, 'thermal': 0, 'heatLevel': heat, 'heatHeadroom': headroom,
  'audio': false, 'codec': '', 'stopReason': '', 'exposureNs': 0, 'iso': 100, 'awbAuto': false, 'afState': 0,
  'focusDiopters': 0.0, 'face': [0.0, 0.0, 0.0, 0.0], 'gpuMs': gpuMs, 'alignThrottled': alignThrottled, 'nrThrottled': nrThrottled, 'hqAvailable': hqAvailable,
});

DeviceCheckResult resultWith(List<FpsResult> fps, {bool live = true, List<CheckItem> items = const []}) => DeviceCheckResult(
  model: 'Pixel 10', androidVersion: '16 (SDK 36)', cameraId: '0', timestamp: 't', items: items, fps: fps, liveRan: live,
);

FpsResult r(int fps, FpsClass c) => FpsResult(fps, 20, 1000.0 / fps, 0, null, c);

void main() {
  group('classifyFps', () {
    test('full: GPU under 85% of the frame and no drops', () {
      expect(classifyFps(fps: 30, gpuMs: 28.0, drops: 0), FpsClass.full); // budget 33.3, line 28.3
      expect(classifyFps(fps: 24, gpuMs: 30.0, drops: 0), FpsClass.full);
    });
    test('over 85% or any drop is not full', () {
      expect(classifyFps(fps: 30, gpuMs: 29.0, drops: 0), isNot(FpsClass.full));
      expect(classifyFps(fps: 24, gpuMs: 10, drops: 3), isNot(FpsClass.full));
    });
    test('reduced: the guard keeps it (almost) drop-free', () {
      expect(classifyFps(fps: 60, gpuMs: 30, drops: 50, guardDrops: 0), FpsClass.reduced);
      expect(classifyFps(fps: 60, gpuMs: 30, drops: 50, guardDrops: 1), FpsClass.reduced);
    });
    test('unsupported: drops even with the guard', () {
      expect(classifyFps(fps: 60, gpuMs: 30, drops: 50, guardDrops: 2), FpsClass.unsupported);
      expect(classifyFps(fps: 60, gpuMs: 30, drops: 50), FpsClass.unsupported); // guard pass missing
    });
    test('no GPU timing: only drops decide', () {
      expect(classifyFps(fps: 30, gpuMs: 0, drops: 0), FpsClass.full);
    });
  });

  group('frame rate picker rules', () {
    final res = resultWith([r(24, FpsClass.full), r(30, FpsClass.full), r(48, FpsClass.reduced), r(60, FpsClass.unsupported)]);
    const all = [23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 60.0];
    test('unsupported ones are hidden, reduced ones stay and are tagged RQ', () {
      expect(visibleFps(all, res), [23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0]); // 50 follows 48
      expect(fpsTag(48, res), 'RQ');
      expect(fpsTag(50, res), 'RQ');
      expect(fpsTag(30, res), isNull);
    });
    test('other rates take the nearest measured class', () {
      expect(res.classFor(25), FpsClass.full);
      expect(res.classFor(29.97), FpsClass.full);
      expect(res.classFor(54), FpsClass.unsupported); // tie between 48 and 60 goes to the higher
    });
    test('without a result nothing changes', () {
      expect(visibleFps(all, null), all);
      expect(fpsTag(60, null), isNull);
      expect(visibleFps(all, resultWith(const [], live: false)), all);
    });
    test('never hides everything', () {
      final bad = resultWith([r(24, FpsClass.unsupported), r(30, FpsClass.unsupported), r(48, FpsClass.unsupported), r(60, FpsClass.unsupported)]);
      expect(visibleFps(all, bad), [23.976]);
    });
  });

  group('static checks', () {
    List<CheckItem> run({List<DetectedCamera>? cams, GpuInfo gpu = const GpuInfo(), List<EncoderCaps>? enc, Pretend pretend = Pretend.off}) =>
        evaluateStatic(cameras: cams ?? [pixelLike()], gpu: gpu, encoders: enc ?? [goodEncoder], pretend: pretend);
    CheckItem item(List<CheckItem> l, String id) => l.firstWhere((i) => i.id == id);

    test('a Pixel-like phone passes without warnings', () {
      final items = run();
      expect(items.where((i) => i.level == CheckLevel.fail), isEmpty);
      expect(items.where((i) => i.level == CheckLevel.warn), isEmpty);
      expect(item(items, 'encoder').text, contains('up to 60 fps'));
    });
    test('RAW_SENSOR-only phone is unsupported with the plain reason', () {
      final items = run(cams: [pixelLike(raw10: false, raw16: true)]);
      expect(item(items, 'camera').level, CheckLevel.fail);
      expect(item(items, 'camera').text, contains('does not offer RAW10 video-speed raw'));
    });
    test('missing MANUAL_SENSOR or RAW capability is a blocker', () {
      expect(item(run(cams: [pixelLike(manual: false)]), 'manual').level, CheckLevel.fail);
      expect(item(run(cams: [pixelLike(rawCap: false)]), 'rawcap').level, CheckLevel.fail);
    });
    test('narrow raw is a warning, not a blocker', () {
      final items = run(cams: [pixelLike(width: 3264)]);
      expect(item(items, 'rawwidth').level, CheckLevel.warn);
      expect(item(items, 'rawwidth').text, '1080p will be slightly soft (raw is only 3264 px wide).');
      expect(items.where((i) => i.level == CheckLevel.fail), isEmpty);
    });
    test('no 10-bit hardware encoder is a warning only', () {
      final items = run(enc: const [EncoderCaps('c2.android.hevc.encoder', 'video/hevc', false, true, [24])]);
      expect(item(items, 'encoder').level, CheckLevel.warn);
      expect(item(items, 'encoder').text, 'No 10-bit hardware encoder: recording may be slow or fail.');
      expect(items.where((i) => i.level == CheckLevel.fail), isEmpty);
      // A hardware encoder without Main10, or one that cannot do 1080p, doesn't count either.
      expect(item(run(enc: const [EncoderCaps('x', 'video/hevc', true, false, [24])]), 'encoder').level, CheckLevel.warn);
      expect(item(run(enc: const [EncoderCaps('x', 'video/hevc', true, true, [])]), 'encoder').level, CheckLevel.warn);
    });
    test('Vulkan failure is a blocker; RGBA16F is a warning', () {
      expect(item(run(gpu: const GpuInfo(vulkanOk: false)), 'vulkan').level, CheckLevel.fail);
      expect(item(run(gpu: const GpuInfo(hqFormat: 'RGBA16F')), 'hq').level, CheckLevel.warn);
      expect(item(run(gpu: const GpuInfo(hqFormat: 'none')), 'hq').level, CheckLevel.warn);
    });
    test('timestamp source is only information', () {
      final c = DetectedCamera(id: '0', tsRealtime: false, modes: pixelLike().modes, rawWidth: 4000, rawHeight: 2250);
      expect(item(run(cams: [c]), 'timestamps').level, CheckLevel.info);
    });
    test('a raw mode slower than 24 fps is a blocker', () {
      final c = DetectedCamera(id: '0', rawWidth: 4000, rawHeight: 3000, modes: [RawMode(4000, 3000, 15)]);
      expect(item(run(cams: [c]), 'rawfps').level, CheckLevel.fail);
    });
    test('pretend unsupported fakes a missing RAW10', () {
      final items = run(pretend: Pretend.unsupported);
      expect(item(items, 'camera').level, CheckLevel.fail);
      expect(item(items, 'camera').text, contains('TEST'));
    });
    test('RawSummary: per-aspect maximum among wide modes', () {
      final s = RawSummary.of(DetectedCamera(id: '0', rawWidth: 8160, rawHeight: 6144, modes: [
        RawMode(8160, 6144, 15), RawMode(4080, 3072, 30), RawMode(4080, 2296, 60),
      ]));
      expect(s.widestWidth, 8160);
      expect(s.maxFps43, 30);
      expect(s.maxFps169, 60);
    });
  });

  group('result and re-run rules', () {
    final res = DeviceCheckResult(
      model: 'Pixel 10', androidVersion: '16 (SDK 36)', cameraId: '0', timestamp: '2026-11-02T10:00:00',
      appVersion: '0.27.0', appBuild: '101',
      items: const [CheckItem('camera', CheckLevel.ok, 'Raw camera', 'ok')],
      fps: [FpsResult(60, 31.5, 16.67, 40, 0, FpsClass.reduced)], liveRan: true, measuredWarm: true,
    );
    test('JSON round trip keeps every field', () {
      final back = DeviceCheckResult.decode(res.encode())!;
      expect(back.model, 'Pixel 10');
      expect(back.checkVersion, kDeviceCheckVersion);
      expect(back.fps.single.cls, FpsClass.reduced);
      expect(back.fps.single.guardDrops, 0);
      expect(back.measuredWarm, isTrue);
      expect(back.items.single.id, 'camera');
      expect(res.encode(), contains('"format": "vesper-device-check/1"'));
      expect(DeviceCheckResult.decode('{"format":"x"}'), isNull);
      expect(DeviceCheckResult.decode('garbage'), isNull);
    });
    test('re-run only when there is no file, the check improved, or the phone changed', () {
      bool due(DeviceCheckResult? s, {String model = 'Pixel 10', String android = '16 (SDK 36)', String cam = '0'}) =>
          deviceCheckDue(s, model: model, androidVersion: android, cameraId: cam);
      expect(due(null), isTrue);
      expect(due(res), isFalse); // an app update alone does not re-run it
      expect(due(res, android: '17 (SDK 37)'), isTrue);
      expect(due(res, model: 'Pixel 11'), isTrue);
      expect(due(res, cam: '1'), isTrue);
      final vkFail = DeviceCheckResult(model: 'Pixel 10', androidVersion: '16 (SDK 36)', cameraId: '0', timestamp: '',
          items: const [CheckItem('vulkan', CheckLevel.fail, 'Graphics (Vulkan)', 'x')]);
      expect(due(vkFail), isTrue); // a saved graphics failure is looked at again
      final old = DeviceCheckResult(checkVersion: kDeviceCheckVersion - 1, model: 'Pixel 10', androidVersion: '16 (SDK 36)', cameraId: '0', timestamp: '', items: const []);
      expect(due(old), isTrue);
    });
    test('summary wording', () {
      expect(summarize(resultWith([r(24, FpsClass.full), r(30, FpsClass.full), r(48, FpsClass.reduced), r(60, FpsClass.reduced)])),
          'Supported: 1080p up to 30 fps at full quality; 48–60 fps with reduced processing.');
      expect(summarize(resultWith([r(24, FpsClass.full), r(30, FpsClass.reduced), r(48, FpsClass.unsupported), r(60, FpsClass.unsupported)])),
          contains('48–60 fps is too much for this phone'));
      final bad = resultWith(const [], items: const [CheckItem('camera', CheckLevel.fail, 'Raw camera', 'No camera.')]);
      expect(bad.supported, isFalse);
      expect(summarize(bad), startsWith('Not supported'));
      expect(summarize(res), contains('Measured while the phone was warm'));
    });
    test('pretend slow phone gives 24 full, 30 reduced, 48 and 60 unsupported', () {
      final slow = pretendSlowResults(kCheckFps);
      expect(slow.map((f) => f.cls), [FpsClass.full, FpsClass.reduced, FpsClass.unsupported, FpsClass.unsupported]);
    });
    test('reachable check rates follow the 16:9 maximum', () {
      expect(reachableCheckFps(60), [24, 30, 48, 60]);
      expect(reachableCheckFps(30), [24, 30]);
      expect(reachableCheckFps(29.97), [24, 30]);
    });
  });

  group('live test', () {
    const fast = LiveTiming(
      poll: Duration(milliseconds: 250), settle: Duration(milliseconds: 500), measure: Duration(milliseconds: 1000),
      guardMeasure: Duration(milliseconds: 1000),
      warmPoll: Duration(seconds: 3), warmGiveUp: Duration(seconds: 12),
    );

    // Simulated phone: GPU time per fps with everything on; the guard-on case has its own drops.
    ({LiveEnv env, List<String> log, List<String> calls}) sim({
      required double Function(int fps) gpuMs,
      required int Function(int fps, bool guard, int stage) dropsPerWindowSample,
      bool Function()? interrupted,
      EngineStatus Function()? warmStatus,
    }) {
      var fps = 24;
      var guard = false;
      var guardPolls = 0; // the guard pauses one more stage every 4 polls (1 s), up to 3 stages
      var drops = 0;
      final log = <String>[];
      final calls = <String>[];
      final env = LiveEnv(
        status: () {
          if (warmStatus != null) return warmStatus();
          final stage = guard ? (guardPolls++ ~/ 4).clamp(0, 3) : 0;
          drops += dropsPerWindowSample(fps, guard, stage);
          return status(gpuMs: gpuMs(fps), drops: drops, alignThrottled: stage >= 1, hqAvailable: stage < 2, nrThrottled: stage >= 3);
        },
        applyFullQuality: () => calls.add('full'),
        setFps: (f) {
          fps = f;
          calls.add('fps$f');
        },
        setGuard: (g) {
          guard = g;
          guardPolls = 0;
          calls.add('guard$g');
        },
        interrupted: interrupted ?? () => false,
        log: log.add,
        delay: (d) async {},
      );
      return (env: env, log: log, calls: calls);
    }

    test('classifies a Pixel-like phone and keeps the guard off while measuring', () async {
      final s = sim(
        gpuMs: (f) => f <= 30 ? 27.0 : 31.0,
        dropsPerWindowSample: (f, g, st) => f >= 48 && (!g || st < 3) ? 3 : 0,
      );
      final out = await runLiveTest(s.env, kCheckFps, startedWarm: false, timing: fast);
      expect(out.aborted, isFalse);
      expect(out.fps.map((f) => f.cls), [FpsClass.full, FpsClass.full, FpsClass.reduced, FpsClass.reduced]);
      expect(out.fps[0].guardDrops, isNull); // fits: no second pass needed
      expect(out.fps[2].guardDrops, 0);
      expect(s.log.where((l) => l.startsWith('Device check: 48 fps: GPU 31.0 ms (budget 20.8), drops ')), hasLength(1));
      expect(s.log.first, startsWith('Device check: 24 fps: GPU 27.0 ms (budget 41.7), drops 0, guard-on drops - -> full'));
      // Guard is switched on only for the second pass and off again afterwards.
      expect(s.calls.indexOf('guardtrue'), greaterThan(s.calls.indexOf('fps48')));
      expect(s.calls.last, isNot('guardtrue'));
    });

    test('drops even with the guard on means unsupported', () async {
      final s = sim(gpuMs: (f) => 40, dropsPerWindowSample: (f, g, st) => 2);
      final out = await runLiveTest(s.env, [60], startedWarm: false, timing: fast);
      expect(out.fps.single.cls, FpsClass.unsupported);
      expect(out.fps.single.drops, greaterThan(0));
    });

    test('a camera restart aborts the test', () async {
      var n = 0;
      final s = sim(gpuMs: (f) => 20, dropsPerWindowSample: (f, g, st) => 0, interrupted: () => ++n > 6);
      final out = await runLiveTest(s.env, kCheckFps, startedWarm: false, timing: fast);
      expect(out.aborted, isTrue);
      expect(out.fps.length, lessThan(4));
    });

    test('the guard pass waits until the guard has finished stepping down (gradual ramp)', () async {
      // Guard ramps 3 stages over 3 s; drops continue until the last one. A fixed 2 s settle would see drops.
      final s = sim(gpuMs: (f) => 31, dropsPerWindowSample: (f, g, st) => (!g || st < 3) ? 3 : 0);
      final out = await runLiveTest(s.env, [60], startedWarm: false, timing: fast);
      expect(out.fps.single.guardDrops, 0);
      expect(out.fps.single.cls, FpsClass.reduced);
    });

    test('heat reaching level 2 during the run marks the result warm', () async {
      var n = 0;
      final env = LiveEnv(
        status: () => status(gpuMs: 20, heat: ++n > 2 ? 2 : 0),
        applyFullQuality: () {}, setFps: (f) {}, setGuard: (g) {},
        interrupted: () => false, log: (l) {}, delay: (d) async {},
      );
      final out = await runLiveTest(env, [24], startedWarm: false, timing: fast);
      expect(out.measuredWarm, isTrue);
    });

    test('an already interrupted run changes nothing', () async {
      final s = sim(gpuMs: (f) => 20, dropsPerWindowSample: (f, g, st) => 0, interrupted: () => true);
      final out = await runLiveTest(s.env, [24, 30], startedWarm: false, timing: fast);
      expect(out.aborted, isTrue);
      expect(s.calls, isEmpty);
    });

    test('the measurement carries the warm flag', () async {
      final s = sim(gpuMs: (f) => 20, dropsPerWindowSample: (f, g, st) => 0);
      final out = await runLiveTest(s.env, [24], startedWarm: true, timing: fast);
      expect(out.measuredWarm, isTrue);
    });

    test('waits while warm, stops on skip or give-up, returns true if still warm', () async {
      final warm = sim(gpuMs: (f) => 20, dropsPerWindowSample: (f, g, st) => 0, warmStatus: () => status(heat: 2, headroom: 0.95));
      expect(await waitUntilCool(warm.env, skip: () => true, timing: fast), isTrue); // skipped at once
      expect(await waitUntilCool(warm.env, skip: () => false, timing: fast), isTrue); // gave up after 12 s
      expect(warm.log.where((l) => l.contains('warm')).length, 4);
      final cool = sim(gpuMs: (f) => 20, dropsPerWindowSample: (f, g, st) => 0);
      expect(await waitUntilCool(cool.env, skip: () => false, timing: fast), isFalse);
      expect(isWarm(status(headroom: 0.92)), isTrue);
      expect(isWarm(status(heat: 2, headroom: -1)), isTrue);
      expect(isWarm(status(heat: 1, headroom: 0.83)), isFalse); // a Pixel 10 at app start
      expect(isWarm(status(headroom: 0.82)), isFalse);
      expect(isWarm(status(headroom: 0.5)), isFalse);
    });
  });
}
