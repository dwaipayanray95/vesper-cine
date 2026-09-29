import 'package:flutter/foundation.dart';

/// Developer tools (chart calibration capture, native ISO analysis, ...).
///
/// On in debug/profile builds, off in release builds, so testers only see
/// finished features. Force either way with
///   flutter build apk --release --dart-define=VESPER_DEV_TOOLS=true
/// (the GitHub build workflow has a "dev_tools" switch for this).
const bool kDevTools = bool.fromEnvironment('VESPER_DEV_TOOLS', defaultValue: !kReleaseMode);
