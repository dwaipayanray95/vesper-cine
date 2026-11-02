import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:vesper_cine/services/calibration_registry.dart';
import 'package:vesper_cine/services/calibration_report.dart';

void main() {
  final now = DateTime.utc(2026, 11, 2, 12);
  final profile = <String, dynamic>{
    'format': 'vesper-sensor-profile/1',
    'device': 'Pixel 10',
    'darkNoise': [
      {'iso': 30, 'factor': 1.0},
    ],
    'defects': [10, 20, 30, 40, 50, 60],
    'sourceFile': '/data/user/0/com.theawesomeray.vespercine/files/sweep_1/VSENSOR_x_profile.json',
  };

  Map<String, dynamic> build({Map<String, dynamic>? p}) => buildCalibrationReport(
    now: now,
    appVersion: '0.29.0',
    appBuild: '104',
    device: {
      'model': 'Pixel 10',
      'manufacturer': 'Google',
      'device': 'frankel',
      'android': '16',
      'sdk': 36,
      'filesDir': '/data/user/0/com.theawesomeray.vespercine/files',
      'calibrationDir': '/storage/emulated/0/Android/data/com.theawesomeray.vespercine/files/calibration',
    },
    cameraId: '0',
    deviceCheck: {
      'fps': [
        {'fps': 30, 'class': 'full', 'note': 'saw 1/25 s and 16/9 and a/b/c'},
      ],
      'measuredWarm': false,
    },
    sensorProfile: p ?? profile,
    isoAnalysis: {'baseIso': 30},
    calibration: jsonDecode(const CalibrationRecord().withDone(kHotPixelsStep, 1, now, '0.29.0').encode()) as Map<String, dynamic>,
  );

  test('hot-pixel map is removed, its count kept', () {
    final r = build();
    final sp = r['sensorProfile'] as Map;
    expect(sp.containsKey('defects'), isFalse);
    expect(sp['darkNoise'], isNotEmpty);
    expect(sp['defectCount'], 3);
    expect(profile.containsKey('defects'), isTrue); // input untouched
    final withCount = build(p: {...profile, 'defectCount': 657});
    expect((withCount['sensorProfile'] as Map)['defectCount'], 657);
  });

  test('absolute paths are stripped, ordinary slashes survive', () {
    final text = encodeCalibrationReport(build());
    expect(text.contains('/data/'), isFalse);
    expect(text.contains('/storage/'), isFalse);
    expect(text.contains('filesDir'), isFalse);
    expect(text.contains('VSENSOR_x_profile.json'), isTrue);
    expect(text.contains('1/25 s and 16/9 and a/b/c'), isTrue);
    expect(stripPaths('failed at /data/user/0/app/files/x.json because of noise'), 'failed at x.json because of noise');
    expect(stripPaths('see /sdcard/Download/Vesper Calibration/a.json now'), contains('a.json'));
  });

  test('report content: format, versions, device, check, steps', () {
    final r = build();
    expect(r['format'], 'vesper-calibration-report/1');
    expect((r['app'] as Map)['version'], '0.29.0');
    expect((r['app'] as Map)['build'], '104');
    expect((r['device'] as Map)['sdk'], 36);
    expect((r['deviceCheck'] as Map)['measuredWarm'], false);
    expect(r['created'], '2026-11-02T12:00:00.000Z');
    expect(reportSteps(r), [kHotPixelsStep]);
    expect(reportSubject(r), 'Vesper Cine calibration: Google Pixel 10, app 0.29.0');
    expect(reportFileName(r, now), 'VesperCalibration_Pixel_10_20261102.json');
    expect(() => jsonDecode(encodeCalibrationReport(r)), returnsNormally);
  });

  test('ISO analysis and calibration record are whitelisted', () {
    final r = buildCalibrationReport(
      now: now,
      appVersion: 'v',
      appBuild: '1',
      device: null,
      cameraId: '0',
      isoAnalysis: {'format': 'vesper-iso-analysis/1', 'baseIso': 30, 'secret': 'x', 'steps': []},
      calibration: {
        'format': 'vesper-calibration/1',
        'promptedAppVersion': '0.29.0',
        'steps': {
          'hotPixels': {'version': 1, 'date': 'd', 'appVersion': 'a', 'extra': 1},
        },
      },
    );
    expect((r['isoAnalysis'] as Map).containsKey('secret'), isFalse);
    expect((r['isoAnalysis'] as Map)['baseIso'], 30);
    expect((r['calibration'] as Map).containsKey('promptedAppVersion'), isFalse);
    expect(((r['calibration'] as Map)['steps'] as Map)['hotPixels'], {'version': 1, 'date': 'd', 'appVersion': 'a'});
  });

  test('missing files give nulls, not a crash; send needs a check or a step', () {
    final r = buildCalibrationReport(now: now, appVersion: 'v', appBuild: '1', device: null, cameraId: '0');
    expect(r['sensorProfile'], isNull);
    expect(r['isoAnalysis'], isNull);
    expect(reportSteps(r), isEmpty);
    expect(hasCalibrationToSend(hasDeviceCheck: false, stepsDone: 0), isFalse);
    expect(hasCalibrationToSend(hasDeviceCheck: true, stepsDone: 0), isTrue);
    expect(hasCalibrationToSend(hasDeviceCheck: false, stepsDone: 1), isTrue);
  });

  test('the send-offered flag survives encode/decode and other updates', () {
    var rec = const CalibrationRecord().withSendOffered();
    rec = rec.withDone(kHotPixelsStep, 1, now, '0.29.0').withPrompted('0.29.0');
    expect(CalibrationRecord.decode(rec.encode())!.sendOffered, isTrue);
    expect(CalibrationRecord.decode(const CalibrationRecord().encode())!.sendOffered, isFalse);
  });
}
