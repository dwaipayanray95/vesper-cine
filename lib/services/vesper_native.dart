import 'dart:typed_data';
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
  final int thermal; // AThermalStatus: 0 none, 1 light, 2 moderate, 3 severe, 4 critical
  final int heatLevel; // engine heat level: 0 cool, 1 warm (hold), 2 hot (stepping down), 3 severe (minimal)
  final double heatHeadroom; // Android's 10 s forecast, 1.0 = severe; -1 if unsupported
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
  final bool alignReduced; // guard's first step: motion search every 4th frame instead of every 2nd
  final bool nrThrottled; // temporal/chroma NR also paused: GPU still over budget
  final bool hqAvailable; // HQ oversampling supported by the GPU and not paused by the budget guard
  final bool hqSupported; // the GPU can run HQ oversampling at all
  final String calibrationSaved; // base path of the last calibration frame written
  final bool profileActive; // per-device chart calibration in use
  final bool focusPulling; // smooth manual focus pull running
  final bool exposureRamping; // auto-exposure glide in progress
  final double isoSweep; // native-ISO analysis progress 0..1, -1 when idle
  final String isoSweepResult; // result JSON path of the last successful analysis
  final String isoSweepError;
  final bool focusLocked; // focus held (AF-L)
  final bool gpuOverloaded; // GPU over budget with nothing left to pause: this frame rate drops frames
  final double calSweep; // sensor calibration sweep progress 0..1, -1 when idle
  final String calSweepResult; // base path of the last sweep written (<base>.json, <base>_ref.*)
  final String calSweepError;
  final String calSweepStage; // e.g. "ISO 400 · 1/1000 s"

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
      heatLevel = (j['heatLevel'] as int?) ?? 0,
      heatHeadroom = ((j['heatHeadroom'] as num?) ?? -1).toDouble(),
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
      alignThrottled = j['alignThrottled'] as bool,
      alignReduced = (j['alignReduced'] as bool?) ?? false,
      nrThrottled = j['nrThrottled'] as bool,
      hqAvailable = (j['hqAvailable'] as bool?) ?? false,
      hqSupported = (j['hqSupported'] as bool?) ?? false,
      calibrationSaved = (j['calibrationSaved'] as String?) ?? '',
      profileActive = (j['profileActive'] as bool?) ?? false,
      focusPulling = (j['focusPulling'] as bool?) ?? false,
      exposureRamping = (j['exposureRamping'] as bool?) ?? false,
      isoSweep = (j['isoSweep'] as num?)?.toDouble() ?? -1,
      isoSweepResult = (j['isoSweepResult'] as String?) ?? '',
      isoSweepError = (j['isoSweepError'] as String?) ?? '',
      focusLocked = (j['focusLocked'] as bool?) ?? false,
      gpuOverloaded = (j['gpuOverloaded'] as bool?) ?? false,
      calSweep = (j['calSweep'] as num?)?.toDouble() ?? -1,
      calSweepResult = (j['calSweepResult'] as String?) ?? '',
      calSweepError = (j['calSweepError'] as String?) ?? '',
      calSweepStage = (j['calSweepStage'] as String?) ?? '';
}

/// Luma scopes of the recorded Apple Log signal (see vesper_get_scopes).
class Scopes {
  static const histBins = 64, waveCols = 128, waveBins = 64;
  final Float32List histogram;
  final Float32List waveform;
  Scopes(this.histogram, this.waveform);
}

/// Measured gain structure of the sensor (vesper-iso-analysis/1).
class IsoAnalysis {
  final int baseIso, hcgIso, digitalFromIso;
  final List<int> nativeIsos;
  IsoAnalysis(this.baseIso, this.hcgIso, this.digitalFromIso, this.nativeIsos);

  static IsoAnalysis? fromJson(Map<String, dynamic> j) {
    if (j['format'] != 'vesper-iso-analysis/1') return null;
    return IsoAnalysis(
      j['baseIso'] as int,
      j['hcgIso'] as int,
      j['digitalFromIso'] as int,
      (j['nativeIsos'] as List).cast<int>(),
    );
  }

