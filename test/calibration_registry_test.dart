import 'package:flutter_test/flutter_test.dart';
import 'package:vesper_cine/services/calibration_registry.dart';

// Calibration memory: which steps are pending, migration of older installs, and
// the once-per-app-version "New calibration available" card.
void main() {
  final now = DateTime.utc(2026, 11, 2, 12);

  test('a new phone has every step pending', () {
    final pending = pendingSteps(const CalibrationRecord());
    expect(pending.map((s) => s.id), [kHotPixelsStep, kNativeIsoStep]);
    expect(statusOf(calibrationSteps.first, const CalibrationRecord()), StepStatus.notDone);
  });

  test('done steps are not pending; an older version is an update', () {
    var rec = const CalibrationRecord().withDone(kHotPixelsStep, 1, now, '0.27.0').withDone(kNativeIsoStep, 1, now, '0.27.0');
    expect(pendingSteps(rec), isEmpty);
    // The app now wants hot pixels v2: the phone's v1 is outdated.
    const v2 = CalibrationStep(
        id: kHotPixelsStep, version: 2, title: 't', improves: 'i', minutes: 1, needsLensCovered: true);
    expect(statusOf(v2, rec), StepStatus.updateAvailable);
    expect(pendingSteps(rec, steps: const [v2]).single.version, 2);
    rec = rec.withDone(kHotPixelsStep, 2, now, '0.28.0');
    expect(pendingSteps(rec, steps: const [v2]), isEmpty);
  });

  test('a shipped model profile covers native ISO but never hot pixels', () {
    const rec = CalibrationRecord();
    final pending = pendingSteps(rec, coveredByProfile: {kHotPixelsStep, kNativeIsoStep});
    expect(pending.map((s) => s.id), [kHotPixelsStep]); // hot pixels are per phone unit
    expect(statusOf(calibrationSteps[1], rec, coveredByProfile: {kNativeIsoStep}), StepStatus.providedForPhone);
    expect(statusOf(calibrationSteps[0], rec, coveredByProfile: {kHotPixelsStep}), StepStatus.notDone);
  });

  test('migration: existing hot-pixel map and ISO analysis count as done', () {
    final both = migrateFromExistingFiles(hasHotPixelProfile: true, hasIsoAnalysis: true, now: now, appVersion: '0.27.0');
    expect(pendingSteps(both), isEmpty);
    expect(both.steps[kHotPixelsStep]!.appVersion, '0.27.0');
    final onlyHot = migrateFromExistingFiles(hasHotPixelProfile: true, hasIsoAnalysis: false, now: now, appVersion: '0.27.0');
    expect(pendingSteps(onlyHot).map((s) => s.id), [kNativeIsoStep]);
    final none = migrateFromExistingFiles(hasHotPixelProfile: false, hasIsoAnalysis: false, now: now, appVersion: '0.27.0');
    expect(pendingSteps(none).length, 2);
  });

  test('"Later" is remembered per app version', () {
    var rec = const CalibrationRecord();
    final pending = pendingSteps(rec);
    expect(shouldPrompt(pending, rec, '0.27.0'), isTrue);
    rec = rec.withPrompted('0.27.0'); // answered Later (or ran it)
    expect(shouldPrompt(pending, rec, '0.27.0'), isFalse);
    expect(shouldPrompt(pending, rec, '0.27.1'), isTrue); // next update asks again
    expect(shouldPrompt(const [], rec, '0.27.1'), isFalse); // nothing pending: no card
  });

  test('the record survives a save and load', () {
    final rec = const CalibrationRecord().withDone(kHotPixelsStep, 1, now, '0.27.0').withPrompted('0.27.0');
    final back = CalibrationRecord.decode(rec.encode())!;
    expect(back.steps[kHotPixelsStep]!.version, 1);
    expect(back.steps[kHotPixelsStep]!.date, now.toIso8601String());
    expect(back.promptedAppVersion, '0.27.0');
    expect(CalibrationRecord.decode('not json'), isNull);
    expect(CalibrationRecord.decode('{"format":"other"}'), isNull);
  });

  test('prompt text names the steps and the time', () {
    final text = promptText(pendingSteps(const CalibrationRecord()));
    expect(text, contains('New calibration available'));
    expect(text, contains('Hot pixel calibration'));
    expect(text, contains('2 min'));
    expect(promptText(const []), '');
  });

  test('sendOffered round trip through encode/decode', () {
    const rec = CalibrationRecord();
    expect(rec.sendOffered, isFalse);
    final offered = rec.withSendOffered();
    final back = CalibrationRecord.decode(offered.encode())!;
    expect(back.sendOffered, isTrue);
    expect(back.withDone(kHotPixelsStep, 1, now, '0.29.0').sendOffered, isTrue);
    expect(CalibrationRecord.decode(rec.encode())!.sendOffered, isFalse);
  });
}
