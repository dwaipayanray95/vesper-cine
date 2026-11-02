import 'dart:convert';

/// "Send calibration to Vesper" (0.29.0): the report a tester emails to the
/// developer so per-model profiles can be built later. Pure Dart, unit-tested in
/// test/calibration_report_test.dart. Nothing here is sent automatically: the
/// user taps SEND and their own mail app opens with the file attached.
const String kCalibrationReportFormat = 'vesper-calibration-report/1';

/// The developer's address for calibration reports.
const String kCalibrationReportEmail = 'ray.vibecoder@gmail.com';

/// Absolute file paths (/data/user/0/..., /storage/emulated/0/...) anywhere in a
/// string. A lone "/" ("1/25 s", "16/9") or a relative "a/b/c" is not matched.
final RegExp _absPath = RegExp(r'(?<![\w.\-/])(?:/[\w.\-+@]+){2,}');

/// Replaces every absolute path in [text] by its last component.
String stripPaths(String text) =>
    text.replaceAllMapped(_absPath, (m) => m.group(0)!.split('/').last.trim());

/// Copy of [v] (JSON-like) with every string cleaned of absolute paths.
Object? stripPathsDeep(Object? v) {
  if (v is String) return stripPaths(v);
  if (v is List) return [for (final e in v) stripPathsDeep(e)];
  if (v is Map) return {for (final e in v.entries) '${e.key}': stripPathsDeep(e.value)};
  return v;
}

const _isoKeys = [
  'format', 'device', 'cameraId', 'minIso', 'maxIso', 'maxAnalogIso', 'exposureNs',
  'baseIso', 'hcgIso', 'digitalFromIso', 'nativeIsos', 'steps',
];

/// Only the known measurement keys of the ISO analysis.
Map<String, dynamic>? isoAnalysisForReport(Map<String, dynamic>? iso) =>
    iso == null ? null : {for (final k in _isoKeys) if (iso.containsKey(k)) k: iso[k]};

/// The calibration record: format and per-step version/date/app version only.
Map<String, dynamic>? calibrationForReport(Map<String, dynamic>? rec) {
  if (rec == null) return null;
  final steps = rec['steps'];
  return {
    'format': rec['format'],
    'steps': steps is Map
        ? {
            for (final e in steps.entries)
              '${e.key}': {
                for (final k in const ['version', 'date', 'appVersion'])
                  if (e.value is Map && (e.value as Map).containsKey(k)) k: (e.value as Map)[k],
              },
          }
        : <String, dynamic>{},
  };
}

/// The sensor profile without the hot-pixel map: `defects` is dropped, the count stays.
Map<String, dynamic>? sensorProfileForReport(Map<String, dynamic>? profile) {
  if (profile == null) return null;
  final out = Map<String, dynamic>.from(profile);
  final defects = out.remove('defects');
  if (out['defectCount'] is! num) {
    out['defectCount'] = defects is List ? defects.length ~/ 2 : 0;
  }
  return out;
}

/// Builds the report. All inputs are already-decoded JSON maps (or null when the
/// phone doesn't have that file). [device] is the native `deviceInfo` map: only
/// model, manufacturer, device name, Android version and SDK are used (no
/// directories). [calibration] is the calibration record as JSON.
Map<String, dynamic> buildCalibrationReport({
  required DateTime now,
  required String appVersion,
  required String appBuild,
  required Map<String, dynamic>? device,
  required String cameraId,
  Map<String, dynamic>? deviceCheck,
  Map<String, dynamic>? sensorProfile,
  Map<String, dynamic>? isoAnalysis,
  Map<String, dynamic>? calibration,
}) {
  final report = <String, dynamic>{
    'format': kCalibrationReportFormat,
    'created': now.toUtc().toIso8601String(),
    'app': {'version': appVersion, 'build': appBuild},
    'device': {
      'manufacturer': device?['manufacturer'],
      'model': device?['model'],
      'device': device?['device'],
      'android': device?['android'],
      'sdk': device?['sdk'],
    },
    'cameraId': cameraId,
    'deviceCheck': deviceCheck,
    'sensorProfile': sensorProfileForReport(sensorProfile),
    'isoAnalysis': isoAnalysisForReport(isoAnalysis),
    'calibration': calibrationForReport(calibration),
  };
  return stripPathsDeep(report) as Map<String, dynamic>;
}

String encodeCalibrationReport(Map<String, dynamic> report) =>
    const JsonEncoder.withIndent('  ').convert(report);

/// Ids of the calibration steps in the report's calibration record, for the log line.
List<String> reportSteps(Map<String, dynamic> report) {
  final steps = (report['calibration'] as Map?)?['steps'];
  return steps is Map ? [for (final k in steps.keys) '$k'] : const [];
}

/// Is there anything to send? A device check or at least one calibration step.
bool hasCalibrationToSend({required bool hasDeviceCheck, required int stepsDone}) =>
    hasDeviceCheck || stepsDone > 0;

String reportSubject(Map<String, dynamic> report) {
  final d = (report['device'] as Map?) ?? const {};
  final a = (report['app'] as Map?) ?? const {};
  final who = [d['manufacturer'], d['model']].where((e) => e != null && '$e'.isNotEmpty).join(' ');
  return 'Vesper Cine calibration: ${who.isEmpty ? 'unknown phone' : who}, app ${a['version'] ?? '?'}';
}

String reportBody() =>
    'This is the calibration report of my phone, made by Vesper Cine. It holds the phone model, '
    'Android version, camera id, measurement dates and the app\'s measurements (device check, noise and '
    'sensor numbers). It has no photos, no location and no hot-pixel map.\n\n'
    'I am sending it so Vesper Cine can work better on my phone model. I understand the developer also '
    'receives my email address, uses the report only for that, and deletes it if I ask by email.';

/// File name for the cache copy: `VesperCalibration_MODEL_YYYYMMDD.json`.
String reportFileName(Map<String, dynamic> report, DateTime now) {
  final model = '${((report['device'] as Map?) ?? const {})['model'] ?? 'phone'}'
      .replaceAll(RegExp(r'[^A-Za-z0-9]+'), '_')
      .replaceAll(RegExp(r'^_+|_+$'), '');
  String two(int n) => n.toString().padLeft(2, '0');
  final date = '${now.year}${two(now.month)}${two(now.day)}';
  return 'VesperCalibration_${model.isEmpty ? 'phone' : model}_$date.json';
}
