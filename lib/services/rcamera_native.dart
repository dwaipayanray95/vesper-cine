import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';

// Native function signatures (C-ABI)
typedef NativeRCameraInit = Int32 Function();
typedef DartRCameraInit = int Function();

typedef NativeRCameraEnumerateCameras = Int32 Function(Pointer<Utf8> outJson, Int32 maxLen);
typedef DartRCameraEnumerateCameras = int Function(Pointer<Utf8> outJson, int maxLen);

typedef NativeRCameraOpenCamera = Int32 Function(Pointer<Utf8> cameraId);
typedef DartRCameraOpenCamera = int Function(Pointer<Utf8> cameraId);

typedef NativeRCameraStartStream = Int32 Function(Int32 width, Int32 height);
typedef DartRCameraStartStream = int Function(int width, int height);

typedef NativeRCameraStopStream = Int32 Function();
typedef DartRCameraStopStream = int Function();

typedef NativeRCameraSetExposure = Void Function(Int64 exposureNs, Int32 iso);
typedef DartRCameraSetExposure = void Function(int exposureNs, int iso);

typedef NativeRCameraSetShutterAngle = Void Function(Float shutterAngle, Float fps, Int32 iso);
typedef DartRCameraSetShutterAngle = void Function(double shutterAngle, double fps, int iso);

typedef NativeRCameraSetWhiteBalanceGains = Void Function(Float rGain, Float gGain, Float bGain);
typedef DartRCameraSetWhiteBalanceGains = void Function(double rGain, double gGain, double bGain);

typedef NativeRCameraSetKelvinTint = Void Function(Int32 kelvin, Int32 tint);
typedef DartRCameraSetKelvinTint = void Function(int kelvin, int tint);

typedef NativeRCameraSetOis = Void Function(Int32 enable);
typedef DartRCameraSetOis = void Function(int enable);

typedef NativeRCameraLockWbFromCenter = Int32 Function();
typedef DartRCameraLockWbFromCenter = int Function();

typedef NativeRCameraSetCropMode = Void Function(Int32 cropMode);
typedef DartRCameraSetCropMode = void Function(int cropMode);

typedef NativeRCameraSetMonitoringMode = Void Function(Int32 mode);
typedef DartRCameraSetMonitoringMode = void Function(int mode);

typedef NativeRCameraSetZebraThreshold = Void Function(Float threshold);
typedef DartRCameraSetZebraThreshold = void Function(double threshold);

typedef NativeRCameraClose = Void Function();
typedef DartRCameraClose = void Function();

class DetectedCamera {
  final String id;
  final int hwLevel;
  final bool supportsRaw10;
  final int rawWidth;
  final int rawHeight;

  DetectedCamera({
    required this.id,
    required this.hwLevel,
    required this.supportsRaw10,
    required this.rawWidth,
    required this.rawHeight,
  });

  factory DetectedCamera.fromJson(Map<String, dynamic> json) {
    return DetectedCamera(
      id: json['id'] as String,
      hwLevel: json['hwLevel'] as int,
      supportsRaw10: json['supportsRaw10'] as bool,
      rawWidth: json['rawWidth'] as int,
      rawHeight: json['rawHeight'] as int,
    );
  }
}

class RCameraNative {
  static RCameraNative? _instance;
  static RCameraNative get instance => _instance ??= RCameraNative._();

  static const MethodChannel _channel = MethodChannel('com.rawedge.r_camera/texture');

  DynamicLibrary? _dylib;

  late final DartRCameraInit _init;
  late final DartRCameraEnumerateCameras _enumerateCameras;
  late final DartRCameraOpenCamera _openCamera;
  late final DartRCameraStartStream _startStream;
  late final DartRCameraStopStream _stopStream;
  late final DartRCameraSetExposure _setExposure;
  late final DartRCameraSetShutterAngle _setShutterAngle;
  late final DartRCameraSetWhiteBalanceGains _setWhiteBalanceGains;
  late final DartRCameraSetKelvinTint _setKelvinTint;
  late final DartRCameraLockWbFromCenter _lockWbFromCenter;
  late final DartRCameraSetOis _setOis;
  late final DartRCameraSetCropMode _setCropMode;
  late final DartRCameraSetMonitoringMode _setMonitoringMode;
  late final DartRCameraSetZebraThreshold _setZebraThreshold;
  late final DartRCameraClose _close;

  bool _isLoaded = false;
  bool get isLoaded => _isLoaded;

  RCameraNative._() {
    _loadLibrary();
  }

