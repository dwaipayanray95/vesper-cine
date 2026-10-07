import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:vesper_cine/services/pro_service.dart';

// The free / Pro rules: clip counter, ad pass, and the plan's limits.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final pro = ProService.instance;
  ProService.adsEnabled = false;

  setUp(() async {
    SharedPreferences.setMockInitialValues({});
    pro.debugReset();
    await pro.loadLocal();
  });

  test('free plan: 5 clips, then recording needs an ad or Pro', () async {
    expect(pro.canRecord, isTrue);
    for (var i = 0; i < ProService.freeClips; i++) {
      expect(pro.canRecord, isTrue, reason: 'clip ${i + 1}');
      await pro.noteClipStarted();
    }
    expect(pro.clipsLeft, 0);
    expect(pro.canRecord, isFalse);
    expect(pro.hasProFeatures, isFalse);
  });

  test('ad pass: Pro features for exactly 3 clips, free clips untouched', () async {
    await pro.debugGrantPass();
    expect(pro.hasProFeatures, isTrue);
    for (var i = 0; i < ProService.proPassClips; i++) {
      expect(pro.hasProFeatures, isTrue);
      await pro.noteClipStarted();
    }
    expect(pro.hasProFeatures, isFalse);
    expect(pro.clipsUsed, 0);
    expect(pro.clipsLeft, ProService.freeClips);
  });

  test('pass and counter survive an app restart', () async {
    await pro.noteClipStarted();
    await pro.debugGrantPass();
    await pro.noteClipStarted();
    pro.debugReset();
    await pro.loadLocal();
    expect(pro.clipsUsed, 1);
    expect(pro.passClipsLeft, ProService.proPassClips - 1);
  });

  test('Pro override: unlimited, and clips are not counted', () async {
    pro.debugPro = true;
    for (var i = 0; i < 20; i++) {
      await pro.noteClipStarted();
    }
    expect(pro.canRecord, isTrue);
    expect(pro.hasProFeatures, isTrue);
    expect(pro.clipsUsed, 0);
  });

  test('free limits match the owner\'s list', () {
    expect(ProLimits.maxFps, 48.0);
    expect(ProLimits.maxTemporalNr, 0.5);
    expect(ProLimits.maxChromaNr, 0.5);
    expect(ProLimits.maxSharpening, 2);
    expect(ProLimits.maxRecordQuality, 0);
  });
}
