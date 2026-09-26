import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';

class DetectedCamera {
  final String id;
  final int facing;
  final bool supportsRaw10;
  final int rawWidth;
  final int rawHeight;
  final double maxFps;

  DetectedCamera.fromJson(Map<String, dynamic> json)
    : id = json['id'] as String,
      facing = json['facing'] as int,
      supportsRaw10 = json['supportsRaw10'] as bool,
      rawWidth = json['rawWidth'] as int,
      rawHeight = json['rawHeight'] as int,
      maxFps = (json['maxFps'] as num).toDouble();
}

/// Snapshot of the native pipeline, polled by the UI.
class EngineStatus {
  final bool streaming;
  final double fps;
  final int cameraDrops;
  final double kelvin;
  final double tint;
  final bool recording;
  final int durationMs;
  final int framesEncoded;
  final int framesDropped;
  final int thermal; // AThermalStatus: 0 none, 1 light, 2 moderate, 3 severe
  final bool audio;
  final String codec;
  final String stopReason;
  final int exposureNs; // actual sensor exposure of the latest frame
  final int iso; // actual sensor sensitivity of the latest frame
  final bool awbAuto; // white balance currently follows Google's AWB
  final int afState; // CONTROL_AF_STATE (2 = passive focused, 4 = focus locked)
  final double focusDiopters;
  final List<double> face; // largest face: x, y, w, h (output-normalised); w == 0 if none
  final double gpuMs; // measured GPU time per frame
  final bool alignThrottled; // NR alignment auto-disabled: GPU over budget

  EngineStatus.fromJson(Map<String, dynamic> j)
    : streaming = j['streaming'] as bool,
      fps = (j['fps'] as num).toDouble(),
      cameraDrops = j['cameraDrops'] as int,
      kelvin = (j['kelvin'] as num).toDouble(),
      tint = (j['tint'] as num).toDouble(),
      recording = j['recording'] as bool,
      durationMs = j['durationMs'] as int,
      framesEncoded = j['framesEncoded'] as int,
      framesDropped = j['framesDropped'] as int,
      thermal = j['thermal'] as int,
      audio = j['audio'] as bool,
      codec = j['codec'] as String,
      stopReason = j['stopReason'] as String,
      exposureNs = j['exposureNs'] as int,
      iso = j['iso'] as int,
      awbAuto = j['awbAuto'] as bool,
      afState = j['afState'] as int,
      focusDiopters = (j['focusDiopters'] as num).toDouble(),
      face = (j['face'] as List).map((e) => (e as num).toDouble()).toList(),
      gpuMs = (j['gpuMs'] as num).toDouble(),
      alignThrottled = j['alignThrottled'] as bool;
}

class RawMode {
  final int width, height;
  final double maxFps;
  RawMode(this.width, this.height, this.maxFps);
  double get aspect => width / height;
}

/// Hardware limits of the open camera.
class CameraCapabilities {
  final int minExposureNs, maxExposureNs, minIso, maxIso;
  final double minFocusDiopters;
  final List<RawMode> modes;

  CameraCapabilities.fromJson(Map<String, dynamic> j)
    : minExposureNs = j['minExposureNs'] as int,
      maxExposureNs = j['maxExposureNs'] as int,
      minIso = j['minIso'] as int,
      maxIso = j['maxIso'] as int,
      minFocusDiopters = (j['minFocus'] as num).toDouble(),
      modes = (j['modes'] as List)
          .map((m) => RawMode(m['w'] as int, m['h'] as int, (m['maxFps'] as num).toDouble()))
          .toList();

  /// Highest frame rate available for the crop (16:9 may use a faster readout).
  double maxFpsFor(int cropMode) {
    final want = cropMode == 1 ? 4 / 3 : 16 / 9;
    final fullWidth = modes.isEmpty ? 0 : modes.map((m) => m.width).reduce((a, b) => a > b ? a : b);
    double best = 0;
    for (final m in modes) {
      if (m.width < fullWidth * 0.9) continue;
      if (cropMode == 1 && (m.aspect - want).abs() > 0.05) continue;
      if (m.maxFps > best) best = m.maxFps;
    }
    return best == 0 ? 30 : best;
  }
}

class RecordingFile {
  final int fd;
  final String uri;
  final String name;
  RecordingFile(this.fd, this.uri, this.name);
}