  String get summary =>
      'Native ISO ${nativeIsos.join(' & ')}'
      '${hcgIso > 0 ? ' (dual gain)' : ''}'
      '${digitalFromIso > 0 ? ' · digital gain from $digitalFromIso' : ''}';
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
  final int maxAnalogIso; // above this the phone uses digital gain (0 = not reported)
  final double minFocusDiopters;
  final List<RawMode> modes;

  CameraCapabilities.fromJson(Map<String, dynamic> j)
    : minExposureNs = j['minExposureNs'] as int,
      maxExposureNs = j['maxExposureNs'] as int,
      minIso = j['minIso'] as int,
      maxIso = j['maxIso'] as int,
      maxAnalogIso = (j['maxAnalogIso'] as int?) ?? 0,
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
  static const MethodChannel _channel = MethodChannel('com.theawesomeray.vespercine/native');

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
  late final void Function(double) _focusPullTo;
  late final void Function(double, double) _pickWb;
  late final void Function(int) _setScopes;
  late final void Function(int) _setSharpening;
  late final void Function(double, double, double) _setViewfinderZoom;
  late final int Function(Pointer<Utf8>, int) _getLog;
  late final void Function() _clearLog;
  late final void Function(Pointer<Utf8>, Pointer<Utf8>) _logLine;
  late final void Function(int) _setOversampling;
  late final void Function(int) _setProcessingPaused;
  late final void Function(int, int) _setNativeIsos;
  late final void Function(int) _setBudgetGuard;
  late final void Function(int) _setExperiments;
  late final void Function(int) _setViewfinderPaused;
  late final void Function(int) _setAlignInterval;
  late final void Function(int) _setRepeatPass;
  late final int Function(Pointer<Utf8>, Pointer<Utf8>) _isoSweepStart;
  late final void Function() _isoSweepCancel;
  late final int Function(int, Pointer<Utf8>, Pointer<Utf8>) _sensorSweepStart;
  late final void Function() _sensorSweepCancel;
  late final int Function(Pointer<Float>, Pointer<Float>) _getScopes;
  late final void Function(double, double, int) _focusAt;
  late final void Function(double) _setFocusSpeed;
  late final void Function() _focusLock;
  late final void Function(int, double, double) _setMetering;
  late final void Function(Pointer<Utf8>, Pointer<Utf8>) _captureCalibration;
  late final void Function(int, Pointer<Float>, Pointer<Float>) _setColorProfile;
  late final void Function(int, Pointer<Float>, Pointer<Float>, int, Pointer<Int32>) _setSensorProfile;
  late final void Function(int, Pointer<Float>, Pointer<Float>) _setShotNoiseProfile;
  late final void Function(double) _setRecordingQuality;
  late final void Function(int) _useColorProfile;

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
      _focusAt = _lib.lookupFunction<Void Function(Float, Float, Int32), void Function(double, double, int)>(
        'vesper_focus_at',
      );
      _isoSweepStart = _lib
          .lookupFunction<Int32 Function(Pointer<Utf8>, Pointer<Utf8>), int Function(Pointer<Utf8>, Pointer<Utf8>)>(
            'vesper_iso_sweep_start',
          );
      _isoSweepCancel = _lib.lookupFunction<Void Function(), void Function()>('vesper_iso_sweep_cancel');
      _sensorSweepStart = _lib.lookupFunction<
        Int32 Function(Int32, Pointer<Utf8>, Pointer<Utf8>),
        int Function(int, Pointer<Utf8>, Pointer<Utf8>)
      >('vesper_sensor_sweep_start');
      _sensorSweepCancel = _lib.lookupFunction<Void Function(), void Function()>('vesper_sensor_sweep_cancel');
      _setNativeIsos = _lib.lookupFunction<Void Function(Int32, Int32), void Function(int, int)>('vesper_set_native_isos');
      _setBudgetGuard = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_budget_guard');
      _setExperiments = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_experiments');
      _setViewfinderPaused =
          _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_viewfinder_paused');
      _setAlignInterval = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_align_interval');
      _setRepeatPass = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_repeat_pass');
      _setProcessingPaused = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_processing_paused');
      _setOversampling = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_oversampling');
      _setSharpening = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_sharpening');
      _setViewfinderZoom = _lib.lookupFunction<Void Function(Float, Float, Float), void Function(double, double, double)>(
          'vesper_set_viewfinder_zoom');
      _getLog = _lib.lookupFunction<Int32 Function(Pointer<Utf8>, Int32), int Function(Pointer<Utf8>, int)>('vesper_get_log');
      _clearLog = _lib.lookupFunction<Void Function(), void Function()>('vesper_clear_log');
      _logLine = _lib.lookupFunction<Void Function(Pointer<Utf8>, Pointer<Utf8>), void Function(Pointer<Utf8>, Pointer<Utf8>)>(
          'vesper_log_line');
      _setScopes = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_set_scopes');
      _getScopes = _lib
          .lookupFunction<Int32 Function(Pointer<Float>, Pointer<Float>), int Function(Pointer<Float>, Pointer<Float>)>(
            'vesper_get_scopes',
          );
      _pickWb = _lib.lookupFunction<Void Function(Float, Float), void Function(double, double)>(
        'vesper_pick_white_balance',
      );
      _focusPullTo = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_focus_pull_to');
      _setFocusSpeed = _lib.lookupFunction<Void Function(Float), void Function(double)>('vesper_set_focus_speed');
      _focusLock = _lib.lookupFunction<Void Function(), void Function()>('vesper_focus_lock');
      _setMetering = _lib.lookupFunction<Void Function(Int32, Float, Float), void Function(int, double, double)>(
        'vesper_set_metering',
      );
      _captureCalibration = _lib
          .lookupFunction<Void Function(Pointer<Utf8>, Pointer<Utf8>), void Function(Pointer<Utf8>, Pointer<Utf8>)>(
            'vesper_capture_calibration_frame',
          );
      _setColorProfile = _lib
          .lookupFunction<
            Void Function(Int32, Pointer<Float>, Pointer<Float>),
            void Function(int, Pointer<Float>, Pointer<Float>)
          >('vesper_set_color_profile');
      _setSensorProfile = _lib.lookupFunction<
        Void Function(Int32, Pointer<Float>, Pointer<Float>, Int32, Pointer<Int32>),
        void Function(int, Pointer<Float>, Pointer<Float>, int, Pointer<Int32>)
      >('vesper_set_sensor_profile');
      _setShotNoiseProfile = _lib.lookupFunction<
        Void Function(Int32, Pointer<Float>, Pointer<Float>),
        void Function(int, Pointer<Float>, Pointer<Float>)
      >('vesper_set_shot_noise_profile');
      _setRecordingQuality =
          _lib.lookupFunction<Void Function(Double), void Function(double)>('vesper_set_recording_quality');
      _useColorProfile = _lib.lookupFunction<Void Function(Int32), void Function(int)>('vesper_use_color_profile');
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
  (int, int)? autoExpose({bool keepShutter = true, bool clean = false}) {
    if (!_loaded) return null;
    final ns = calloc<Int64>();
    final iso = calloc<Int32>();
    try {
      return _autoExpose(clean ? 2 : (keepShutter ? 0 : 1), ns, iso) == 0 ? (ns.value, iso.value) : null;
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

  /// Takes left hidden by a crash or a killed app (see MainActivity):
  /// published at launch. Returns (file name, complete) per file.
  Future<List<(String, bool)>> recoverRecordings() async {
    if (!Platform.isAndroid) return const [];
    try {
      final list = await _channel.invokeListMethod<Map<Object?, Object?>>('recoverRecordings') ?? const [];
      return [for (final m in list) (m['name'] as String, m['complete'] as bool)];
    } on PlatformException {
      return const [];
    }
  }

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

  /// Hardware PDAF + laser AF at an upright viewfinder point. lock: one scan, then hold (AF-L);
  /// otherwise keep tracking that region (AF-C).
  void focusAt(double x, double y, {required bool lock}) => _loaded ? _focusAt(x, y, lock ? 1 : 0) : null;

  /// Native-ISO analysis: dark-frame sweep over the ISO range (lens covered).
  /// Progress / result come through [status]. Returns false if it couldn't start.
  bool startIsoSweep(String outPath, String deviceModel) {
    if (!_loaded) return false;
    final p = outPath.toNativeUtf8();
    final d = deviceModel.toNativeUtf8();
    try {
      return _isoSweepStart(p, d) == 0;
    } finally {
      calloc.free(p);
      calloc.free(d);
    }
  }

  void cancelIsoSweep() => _loaded ? _isoSweepCancel() : null;

  /// Sensor calibration sweep (tools/calibration/sensor.py): dark = lens
  /// covered, otherwise white (flat field). Writes `<basePath>.json` and a
  /// reference frame `<basePath>_ref.*`. Progress / result via [status].
  bool startSensorSweep({required bool dark, required String basePath, required String deviceModel}) {
    if (!_loaded) return false;
    final p = basePath.toNativeUtf8();
    final d = deviceModel.toNativeUtf8();
    try {
      return _sensorSweepStart(dark ? 0 : 1, p, d) == 0;
    } finally {
      calloc.free(p);
      calloc.free(d);
    }
  }

  void cancelSensorSweep() => _loaded ? _sensorSweepCancel() : null;

  /// Measured native ISOs for the clean auto-exposure (0 = unknown).
  void setNativeIsos(int baseIso, int hcgIso) => _loaded ? _setNativeIsos(baseIso, hcgIso) : null;

  /// GPU budget guard: auto-pause alignment / HQ / NR when frames would drop.
  void setBudgetGuard(bool on) => _loaded ? _setBudgetGuard(on ? 1 : 0) : null;

  /// GPU benchmark A/B: opt-in optimisation experiments (bit mask, see
  /// VulkanEngine::kExp*); 0 = the proven GPU path.
  void setExperiments(int mask) => _loaded ? _setExperiments(mask) : null;

  /// GPU benchmark: motion search every n-th frame (0 = automatic, the guard decides).
  void setAlignInterval(int n) => _loaded ? _setAlignInterval(n) : null;

  /// GPU benchmark pass-cost probe: run one pass (or part of one) an extra
  /// time per frame (-1 off, 0 unpack, 1 HQ, 2 alignment, 3 NR, 4 render,
  /// 5 alignment luma only, 6 alignment search only, 7 HQ load only,
  /// 8 HQ load + demosaic).
  void setRepeatPass(int pass) => _loaded ? _setRepeatPass(pass) : null;

  /// Asks for camera + microphone permission and waits for the answer.
  /// True only when both are granted (every recording has sound).
  Future<bool> requestPermissions() async {
    if (!Platform.isAndroid) return true;
    try {
      return await _channel.invokeMethod<bool>('requestPermissions') ?? false;
    } on PlatformException {
      return false;
    }
  }

  /// Locks the current landscape orientation (while recording) or frees both.
  /// Recording power saver: stop updating the viewfinder (the recording is unaffected).
  void setViewfinderPaused(bool paused) => _loaded ? _setViewfinderPaused(paused ? 1 : 0) : null;

  /// This window's screen brightness, 0..1; null = back to the system setting.
  Future<void> setScreenBrightness(double? value) async {
    if (!Platform.isAndroid) return;
    await _channel.invokeMethod('setScreenBrightness', {'value': value ?? -1.0});
  }

  Future<void> lockRotation(bool locked) async {
    if (!Platform.isAndroid) return;
    await _channel.invokeMethod('lockRotation', {'locked': locked});
  }

  /// Skip per-frame processing while a full-screen page covers the viewfinder
  /// (never while recording). The camera keeps streaming, so resuming is instant.
  void setProcessingPaused(bool paused) => _loaded ? _setProcessingPaused(paused ? 1 : 0) : null;

  /// HQ oversampling: luma from the full-resolution sensor, anti-alias downscaled.
  void setOversampling(bool on) => _loaded ? _setOversampling(on ? 1 : 0) : null;

  /// Detail enhancement: 0 off, 1 low, 2 medium, 3 high (recording and viewfinder).
  void setSharpening(int level) => _loaded ? _setSharpening(level) : null;

  /// Viewfinder magnifier, done in the GPU copy to the display: shows 1/scale
  /// of the frame around the normalised point (cx, cy). scale 1 = off.
  void setViewfinderZoom(double cx, double cy, double scale) => _loaded ? _setViewfinderZoom(cx, cy, scale) : null;

  /// In-app log since launch (native engine + lines added with [log]).
  String appLog() {
    if (!_loaded) return '';
    var size = 1 << 20;
    for (var attempt = 0; attempt < 2; ++attempt) {
      final buf = calloc<Uint8>(size).cast<Utf8>();
      try {
        final n = _getLog(buf, size);
        if (n >= 0) return buf.toDartString(length: n);
        size = -n + 4096; // grew meanwhile: retry with the size it asked for
      } finally {
        calloc.free(buf);
      }
    }
    return '';
  }

  void clearAppLog() => _loaded ? _clearLog() : null;

  /// Adds a line to the in-app log (and logcat).
  void log(String message, {String tag = 'Vesper_UI'}) {
    if (!_loaded) return;
    final t = tag.toNativeUtf8();
    final m = message.toNativeUtf8();
    try {
      _logLine(t, m);
    } finally {
      calloc.free(t);
      calloc.free(m);
    }
  }

  /// Where the native viewfinder surface sits on screen, in physical pixels
  /// (it is composited by the system underneath the transparent Flutter UI).
  Future<void> setViewfinderRect(int left, int top, int width, int height) async {
    if (!Platform.isAndroid) return;
    await _channel.invokeMethod('setViewfinderRect', {'left': left, 'top': top, 'width': width, 'height': height});
  }

  /// Histogram/waveform computation (off unless an overlay is shown).
  void setScopes(bool on) => _loaded ? _setScopes(on ? 1 : 0) : null;

  /// Latest scopes: histogram (64 bins, 0..1 of the tallest) and waveform
  /// (128 columns x 64 bins, column-major, fraction of the column's samples).
  Scopes? scopes() {
    if (!_loaded) return null;
    final h = calloc<Float>(Scopes.histBins);
    final w = calloc<Float>(Scopes.waveCols * Scopes.waveBins);
    try {
      if (_getScopes(h, w) != 0) return null;
      return Scopes(
        Float32List.fromList(h.asTypedList(Scopes.histBins)),
        Float32List.fromList(w.asTypedList(Scopes.waveCols * Scopes.waveBins)),
      );
    } finally {
      calloc.free(h);
      calloc.free(w);
    }
  }

  /// Eyedropper: sample white balance at an upright viewfinder point (0..1).
  /// Returns (kelvin, tint), or null if the patch is too dark or no frame arrived.
  Future<(double, double)?> pickWhiteBalance(double x, double y) async {
    if (!_loaded) return null;
    _pickWb(x, y);
    for (var i = 0; i < 20; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
      final k = calloc<Double>();
      final t = calloc<Double>();
      try {
        final r = _lockWb(k, t);
        if (r == 0) return (k.value, t.value);
        if (r != -1) return null; // sampled, but too dark to trust
      } finally {
        calloc.free(k);
        calloc.free(t);
      }
    }
    return null;
  }

  /// Smooth focus rack to a distance in diopters.
  void focusPullTo(double diopters) => _loaded ? _focusPullTo(diopters) : null;

  /// Seconds for a full-range focus move.
  void setFocusSpeed(double seconds) => _loaded ? _setFocusSpeed(seconds) : null;

  /// Holds focus where it is (AF-L).
  void focusLock() => _loaded ? _focusLock() : null;

  /// 0 = centre-weighted with face priority, 1 = spot at (x, y).
  void setMetering(int mode, [double x = 0.5, double y = 0.5]) => _loaded ? _setMetering(mode, x, y) : null;

  /// Saves the next raw frame to `<basePath>.raw10/.json/.dng` (metadata incl. noise profile, gain split, AWB).
  void captureCalibrationFrame(String basePath, String deviceModel) {
    if (!_loaded) return;
    final b = basePath.toNativeUtf8();
    final d = deviceModel.toNativeUtf8();
    try {
      _captureCalibration(b, d);
    } finally {
      calloc.free(b);
      calloc.free(d);
    }
  }

  /// Per-device forward matrices (9 floats row-major each) with their CCTs; empty clears.
  void setColorProfile(List<List<double>> matrices, List<double> kelvins) {
    if (!_loaded) return;
    final n = matrices.length.clamp(0, 2);
    final m = calloc<Float>(18);
    final k = calloc<Float>(2);
    try {
      for (var i = 0; i < n; i++) {
        for (var j = 0; j < 9; j++) {
          m[i * 9 + j] = matrices[i][j];
        }
        k[i] = kelvins[i];
      }
      _setColorProfile(n, m, k);
    } finally {
      calloc.free(m);
      calloc.free(k);
    }
  }

  /// Per-device sensor profile (tools/calibration/sensor.py --profile): dark-noise
  /// correction factors per ISO and the static hot-pixel map (array x, y pairs).
  void setSensorProfile(List<double> isos, List<double> noiseFactors, List<int> defectsXY) {
    if (!_loaded) return;
    final n = isos.length < noiseFactors.length ? isos.length : noiseFactors.length;
    final d = defectsXY.length ~/ 2;
    final pi = calloc<Float>(n + 1), pf = calloc<Float>(n + 1), pd = calloc<Int32>(d * 2 + 1);
    try {
      for (var i = 0; i < n; i++) {
        pi[i] = isos[i];
        pf[i] = noiseFactors[i];
      }
      for (var i = 0; i < d * 2; i++) {
        pd[i] = defectsXY[i];
      }
      _setSensorProfile(n, pi, pf, d, pd);
    } finally {
      calloc.free(pi);
      calloc.free(pf);
      calloc.free(pd);
    }
  }

  /// Shot-noise (S) correction factors per ISO of the sensor profile; empty clears.
  void setShotNoiseProfile(List<double> isos, List<double> factors) {
    if (!_loaded) return;
    final n = isos.length < factors.length ? isos.length : factors.length;
    final pi = calloc<Float>(n + 1), pf = calloc<Float>(n + 1);
    try {
      for (var i = 0; i < n; i++) {
        pi[i] = isos[i];
        pf[i] = factors[i];
      }
      _setShotNoiseProfile(n, pi, pf);
    } finally {
      calloc.free(pi);
      calloc.free(pf);
    }
  }

  /// Bitrate multiplier for the next take: 1 standard, 2 high, 3 max.
  void setRecordingQuality(double scale) => _loaded ? _setRecordingQuality(scale) : null;

  void useColorProfile(bool on) => _loaded ? _useColorProfile(on ? 1 : 0) : null;

  /// One line per hardware HEVC / AV1 encoder: bitrate modes, ranges, profiles, features.
  Future<List<String>> encoderInfo() async {
    if (!Platform.isAndroid) return const [];
    try {
      return (await _channel.invokeListMethod<String>('encoderInfo')) ?? const [];
    } on PlatformException {
      return const [];
    }
  }

  Future<Map<String, dynamic>?> deviceInfo() async {
    if (!Platform.isAndroid) return null;
    return _channel.invokeMapMethod<String, dynamic>('deviceInfo');
  }

  /// Copies the .raw10/.json pair into Downloads/Vesper Calibration.
  Future<bool> publishCalibration(String basePath) async {
    if (!Platform.isAndroid) return false;
    try {
      return await _channel.invokeMethod<bool>('publishCalibration', {'base': basePath}) ?? false;
    } on PlatformException {
      return false;
    }
  }

  void close() => _loaded ? _close() : null;
}
