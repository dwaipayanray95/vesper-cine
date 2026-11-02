/// Gyroflow motion data next to each clip (0.28.0). Pure helpers; the
/// recording itself is native (imu_logger.cpp).
///
/// A take writes the file only when the switch is on AND Pro (or an ad pass)
/// is active at record start; free users see the switch locked.
bool gyroflowActive({required bool setting, required bool hasPro}) => setting && hasPro;

/// What the user is told after a take, from the folders the files were put in.
/// Empty when no motion file was written (no gyro, or the log failed).
String? gyroflowSavedNote(List<String> folders) {
  if (folders.isEmpty) return null;
  final unique = folders.toSet().toList();
  return 'Gyroflow files (.gcsv, _lens.json) saved to ${unique.join(' and ')}';
}

/// Developer override of the gcsv IMU orientation (Settings › Developer): AUTO
/// uses the app's own derivation; the rest are the plausible phone strings.
const gyroOrientationOptions = ['AUTO', 'yxz', 'YXz', 'Yxz', 'yXz', 'YXZ', 'yxZ', 'YxZ', 'yXZ', 'Xyz', 'xYz'];

/// The string sent to the engine ('' = automatic).
String gyroOrientationForEngine(String choice) => choice == 'AUTO' ? '' : choice;