/// Dart side of the Vesper engine (android/app/src/main/cpp/native_bridge.cpp).
class VesperNative {
  static final VesperNative instance = VesperNative._();
  static const MethodChannel _channel = MethodChannel('com.vesper.cine/native');

  late final DynamicLibrary _lib;
  bool _loaded = false;
  bool get isLoaded => _loaded;

  late final int Function() _init;
  late final int Function(Pointer<Utf8>, int) _enumerate;
  late final int Function(Pointer<Utf8>) _open;
  late final int Function() _startStream;
  late final int Function() _stopStream;
  late final void Function(double) _setFrameRate;
  late final void Function(double, int) _setShutterAngle;
  late final void Function(int, int) _setExposureTime;
  late final int Function(Pointer<Utf8>, int) _capabilities;
  late final int Function(int, Pointer<Int64>, Pointer<Int32>) _autoExpose;
  late final void Function() _closeCamera;
  late final void Function(int) _setAutoWb;
  late final void Function(int) _setFocusMode;
  late final void Function(double, double) _setFocusPoint;
  late final void Function(int) _setFaceDetection;
  late final void Function(int) _setLensCorrection;
  late final void Function(int) _setHotPixelFix;
  late final void Function(double) _setTemporalNr;
  late final void Function(double) _setChromaNr;
  late final void Function(int) _setNrAlignment;
  late final void Function(int, int) _setKelvinTint;
  late final int Function(Pointer<Double>, Pointer<Double>) _lockWb;
  late final void Function(int) _setOis;
  late final void Function(double) _setFocus;
  late final double Function() _minFocus;
  late final void Function(int) _setCropMode;
  late final void Function(int) _setResolution;
  late final void Function(Pointer<Int32>, Pointer<Int32>) _outputSize;
  late final void Function(int) _setMonitoringMode;
  late final void Function(double) _setZebra;
  late final void Function(double) _setHeadroom;
  late final int Function(int, int, int) _startRecording;
  late final void Function() _stopRecording;
  late final int Function(Pointer<Utf8>, int) _status;
  late final void Function() _close;

  VesperNative._() {
    if (!Platform.isAndroid) return;
    try {
      _lib = DynamicLibrary.open('libvesper_engine.so');
      _init = _lib.lookupFunction<Int32 Function(), int Function()>('vesper_init');
      _enumerate = _lib.lookupFunction<Int32 Function(Pointer<Utf8>, Int32), int Function(Pointer<Utf8>, int)>(
        'vesper_enumerate_cameras',
      );
      _open = _lib.lookupFunction<Int32 Function(Pointer<Utf8>), int Function(Pointer<Utf8>)>('vesper_open_camera');
      _startStream = _lib.lookupFunction<Int32 Function(), int Function()>('vesper_start_stream');
      _stopStream = _lib.lookupFunction<Int32 Function(), int Function()>('vesper_stop_stream');
      _setFrameRate = _lib.lookupFunction<Void Function(Double), void Function(double)>('vesper_set_frame_rate');
      _setShutterAngle = _lib.lookupFunction<Void Function(Double, Int32), void Function(double, int)>(
        'vesper_set_shutter_angle',
      );
      _setExposureTime = _lib.lookupFunction<Void Function(Int64, Int32), void Function(int, int)>(
        'vesper_set_exposure_time',
      );
      _capabilities = _lib.lookupFunction<Int32 Function(Pointer<Utf8>, Int32), int Function(Pointer<Utf8>, int)>(
        'vesper_get_capabilities',
      );
      _autoExpose = _lib
          .lookupFunction<
            Int32 Function(Int32, Pointer<Int64>, Pointer<Int32>),
            int Function(int, Pointer<Int64>, Pointer<Int32>)
          >('vesper_auto_expose');
      _closeCamera = _lib.lookupFunction<Void Function(), void Function()>('vesper_close_camera');
      _setAutoWb = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_auto_white_balance');
      _setFocusMode = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_focus_mode');
      _setFocusPoint = _lib.lookupFunction<Void Function(Float, Float), void Function(double, double)>(
        'vesper_set_focus_point',
      );
      _setFaceDetection = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_face_detection');
      _setLensCorrection = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_lens_correction');
      _setHotPixelFix = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_hot_pixel_fix');
      _setTemporalNr = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_set_temporal_nr');
      _setNrAlignment = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_nr_alignment');
      _setChromaNr = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_set_chroma_nr');
      _setKelvinTint = _lib.lookupFunction<Void Function(Int32, Int32), void Function(int, int)>(
        'vesper_set_kelvin_tint',
      );
      _lockWb = _lib
          .lookupFunction<
            Int32 Function(Pointer<Double>, Pointer<Double>),
            int Function(Pointer<Double>, Pointer<Double>)
          >('vesper_lock_white_balance');
      _setOis = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_ois');
      _setFocus = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_set_focus');
      _minFocus = _lib.lookupFunction<Float Function(), double Function()>('vesper_get_min_focus');
      _setCropMode = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_crop_mode');
      _setResolution = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_resolution');
      _outputSize = _lib
          .lookupFunction<Void Function(Pointer<Int32>, Pointer<Int32>), void Function(Pointer<Int32>, Pointer<Int32>)>(
            'vesper_get_output_size',
          );
      _setMonitoringMode = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_monitoring_mode');
      _setZebra = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_set_zebra_threshold');
      _setHeadroom = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_set_highlight_headroom');
      _startRecording = _lib.lookupFunction<Int32 Function(Int32, Int32, Int32), int Function(int, int, int)>(
        'vesper_start_recording',
      );
      _stopRecording = _lib.lookupFunction<Void Function(), void Function()>('vesper_stop_recording');
      _status = _lib.lookupFunction<Int32 Function(Pointer<Utf8>, Int32), int Function(Pointer<Utf8>, int)>(
        'vesper_get_status',
      );
      _close = _lib.lookupFunction<Void Function(), void Function()>('vesper_close');
      _loaded = true;
    } catch (_) {
      _loaded = false;
    }
  }

