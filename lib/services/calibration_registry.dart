import 'dart:convert';

/// Calibration memory (0.27.0): which calibration steps the app wants today, and
/// which of them this phone has done. After an update that adds a step or bumps
/// a step's [CalibrationStep.version], the phone is asked once ("New calibration
/// available") instead of testers having to remember.
///
/// HOW TO ADD OR UPDATE A STEP: add a [CalibrationStep] to [calibrationSteps]
/// (new `id`, `version: 1`), or raise the `version` of an existing one when its
/// measurement changed and old results should be redone. Then wire the id to the
/// code that runs it and calls `markDone(id)` when it finishes (camera_screen.dart:
/// `_onSensorSweepFinished`, `_onIsoSweepFinished`, `_runCalibrationStep`).
/// Pure Dart: the rules are unit-tested in test/calibration_registry_test.dart.
class CalibrationStep {
  final String id;
  final int version;
  final String title;
  final String improves; // plain English: what the owner gets from it
  final int minutes; // about how long it takes
  final bool needsLensCovered;

  /// A shipped per-model profile may stand in for this step (nothing ships yet).
  /// Hot pixels are per phone unit, so they are never covered.
  final bool coverableByModelProfile;
  const CalibrationStep({
    required this.id,
    required this.version,
    required this.title,
    required this.improves,
    required this.minutes,
    required this.needsLensCovered,
    this.coverableByModelProfile = false,
  });
}

const String kHotPixelsStep = 'hotPixels';
const String kNativeIsoStep = 'nativeIso';

/// The steps the app currently wants, in the order the guided setup runs them.
const List<CalibrationStep> calibrationSteps = [
  CalibrationStep(
    id: kHotPixelsStep,
    version: 1,
    title: 'Hot pixel calibration',
    improves: 'Finds this phone\'s own stuck and hot pixels so they are always removed, and measures its dark noise.',
    minutes: 1,
    needsLensCovered: true,
    // Every sensor has different hot pixels: a model profile can never replace this.
  ),
  CalibrationStep(
    id: kNativeIsoStep,
    version: 1,
    title: 'Native ISO analysis',
    improves: 'Finds the cleanest ISO settings of this sensor, so auto-exposure picks the least noisy one.',
    minutes: 1,
    needsLensCovered: true,
    coverableByModelProfile: true, // nothing ships yet
  ),
  // 0.28: CalibrationStep(id: 'white', ...) — White sweep (paper over the lens, daylight): lens shading and clip point.
];

enum StepStatus { done, providedForPhone, notDone, updateAvailable }

class StepRecord {
  final int version;
  final String date; // ISO 8601
  final String appVersion;
  const StepRecord(this.version, this.date, this.appVersion);

  Map<String, dynamic> toJson() => {'version': version, 'date': date, 'appVersion': appVersion};

  static StepRecord? fromJson(Object? j) {
    if (j is! Map) return null;
    final v = j['version'];
    if (v is! num) return null;
    return StepRecord(v.toInt(), j['date'] as String? ?? '', j['appVersion'] as String? ?? '');
  }
}

/// `vesper_calibration_<cameraId>.json`.
class CalibrationRecord {
  static const format = 'vesper-calibration/1';
  final Map<String, StepRecord> steps;

  /// App version for which the "New calibration available" card was already shown
  /// (or declined with "Later"): it is not shown again until the app version changes.
  final String promptedAppVersion;

  /// 0.29.0: the "Help Vesper support your phone" (send calibration) offer was shown once.
  final bool sendOffered;
  const CalibrationRecord({this.steps = const {}, this.promptedAppVersion = '', this.sendOffered = false});

  CalibrationRecord withDone(String id, int version, DateTime now, String appVersion) => CalibrationRecord(
    steps: {...steps, id: StepRecord(version, now.toIso8601String(), appVersion)},
    promptedAppVersion: promptedAppVersion,
    sendOffered: sendOffered,
  );

  CalibrationRecord withPrompted(String appVersion) =>
      CalibrationRecord(steps: steps, promptedAppVersion: appVersion, sendOffered: sendOffered);

  CalibrationRecord withSendOffered() =>
      CalibrationRecord(steps: steps, promptedAppVersion: promptedAppVersion, sendOffered: true);

  String encode() => const JsonEncoder.withIndent('  ').convert({
    'format': format,
    'steps': {for (final e in steps.entries) e.key: e.value.toJson()},
    'promptedAppVersion': promptedAppVersion,
    'calibrationSendOffered': sendOffered,
  });

  /// Null when [text] isn't a record file.
  static CalibrationRecord? decode(String text) {
    try {
      final j = jsonDecode(text) as Map<String, dynamic>;
      if (j['format'] != format) return null;
      final steps = <String, StepRecord>{};
      for (final e in ((j['steps'] as Map?) ?? const {}).entries) {
        final r = StepRecord.fromJson(e.value);
        if (r != null) steps[e.key as String] = r;
      }
      return CalibrationRecord(
        steps: steps,
        promptedAppVersion: j['promptedAppVersion'] as String? ?? '',
        sendOffered: j['calibrationSendOffered'] == true,
      );
    } catch (_) {
      return null;
    }
  }
}

/// Where this phone stands on [step]. [coveredByProfile]: ids a shipped model
/// profile provides (only honoured for steps that allow it).
StepStatus statusOf(CalibrationStep step, CalibrationRecord rec, {Set<String> coveredByProfile = const {}}) {
  final done = rec.steps[step.id];
  if (done != null && done.version >= step.version) return StepStatus.done;
  if (step.coverableByModelProfile && coveredByProfile.contains(step.id)) return StepStatus.providedForPhone;
  return done == null ? StepStatus.notDone : StepStatus.updateAvailable;
}

/// Steps still to do: not done, or done with an older version, and not covered by a profile.
List<CalibrationStep> pendingSteps(
  CalibrationRecord rec, {
  Set<String> coveredByProfile = const {},
  List<CalibrationStep> steps = calibrationSteps,
}) => [
  for (final s in steps)
    if (const {StepStatus.notDone, StepStatus.updateAvailable}.contains(statusOf(s, rec, coveredByProfile: coveredByProfile))) s,
];

/// Show the "New calibration available" card? Only when something is pending and
/// it was not already shown (or postponed with "Later") for this app version.
bool shouldPrompt(List<CalibrationStep> pending, CalibrationRecord rec, String appVersion) =>
    pending.isNotEmpty && rec.promptedAppVersion != appVersion;

/// First start of the version that introduced the record: whatever the phone
/// already has counts as done, so nobody is asked again for work they did.
CalibrationRecord migrateFromExistingFiles({
  required bool hasHotPixelProfile,
  required bool hasIsoAnalysis,
  required DateTime now,
  required String appVersion,
}) {
  var rec = const CalibrationRecord();
  if (hasHotPixelProfile) rec = rec.withDone(kHotPixelsStep, 1, now, appVersion);
  if (hasIsoAnalysis) rec = rec.withDone(kNativeIsoStep, 1, now, appVersion);
  return rec;
}

/// "New calibration available: A, B — what it improves, ~N min".
String promptText(List<CalibrationStep> pending) {
  if (pending.isEmpty) return '';
  final minutes = pending.fold<int>(0, (a, s) => a + s.minutes);
  final names = pending.map((s) => s.title).join(' + ');
  final improves = pending.map((s) => s.improves).join(' ');
  return 'New calibration available: $names. $improves About $minutes min.';
}
