import 'package:flutter_test/flutter_test.dart';
import 'package:vesper_cine/services/gyroflow.dart';

void main() {
  test('Gyroflow data needs the switch and Pro', () {
    expect(gyroflowActive(setting: true, hasPro: true), isTrue);
    expect(gyroflowActive(setting: true, hasPro: false), isFalse);
    expect(gyroflowActive(setting: false, hasPro: true), isFalse);
  });

  test('saved note names each folder once', () {
    expect(gyroflowSavedNote(const []), isNull);
    expect(gyroflowSavedNote(const ['Movies/Vesper Cine', 'Movies/Vesper Cine']), 'Gyroflow files (.gcsv, _lens.json) saved to Movies/Vesper Cine');
    expect(gyroflowSavedNote(const ['Movies/Vesper Cine', 'Download/Vesper Cine']),
        'Gyroflow files (.gcsv, _lens.json) saved to Movies/Vesper Cine and Download/Vesper Cine');
  });

  test('orientation override options are valid axis strings', () {
    expect(gyroOrientationForEngine('AUTO'), '');
    for (final o in gyroOrientationOptions.skip(1)) {
      expect(o.length, 3);
      expect(o.toLowerCase().split('').toSet(), {'x', 'y', 'z'});
      expect(gyroOrientationForEngine(o), o);
    }
  });
}