  bool initialize() => _loaded && _init() == 0;

  String? _readJson(int Function(Pointer<Utf8>, int) fn) {
    const maxLen = 8192;
    final buf = calloc<Uint8>(maxLen).cast<Utf8>();
    try {
      return fn(buf, maxLen) > 0 ? buf.toDartString() : null;
    } finally {
      calloc.free(buf);
    }
  }

  List<DetectedCamera> enumerateCameras() {
    if (!_loaded) return [];
    final json = _readJson(_enumerate);
    if (json == null) return [];
    return (jsonDecode(json) as List).map((e) => DetectedCamera.fromJson(e as Map<String, dynamic>)).toList();
  }

  bool openCamera(String id) {
    if (!_loaded) return false;
    final p = id.toNativeUtf8();
    try {
      return _open(p) == 0;
    } finally {
      calloc.free(p);
    }
  }

  bool startStream() => _loaded && _startStream() == 0;
  void stopStream() => _loaded ? _stopStream() : null;
  void setFrameRate(double fps) => _loaded ? _setFrameRate(fps) : null;
  void setShutterAngle(double angle, int iso) => _loaded ? _setShutterAngle(angle, iso) : null;

  /// Shutter as a speed; independent of frame rate (clamped to the frame duration).
  void setExposureTime(int exposureNs, int iso) => _loaded ? _setExposureTime(exposureNs, iso) : null;

  /// true: follow Google's AWB (HAL neutral point); false: manual Kelvin/tint.
  void setAutoWhiteBalance(bool on) => _loaded ? _setAutoWb(on ? 1 : 0) : null;

  /// Continuous AF (PDAF + laser) or manual. Leaving continuous locks focus where it is.
  void setContinuousFocus(bool on) => _loaded ? _setFocusMode(on ? 1 : 0) : null;

  /// Tap-to-focus at an upright viewfinder point (0..1); enables continuous AF on that region.
  void setFocusPoint(double x, double y) => _loaded ? _setFocusPoint(x, y) : null;
  void setFaceDetection(bool on) => _loaded ? _setFaceDetection(on ? 1 : 0) : null;
  void setLensCorrection(bool on) => _loaded ? _setLensCorrection(on ? 1 : 0) : null;
  void setHotPixelFix(bool on) => _loaded ? _setHotPixelFix(on ? 1 : 0) : null;

  /// 0 = off, else max weight of the previous frame (0.5 low … 0.85 high).
  void setTemporalNr(double strength) => _loaded ? _setTemporalNr(strength) : null;

  /// Motion-aligned temporal NR (tile alignment); keeps denoising while the camera moves.
  void setNrAlignment(bool on) => _loaded ? _setNrAlignment(on ? 1 : 0) : null;

  /// 0 = off … 1 = full chroma smoothing (luma untouched).
  void setChromaNr(double strength) => _loaded ? _setChromaNr(strength) : null;
  void closeCamera() => _loaded ? _closeCamera() : null;

