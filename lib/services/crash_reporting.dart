import 'package:firebase_core/firebase_core.dart';
import 'package:firebase_crashlytics/firebase_crashlytics.dart';
import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

/// Crash reports via Firebase Crashlytics: release builds only, and only while
/// the user's switch (Settings > Info > Send crash reports) is on (default on).
/// Reports hold the crash stack, device model and Android version, app version;
/// no footage, no location, no account (see the privacy policy).
/// Never throws: without a working Firebase config the app simply runs without it.
class CrashReporting {
  static const _prefKey = 'sendCrashReports';
  static bool _ready = false;
  static bool _enabled = true;

  /// Whether reports are being sent (switch state).
  static bool get enabled => _enabled;

  /// True when this build can send reports at all (release build, Firebase up).
  static bool get available => _ready;

  static Future<void> init() async {
    if (!kReleaseMode) return;
    try {
      final prefs = await SharedPreferences.getInstance();
      _enabled = prefs.getBool(_prefKey) ?? true;
      await Firebase.initializeApp();
      await FirebaseCrashlytics.instance.setCrashlyticsCollectionEnabled(_enabled);
      FlutterError.onError = FirebaseCrashlytics.instance.recordFlutterFatalError;
      PlatformDispatcher.instance.onError = (error, stack) {
        FirebaseCrashlytics.instance.recordError(error, stack, fatal: true);
        return true;
      };
      _ready = true;
    } catch (e) {
      debugPrint('Crash reporting unavailable: $e');
    }
  }

  static Future<void> setEnabled(bool on) async {
    _enabled = on;
    try {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setBool(_prefKey, on);
      if (_ready) await FirebaseCrashlytics.instance.setCrashlyticsCollectionEnabled(on);
    } catch (e) {
      debugPrint('Crash reporting switch failed: $e');
    }
  }

  /// A non-fatal problem worth seeing in the console (e.g. a recording error).
  static void note(Object error, [StackTrace? stack]) {
    if (_ready && _enabled) FirebaseCrashlytics.instance.recordError(error, stack);
  }
}