  void _loadLibrary() {
    try {
      if (Platform.isAndroid) {
        _dylib = DynamicLibrary.open('librcamera_engine.so');
      } else {
        // Fallback for Windows desktop offline test harness
        _dylib = DynamicLibrary.process();
      }

      _init = _dylib!.lookupFunction<NativeRCameraInit, DartRCameraInit>('rcamera_init');
      _enumerateCameras = _dylib!.lookupFunction<NativeRCameraEnumerateCameras, DartRCameraEnumerateCameras>('rcamera_enumerate_cameras');
      _openCamera = _dylib!.lookupFunction<NativeRCameraOpenCamera, DartRCameraOpenCamera>('rcamera_open_camera');
      _startStream = _dylib!.lookupFunction<NativeRCameraStartStream, DartRCameraStartStream>('rcamera_start_stream');
      _stopStream = _dylib!.lookupFunction<NativeRCameraStopStream, DartRCameraStopStream>('rcamera_stop_stream');
      _setExposure = _dylib!.lookupFunction<NativeRCameraSetExposure, DartRCameraSetExposure>('rcamera_set_exposure');
      _setShutterAngle = _dylib!.lookupFunction<NativeRCameraSetShutterAngle, DartRCameraSetShutterAngle>('rcamera_set_shutter_angle');
      _setWhiteBalanceGains = _dylib!.lookupFunction<NativeRCameraSetWhiteBalanceGains, DartRCameraSetWhiteBalanceGains>('rcamera_set_white_balance_gains');
      _setKelvinTint = _dylib!.lookupFunction<NativeRCameraSetKelvinTint, DartRCameraSetKelvinTint>('rcamera_set_kelvin_tint');
      _lockWbFromCenter = _dylib!.lookupFunction<NativeRCameraLockWbFromCenter, DartRCameraLockWbFromCenter>('rcamera_lock_white_balance_from_center');
      _setOis = _dylib!.lookupFunction<NativeRCameraSetOis, DartRCameraSetOis>('rcamera_set_ois');
      _setCropMode = _dylib!.lookupFunction<NativeRCameraSetCropMode, DartRCameraSetCropMode>('rcamera_set_crop_mode');
      _setMonitoringMode = _dylib!.lookupFunction<NativeRCameraSetMonitoringMode, DartRCameraSetMonitoringMode>('rcamera_set_monitoring_mode');
      _setZebraThreshold = _dylib!.lookupFunction<NativeRCameraSetZebraThreshold, DartRCameraSetZebraThreshold>('rcamera_set_zebra_threshold');
      _close = _dylib!.lookupFunction<NativeRCameraClose, DartRCameraClose>('rcamera_close');

      _isLoaded = true;
      // Native library not loaded (e.g. running on non-Android platform)
    } catch (_) {
      _isLoaded = false;
    }
  }

  bool initialize() {
    if (!_isLoaded) return false;
    return _init() == 0;
  }

  List<DetectedCamera> enumerateCameras() {
    if (!_isLoaded) return [];
    const maxLen = 4096;
    final ptr = calloc<Uint8>(maxLen).cast<Utf8>();
    try {
      final res = _enumerateCameras(ptr, maxLen);
      if (res > 0) {
        final jsonStr = ptr.toDartString();
        final List<dynamic> list = jsonDecode(jsonStr);
        return list.map((item) => DetectedCamera.fromJson(item as Map<String, dynamic>)).toList();
      }
      return [];
    } finally {
      calloc.free(ptr);
    }
  }

  bool openCamera(String cameraId) {
    if (!_isLoaded) return false;
    final ptr = cameraId.toNativeUtf8();
    try {
      return _openCamera(ptr) == 0;
    } finally {
      calloc.free(ptr);
    }
  }

  bool startStream(int width, int height) {
    if (!_isLoaded) return false;
    return _startStream(width, height) == 0;
  }

  void stopStream() {
    if (!_isLoaded) return;
    _stopStream();
  }

  void setExposure({required int exposureNs, required int iso}) {
    if (!_isLoaded) return;
    _setExposure(exposureNs, iso);
  }

  void setShutterAngle({required double shutterAngle, required double fps, required int iso}) {
    if (!_isLoaded) return;
    _setShutterAngle(shutterAngle, fps, iso);
  }

  void setWhiteBalanceGains(double rGain, double gGain, double bGain) {
    if (!_isLoaded) return;
    _setWhiteBalanceGains(rGain, gGain, bGain);
  }

  void setKelvinTint({required int kelvin, required int tint}) {
    if (!_isLoaded) return;
    _setKelvinTint(kelvin, tint);
  }

  /// Samples the current viewfinder's center patch and adjusts white balance
  /// so it renders neutral. Returns true on success (a frame was available
  /// and bright enough to sample); false if there's nothing usable to lock
  /// onto yet (e.g. still initializing, or pointed at near-black).
  bool lockWhiteBalanceFromCenter() {
    if (!_isLoaded) return false;
    return _lockWbFromCenter() == 0;
  }

  void setOis(bool enable) {
    if (!_isLoaded) return;
    _setOis(enable ? 1 : 0);
  }

  void setCropMode(int cropMode) {
    // 0: 16:9 UHD crop, 1: 4:3 open gate
    if (!_isLoaded) return;
    _setCropMode(cropMode);
  }

  void setMonitoringMode(int mode) {
    // 0: R-Log flat, 1: Rec.709 preview LUT, 2: False Color, 3: Focus Peaking, 4: Zebras
    if (!_isLoaded) return;
    _setMonitoringMode(mode);
  }

  void setZebraThreshold(double threshold) {
    if (!_isLoaded) return;
    _setZebraThreshold(threshold);
  }

  Future<int?> createViewfinderTexture({int width = 1920, int height = 1080}) async {
    if (!Platform.isAndroid) return null;
    try {
      final textureId = await _channel.invokeMethod<int>('createTexture', {
        'width': width,
        'height': height,
      });
      return textureId;
    } on PlatformException {
      // Failed to create texture
      return null;
    }
  }

  Future<void> destroyViewfinderTexture() async {
    if (!Platform.isAndroid) return;
    try {
      await _channel.invokeMethod<bool>('destroyTexture');
    } on PlatformException {
      // Ignored
    }
  }

  void close() {
    if (!_isLoaded) return;
    _close();
  }
}