  CameraCapabilities? capabilities() {
    if (!_loaded) return null;
    final json = _readJson(_capabilities);
    return json == null ? null : CameraCapabilities.fromJson(jsonDecode(json) as Map<String, dynamic>);
  }

  /// One-shot exposure assist. keepShutter: move ISO first (keeps motion blur);
  /// otherwise move the shutter first. Returns (exposureNs, iso) or null.
  (int, int)? autoExpose({bool keepShutter = true}) {
    if (!_loaded) return null;
    final ns = calloc<Int64>();
    final iso = calloc<Int32>();
    try {
      return _autoExpose(keepShutter ? 0 : 1, ns, iso) == 0 ? (ns.value, iso.value) : null;
    } finally {
      calloc.free(ns);
      calloc.free(iso);
    }
  }

  void setKelvinTint(int kelvin, int tint) => _loaded ? _setKelvinTint(kelvin, tint) : null;
  void setOis(bool on) => _loaded ? _setOis(on ? 1 : 0) : null;
  void setFocus(double diopters) => _loaded ? _setFocus(diopters) : null;
  double get minFocusDiopters => _loaded ? _minFocus() : 0;
  void setCropMode(int mode) => _loaded ? _setCropMode(mode) : null;
  void setResolution(int res) => _loaded ? _setResolution(res) : null;
  void setMonitoringMode(int mode) => _loaded ? _setMonitoringMode(mode) : null;
  void setZebraThreshold(double t) => _loaded ? _setZebra(t) : null;

  /// Stops of highlight headroom above 18% grey before sensor clip (3-6.3).
  void setHighlightHeadroom(double stops) => _loaded ? _setHeadroom(stops) : null;

  /// Meters the raw frame centre and sets white balance so it renders
  /// neutral. Returns the equivalent (kelvin, tint), or null if the centre is
  /// too dark or no frame has arrived yet.
  (double, double)? lockWhiteBalance() {
    if (!_loaded) return null;
    final k = calloc<Double>();
    final t = calloc<Double>();
    try {
      return _lockWb(k, t) == 0 ? (k.value, t.value) : null;
    } finally {
      calloc.free(k);
      calloc.free(t);
    }
  }

  (int, int) outputSize() {
    if (!_loaded) return (1920, 1080);
    final w = calloc<Int32>();
    final h = calloc<Int32>();
    try {
      _outputSize(w, h);
      return (w.value, h.value);
    } finally {
      calloc.free(w);
      calloc.free(h);
    }
  }

  EngineStatus? status() {
    if (!_loaded) return null;
    final json = _readJson(_status);
    return json == null ? null : EngineStatus.fromJson(jsonDecode(json) as Map<String, dynamic>);
  }

  /// Creates the output file in Movies/Vesper Cine and starts recording.
  /// codec: 0 = HEVC Main10, 1 = AV1 Main10 (falls back to HEVC if absent).
  Future<RecordingFile?> startRecording({int codec = 0, bool audio = true}) async {
    if (!_loaded) return null;
    final m = await _channel.invokeMapMethod<String, dynamic>('createRecordingFile');
    if (m == null) return null;
    final file = RecordingFile(m['fd'] as int, m['uri'] as String, m['name'] as String);
    if (_startRecording(file.fd, codec, audio ? 1 : 0) != 0) {
      await finalizeRecording(file, keep: false);
      return null;
    }
    return file;
  }

  void stopRecording() => _loaded ? _stopRecording() : null;

  Future<void> finalizeRecording(RecordingFile file, {bool keep = true}) =>
      _channel.invokeMethod('finalizeRecordingFile', {'uri': file.uri, 'keep': keep});

  Future<int?> createViewfinderTexture(int width, int height) async {
    if (!Platform.isAndroid) return null;
    try {
      return await _channel.invokeMethod<int>('createTexture', {'width': width, 'height': height});
    } on PlatformException {
      return null;
    }
  }

  Future<void> resizeViewfinderTexture(int width, int height) async {
    if (!Platform.isAndroid) return;
    await _channel.invokeMethod('resizeTexture', {'width': width, 'height': height});
  }

  Future<void> destroyViewfinderTexture() async {
    if (!Platform.isAndroid) return;
    await _channel.invokeMethod('destroyTexture');
  }

  void close() => _loaded ? _close() : null;
}
