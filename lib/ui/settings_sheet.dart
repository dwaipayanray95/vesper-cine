import 'package:flutter/material.dart';
import '../build_flags.dart';
import '../services/vesper_native.dart';
import 'app_log_screen.dart';
import 'changelog.dart';
import 'value_picker.dart';

/// Full-screen settings, laid out like a camera menu: category tiles on the
/// left (each with a one-line summary of its current values), the selected
/// category's settings on the right, and an Info tab (version, changelog,
/// how-to, FAQ).
class SettingsScreen extends StatefulWidget {
  final int codec; // 0 = HEVC, 1 = AV1
  final ValueChanged<int> onCodecChanged;
  final int recordQuality; // 0 standard, 1 high, 2 max
  final double fps;
  final ValueChanged<int>? onRecordQualityChanged;
  final int cropMode; // 0 = 16:9, 1 = 4:3
  final ValueChanged<int> onCropModeChanged;
  final bool isRecording;
  final int powerSaver; // while recording: 0 off, 1 dim screen, 2 dim + viewfinder off
  final ValueChanged<int> onPowerSaverChanged;
  final bool lensCorrection;
  final ValueChanged<bool> onLensCorrectionChanged;
  final bool hotPixelFix;
  final ValueChanged<bool> onHotPixelFixChanged;
  final double temporalNr;
  final ValueChanged<double> onTemporalNrChanged;
  final bool nrAlignment;
  final ValueChanged<bool> onNrAlignmentChanged;
  final double chromaNr;
  final ValueChanged<double> onChromaNrChanged;
  final int sharpening;
  final bool oversampling;
  final bool gpuGuard;
  final bool calibratedNoise;
  final ValueChanged<bool>? onCalibratedNoiseChanged;
  final ValueChanged<bool> onGpuGuardChanged;
  final ValueChanged<bool> onOversamplingChanged;
  final ValueChanged<int> onSharpeningChanged;
  final bool profileAvailable;
  final bool useProfile;
  final String profileInfo;
  final ValueChanged<bool> onUseProfileChanged;
  final VoidCallback? onCaptureCalibration;
  final VoidCallback? onRunGpuBenchmark; // developer tool
  final VoidCallback? onRunGpuAbTest; // developer tool
  final bool tapLocks;
  final ValueChanged<bool> onTapLocksChanged;
  final bool tapSetsExposure;
  final ValueChanged<bool> onTapSetsExposureChanged;
  final bool faceDetect;
  final ValueChanged<bool> onFaceDetectChanged;
  final String isoAnalysisSummary; // '' = not analyzed yet
  final VoidCallback? onAnalyzeIso;
  final void Function(bool dark)? onSensorSweep; // Dark: Hot Pixel Calibration (all builds); White: Developer
  final String hotPixelSummary; // '' = this phone has no hot-pixel map yet

  const SettingsScreen({
    super.key,
    required this.codec,
    required this.onCodecChanged,
    this.recordQuality = 1,
    this.fps = 24,
    this.onRecordQualityChanged,
    required this.cropMode,
    required this.onCropModeChanged,
    required this.isRecording,
    this.powerSaver = 0,
    required this.onPowerSaverChanged,
    required this.lensCorrection,
    required this.onLensCorrectionChanged,
    required this.hotPixelFix,
    required this.onHotPixelFixChanged,
    required this.temporalNr,
    required this.onTemporalNrChanged,
    required this.nrAlignment,
    required this.onNrAlignmentChanged,
    required this.chromaNr,
    required this.onChromaNrChanged,
    this.sharpening = 1,
    this.oversampling = true,
    this.gpuGuard = true,
    this.calibratedNoise = false,
    this.onCalibratedNoiseChanged,
    required this.onGpuGuardChanged,
    required this.onOversamplingChanged,
    required this.onSharpeningChanged,
    required this.profileAvailable,
    required this.useProfile,
    required this.profileInfo,
    required this.onUseProfileChanged,
    this.onCaptureCalibration,
    this.onRunGpuBenchmark,
    this.onRunGpuAbTest,
    required this.tapLocks,
    required this.onTapLocksChanged,
    required this.tapSetsExposure,
    required this.onTapSetsExposureChanged,
    required this.faceDetect,
    required this.onFaceDetectChanged,
    this.isoAnalysisSummary = '',
    this.onAnalyzeIso,
    this.onSensorSweep,
    this.hotPixelSummary = '',
  });

  @override
  State<SettingsScreen> createState() => _SettingsScreenState();
}

// The page is a separate route, so the camera screen's setState doesn't rebuild
// it: keep local copies and update them alongside each callback.
class _SettingsScreenState extends State<SettingsScreen> {
  static const _nrLevels = [0.0, 0.5, 0.7, 0.85];
  static const _chromaLevels = [0.0, 0.5, 1.0];

  late int codec = widget.codec;
  late int recordQuality = widget.recordQuality;

  // Mirrors recorder.cpp: 1080p, 0.9 (HEVC) / 0.63 (AV1) bits per pixel per frame x quality.
  String get _bitrateLabel {
    final mbps = 1920 * 1080 * widget.fps * (codec == 1 ? 0.63 : 0.9) * (1 + recordQuality) / 1e6;
    return '~${mbps.clamp(0, 240).round()} Mb/s at ${widget.fps.round()} fps';
  }
  late int cropMode = widget.cropMode;
  late int powerSaver = widget.powerSaver;
  late bool lensCorrection = widget.lensCorrection;
  late bool hotPixelFix = widget.hotPixelFix;
  late double temporalNr = widget.temporalNr;
  late bool nrAlignment = widget.nrAlignment;
  late double chromaNr = widget.chromaNr;
  late int sharpening = widget.sharpening;
  late bool oversampling = widget.oversampling;
  late bool gpuGuard = widget.gpuGuard;
  late bool calibratedNoise = widget.calibratedNoise;
  late bool useProfile = widget.useProfile;
  late bool tapLocks = widget.tapLocks;
  late bool tapSetsExposure = widget.tapSetsExposure;
  late bool faceDetect = widget.faceDetect;

  bool get isRecording => widget.isRecording;
  bool get profileAvailable => widget.profileAvailable;
  String get profileInfo => widget.profileInfo;
  VoidCallback? get onCaptureCalibration => widget.onCaptureCalibration;

  // Last category shown, kept while the app runs (Settings reopens there).
  static int _lastCategory = 0;
  late int category = _lastCategory;
  Map<String, dynamic>? _deviceInfo;

  static const _levels4 = ['OFF', 'LOW', 'MED', 'HIGH'];

  @override
  void initState() {
    super.initState();
    VesperNative.instance.deviceInfo().then((m) {
      if (mounted) setState(() => _deviceInfo = m);
    });
  }

  String get _version {
    final v = _deviceInfo?['version'] as String?;
    final b = _deviceInfo?['build'] as String?;
    if (v == null || v.isEmpty) return '';
    return b == null || b.isEmpty ? v : '$v ($b)';
  }

  List<_Category> get _categories => [
    _Category('Recording', Icons.fiber_manual_record_outlined,
        '${codec == 0 ? 'HEVC' : 'AV1'} 10-BIT · ${cropMode == 0 ? '16:9' : '4:3 OG'}', _recording),
    _Category('Audio', Icons.mic_none, 'MIC ON', _audio),
    _Category('Image & NR', Icons.photo_outlined,
        'SHARP ${_levels4[sharpening.clamp(0, 3)]} · ${oversampling ? 'HQ' : 'HQ OFF'} · TNR ${_levels4[_nrLevels.indexOf(temporalNr).clamp(0, 3)]}',
        _image),
    _Category('Exposure · Color · Focus', Icons.tune,
        '${tapLocks ? 'AF-L' : 'AF-C'} · ${(profileAvailable && useProfile) ? 'CHART' : 'FACTORY'}'
            '${widget.isoAnalysisSummary.isEmpty ? '' : ' · ISO ★'}',
        _exposure),
    if (kDevTools) _Category('Developer', Icons.code, 'GUARD ${gpuGuard ? 'AUTO' : 'OFF'} · LOG', _developer),
    _Category('Info', Icons.info_outline, _version.isEmpty ? 'VERSION · HELP' : 'v$_version', _info),
  ];

  @override
  Widget build(BuildContext context) {
    final cats = _categories;
    final sel = category.clamp(0, cats.length - 1);
    return Scaffold(
      backgroundColor: const Color(0xFF0E1013),
      body: SafeArea(
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Container(
              width: 290,
              decoration: const BoxDecoration(
                color: Color(0xFF14171D),
                border: Border(right: BorderSide(color: Color(0xFF262B33))),
              ),
              child: ListView(
                padding: const EdgeInsets.fromLTRB(12, 10, 12, 12),
                children: [
                  Row(
                    children: [
                      const Padding(
                        padding: EdgeInsets.only(left: 4),
                        child: Text('SETTINGS',
                            style: TextStyle(color: Color(0xFF9AA3AE), fontSize: 12, fontWeight: FontWeight.w600, letterSpacing: 1.6)),
                      ),
                      const Spacer(),
                      IconButton(
                        tooltip: 'Close',
                        icon: const Icon(Icons.close, color: Colors.white),
                        onPressed: () => Navigator.of(context).pop(),
                      ),
                    ],
                  ),
                  for (var i = 0; i < cats.length; i++) _categoryTile(cats[i], i == sel, () {
                    setState(() => category = i);
                    _lastCategory = i;
                  }),
                ],
              ),
            ),
            Expanded(
              // Keyed by category: each one gets a fresh list that opens at the top
              // (one shared list kept the previous category's scroll position).
              child: ListView(
                key: ValueKey('settings-category-$sel'),
                padding: const EdgeInsets.fromLTRB(26, 18, 26, 24),
                children: [
                  Row(
                    crossAxisAlignment: CrossAxisAlignment.baseline,
                    textBaseline: TextBaseline.alphabetic,
                    children: [
                      Text(cats[sel].name,
                          style: const TextStyle(color: Colors.white, fontSize: 20, fontWeight: FontWeight.w600)),
                      const SizedBox(width: 12),
                      Flexible(
                        child: Text(cats[sel].summary,
                            overflow: TextOverflow.ellipsis,
                            style: const TextStyle(color: Colors.amber, fontSize: 11, fontFamily: 'monospace')),
                      ),
                    ],
                  ),
                  if (isRecording && sel == 0)
                    const Padding(
                      padding: EdgeInsets.only(top: 8),
                      child: Text('Recording: format settings are locked until the clip ends',
                          style: TextStyle(color: Colors.redAccent, fontSize: 11)),
                    ),
                  const SizedBox(height: 14),
                  ...cats[sel].build(),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _categoryTile(_Category c, bool selected, VoidCallback onTap) => Padding(
    padding: const EdgeInsets.only(bottom: 8),
    child: Material(
      color: selected ? const Color(0xFF1E2229) : Colors.transparent,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(10),
        side: BorderSide(color: selected ? Colors.amber : const Color(0xFF232831)),
      ),
      child: InkWell(
        borderRadius: BorderRadius.circular(10),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 9),
          child: Row(
            children: [
              Container(
                width: 36,
                height: 36,
                decoration: BoxDecoration(color: const Color(0xFF1F242C), borderRadius: BorderRadius.circular(8)),
                child: Icon(c.icon, size: 19, color: selected ? Colors.amber : const Color(0xFF9AA3AE)),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(c.name,
                        style: const TextStyle(color: Colors.white, fontSize: 14, fontWeight: FontWeight.w600)),
                    const SizedBox(height: 2),
                    Text(c.summary,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                            color: selected ? Colors.amber : const Color(0xFF7D8692), fontSize: 10.5, fontFamily: 'monospace')),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    ),
  );

  Widget _button(String label, VoidCallback? onPressed, {IconData? icon}) => OutlinedButton.icon(
    icon: Icon(icon ?? Icons.chevron_right, size: 14, color: Colors.amber),
    label: Text(label, style: const TextStyle(color: Colors.amber, fontSize: 11)),
    style: OutlinedButton.styleFrom(
      side: const BorderSide(color: Colors.amber),
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
    ),
    onPressed: onPressed,
  );

  // ---- Categories ---------------------------------------------------------

  List<Widget> _recording() => [
    _settingRow(
      'Recording Codec',
      '10-bit cinema master (Apple Log)',
      Segmented(
        options: const ['HEVC 10-BIT', 'AV1 10-BIT'],
        selected: codec,
        onSelected: isRecording
            ? (_) {}
            : (i) {
                setState(() => codec = i);
                widget.onCodecChanged(i);
              },
      ),
    ),
    _settingRow(
      'Recording Quality',
      '${['Standard: in-between frames lose ~25% of the fine detail', 'High (recommended): keeps ~90% of the detail between keyframes', 'Max: 3x bitrate (use HEVC: this phone\'s AV1 encoder is rated to 60 Mb/s)'][recordQuality]} · $_bitrateLabel',
      Segmented(
        options: const ['STANDARD', 'HIGH', 'MAX'],
        selected: recordQuality,
        onSelected: isRecording
            ? (_) {}
            : (i) {
                setState(() => recordQuality = i);
                widget.onRecordQualityChanged?.call(i);
              },
      ),
    ),
    _settingRow(
      'Sensor Aspect / Crop',
      cropMode == 0 ? '16:9 standard widescreen, 1920x1080' : '4:3 open gate, full sensor height',
      Segmented(
        options: const ['16:9', '4:3 OPEN GATE'],
        selected: cropMode,
        onSelected: isRecording
            ? (_) {}
            : (i) {
                if (i == cropMode) return;
                setState(() => cropMode = i);
                widget.onCropModeChanged(i);
              },
      ),
    ),
    _settingRow(
      'Power Saver While Recording',
      const [
        'Screen and viewfinder stay as they are',
        'After 10 s the screen dims to minimum; tap to brighten it for 10 s. Less heat on long takes',
        'After 10 s the screen dims and the viewfinder stops updating (less heat and GPU work); tap to view for 10 s',
      ][powerSaver.clamp(0, 2)],
      Segmented(
        options: const ['OFF', 'DIM SCREEN', 'VIEWFINDER OFF'],
        selected: powerSaver.clamp(0, 2),
        onSelected: (i) {
          setState(() => powerSaver = i);
          widget.onPowerSaverChanged(i);
        },
      ),
    ),
    _note('The recording itself is never affected: only what the screen shows.'),
  ];

  List<Widget> _audio() => [
    _settingRow('Microphone', 'Built-in microphone, recorded with every clip', const _Badge('ALWAYS ON')),
    _note('Audio levels and microphone choice are planned.'),
  ];

  List<Widget> _image() => [
    _sectionHeader('DETAIL'),
    _settingRow(
      'Detail / Sharpening',
      'Noise-aware edge enhancement (recording + viewfinder)',
      Segmented(
        options: _levels4,
        selected: sharpening.clamp(0, 3),
        onSelected: (i) {
          setState(() => sharpening = i);
          widget.onSharpeningChanged(i);
        },
      ),
    ),
    _settingRow(
      'HQ Oversampling',
      'Full-sensor detail, anti-aliased to the output: sharper, less moiré (more GPU)',
      Segmented(
        options: const ['OFF', 'ON'],
        selected: oversampling ? 1 : 0,
        onSelected: (i) {
          setState(() => oversampling = i == 1);
          widget.onOversamplingChanged(i == 1);
        },
      ),
    ),
    _settingRow(
      'Lens Distortion Correction',
      'Radial optical correction from the lens calibration',
      Segmented(
        options: const ['OFF', 'ON'],
        selected: lensCorrection ? 1 : 0,
        onSelected: (i) {
          setState(() => lensCorrection = i == 1);
          widget.onLensCorrectionChanged(i == 1);
        },
      ),
    ),
    _settingRow(
      'Hot / Dead Pixel Repair',
      'Detects and fixes defective photosites',
      Segmented(
        options: const ['OFF', 'ON'],
        selected: hotPixelFix ? 1 : 0,
        onSelected: (i) {
          setState(() => hotPixelFix = i == 1);
          widget.onHotPixelFixChanged(i == 1);
        },
      ),
    ),
    _settingRow(
      'Hot Pixel Calibration',
      widget.hotPixelSummary.isEmpty
          ? 'Not done yet: maps this phone\'s own hot pixels so they are always removed (lens covered, ~1 min)'
          : '${widget.hotPixelSummary}. Run again now and then: new ones are added',
      _button(widget.hotPixelSummary.isEmpty ? 'CALIBRATE' : 'RE-RUN',
          widget.onSensorSweep == null ? null : () => widget.onSensorSweep!(true), icon: Icons.dark_mode),
    ),
    const SizedBox(height: 10),
    _sectionHeader('NOISE REDUCTION'),
    _settingRow(
      'Temporal Noise Reduction',
      'Averages noise over frames; moving areas are left alone',
      Segmented(
        options: _levels4,
        selected: _nrLevels.indexOf(temporalNr).clamp(0, 3),
        onSelected: (i) {
          setState(() => temporalNr = _nrLevels[i]);
          widget.onTemporalNrChanged(_nrLevels[i]);
        },
      ),
    ),
    _settingRow(
      'Motion Alignment',
      'Aligns frames for handheld shots and pans before averaging',
      Segmented(
        options: const ['OFF', 'ON'],
        selected: nrAlignment ? 1 : 0,
        onSelected: (i) {
          setState(() => nrAlignment = i == 1);
          widget.onNrAlignmentChanged(i == 1);
        },
      ),
    ),
    _settingRow(
      'Chroma Noise Reduction',
      'Smooths colour noise, keeps luma detail',
      Segmented(
        options: const ['OFF', 'LOW', 'HIGH'],
        selected: _chromaLevels.indexOf(chromaNr).clamp(0, 2),
        onSelected: (i) {
          setState(() => chromaNr = _chromaLevels[i]);
          widget.onChromaNrChanged(_chromaLevels[i]);
        },
      ),
    ),
    _settingRow(
      'NR Character',
      calibratedNoise
          ? 'SMOOTH: also treats the measured midtone grain as noise. Cleaner low-light footage, slightly softer fine texture'
          : 'TEXTURE: keeps the most fine detail. Shadows and high ISO are always cleaned with the measured sensor noise',
      Segmented(
        options: const ['TEXTURE', 'SMOOTH'],
        selected: calibratedNoise ? 1 : 0,
        onSelected: (i) {
          setState(() => calibratedNoise = i == 1);
          widget.onCalibratedNoiseChanged?.call(i == 1);
        },
      ),
    ),
  ];

  List<Widget> _exposure() => [
    _sectionHeader('FOCUS'),
    _settingRow(
      'Viewfinder Tap Mode',
      tapLocks ? 'Focus & lock (AF-L)' : 'Continuous tracking (AF-C)',
      Segmented(
        options: const ['TRACK', 'FOCUS & LOCK'],
        selected: tapLocks ? 1 : 0,
        onSelected: (i) {
          setState(() => tapLocks = i == 1);
          widget.onTapLocksChanged(i == 1);
        },
      ),
    ),
    _settingRow(
      'Face Priority Detection',
      'Detect and prioritize faces in the scene',
      Segmented(
        options: const ['OFF', 'ON'],
        selected: faceDetect ? 1 : 0,
        onSelected: (i) {
          setState(() => faceDetect = i == 1);
          widget.onFaceDetectChanged(i == 1);
        },
      ),
    ),
    const SizedBox(height: 10),
    _sectionHeader('EXPOSURE'),
    _settingRow(
      'Tap Spot Exposure',
      tapSetsExposure ? 'A tap meters exposure at that spot too' : 'A tap only sets focus',
      Segmented(
        options: const ['FOCUS ONLY', 'FOCUS + EXPOSURE'],
        selected: tapSetsExposure ? 1 : 0,
        onSelected: (i) {
          setState(() => tapSetsExposure = i == 1);
          widget.onTapSetsExposureChanged(i == 1);
        },
      ),
    ),
    _settingRow(
      'Native ISO Analysis',
      widget.isoAnalysisSummary.isEmpty
          ? 'Measures the sensor\'s native ISOs (lens covered, ~15 s); auto-exposure then prefers them'
          : widget.isoAnalysisSummary,
      _button(widget.isoAnalysisSummary.isEmpty ? 'ANALYZE' : 'RE-RUN', widget.onAnalyzeIso, icon: Icons.grain),
    ),
    if (kDevTools || profileAvailable) ...[
      const SizedBox(height: 10),
      _sectionHeader('COLOR'),
      _settingRow(
        'Calibration Profile',
        profileAvailable
            ? (useProfile ? 'Chart calibrated: $profileInfo' : 'Factory profile from the camera')
            : 'No chart profile found',
        Segmented(
          options: const ['FACTORY', 'CHART PROFILE'],
          selected: (profileAvailable && useProfile) ? 1 : 0,
          onSelected: (i) {
            if (profileAvailable) setState(() => useProfile = i == 1);
            widget.onUseProfileChanged(i == 1);
          },
        ),
      ),
    ],
  ];

  List<Widget> _developer() => [
    _settingRow(
      'GPU Performance Guard',
      gpuGuard
          ? 'AUTO: pauses alignment / HQ / NR when frames would drop, brings them back when they fit'
          : 'OFF: processing always runs (frames may drop). Release builds are always AUTO; recording forces it on',
      Segmented(
        options: const ['AUTO', 'OFF'],
        selected: gpuGuard ? 0 : 1,
        onSelected: (i) {
          setState(() => gpuGuard = i == 0);
          widget.onGpuGuardChanged(i == 0);
        },
      ),
    ),
    _settingRow(
      'App Log',
      'Engine log since launch: GPU, guard, camera, recorder. Copy it to share',
      _button('OPEN', () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const AppLogScreen())),
          icon: Icons.article_outlined),
    ),
    _settingRow(
      'GPU Benchmark',
      'Measures each processing option\'s GPU cost on this phone (~70 s)',
      _button('RUN', widget.onRunGpuBenchmark, icon: Icons.speed),
    ),
    _settingRow(
      'GPU A/B Test',
      'Each speed-up experiment against the normal path, in alternating blocks so heat cancels out (~35 s, best at 60 fps)',
      _button('RUN', widget.onRunGpuAbTest, icon: Icons.compare_arrows),
    ),
    if (onCaptureCalibration != null)
      _settingRow(
        'Calibration Frame',
        'Saves the next raw frame as DNG + raw10 + JSON (noise profile, gain, Google AWB, shading map) '
            'to Download/Vesper Calibration',
        _button('CAPTURE', onCaptureCalibration, icon: Icons.camera),
      ),
    _settingRow(
      'Sensor Calibration · Dark',
      'Same as Image & NR › Hot Pixel Calibration; in developer builds the statistics also go to '
          'Download/Vesper Calibration (tools/calibration/sensor.py)',
      _button('RUN', widget.onSensorSweep == null ? null : () => widget.onSensorSweep!(true), icon: Icons.dark_mode),
    ),
    _settingRow(
      'Sensor Calibration · White',
      'White paper over the lens, aimed at daylight: noise, lens shading, clip point and linearity (~1-2 min)',
      _button('RUN', widget.onSensorSweep == null ? null : () => widget.onSensorSweep!(false), icon: Icons.light_mode),
    ),
  ];

  List<Widget> _info() {
    final model = _deviceInfo?['model'] as String? ?? '';
    final android = _deviceInfo?['android'] as String? ?? '';
    return [
      _sectionHeader('ABOUT'),
      _infoLine('Version', _version.isEmpty ? '—' : _version),
      _infoLine('Build', kDevTools ? 'Developer (debug / profile)' : 'Release'),
      if (model.isNotEmpty) _infoLine('Device', android.isEmpty ? model : '$model · Android $android'),
      _infoLine('Recordings', 'Movies/Vesper Cine'),
      const SizedBox(height: 6),
      _note('Vesper Cine records straight from the sensor\'s RAW data: its own colour science, '
          'true Apple Log and noise processing on the GPU, encoded as 10-bit HEVC or AV1.'),
      const SizedBox(height: 14),
      _sectionHeader('WHAT\'S NEW'),
      for (final e in changelog.take(6)) _changelogEntry(e),
      if (changelog.length > 6)
        Align(
          alignment: Alignment.centerLeft,
          child: TextButton(
            onPressed: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const _FullChangelog())),
            child: const Text('Full changelog', style: TextStyle(color: Colors.amber, fontSize: 12)),
          ),
        ),
      const SizedBox(height: 14),
      _sectionHeader('HOW TO'),
      ..._howTo.map((h) => _qa(h.$1, h.$2)),
      const SizedBox(height: 14),
      _sectionHeader('FAQ'),
      ..._faq.map((h) => _qa(h.$1, h.$2)),
      const SizedBox(height: 14),
      _sectionHeader('REPORTING A PROBLEM'),
      _note(kDevTools
          ? 'Settings › Developer › App Log › Copy, and send the log with what you did and what you saw.'
          : 'Describe what you did and what you saw, with the version number above.'),
    ];
  }

  static const _howTo = <(String, String)>[
    ('Focus', 'Tap the viewfinder to focus there. Long-press to focus and lock. TRACK or FOCUS & LOCK is set in Exposure · Color · Focus.'),
    ('Exposure', 'With "Focus + exposure" on, a tap also meters at that spot. ISO and shutter tiles set them manually.'),
    ('Monitoring', 'MAG magnifies 3x around the focus point, PEAK highlights what is in focus, ZEBRA marks clipping, FC shows false colour. SCOPE shows a histogram or waveform.'),
    ('White balance', 'Open the WB tile. The eyedropper sets white balance from a neutral spot you tap.'),
    ('Look', 'Apple Log is flat on purpose, for grading. The REC.709 LUT button only changes the viewfinder; the recording stays log.'),
    ('Hot pixels', 'Image & NR › Hot Pixel Calibration: cover the lens, run it once (about a minute). The phone\'s own hot pixels are then always removed.'),
    ('Native ISO', 'Exposure · Color · Focus › Native ISO Analysis: cover the lens, run it once. Auto-exposure then prefers the cleanest ISOs, marked ★ on the ISO dial.'),
  ];

  static const _faq = <(String, String)>[
    ('Why do HQ / NR / ALIGN show PAUSED?', 'The GPU guard keeps the frame rate smooth: at higher frame rates there is less time per frame, so it pauses the most expensive processing. It turns it back on as soon as it fits; at 24 fps everything runs.'),
    ('Why is the image noisy in the dark?', 'Little light means high ISO. A slower shutter helps most: at 24 fps, 1/48 (180°) collects 2.5 stops more light than 1/271.'),
    ('"FPS NOT SUSTAINABLE"?', 'Even with optional processing paused, this frame rate is too much for the phone. Lower the frame rate or turn off HQ / noise reduction.'),
    ('Why does the phone get warm?', 'All image processing runs on the GPU from RAW. Long takes at high frame rates warm the phone; recording stops safely if it gets too hot.'),
  ];

  Widget _infoLine(String k, String v) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 4),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(width: 110, child: Text(k, style: const TextStyle(color: Color(0xFF9AA3AE), fontSize: 12))),
        Expanded(child: Text(v, style: const TextStyle(color: Colors.white, fontSize: 12))),
      ],
    ),
  );

  Widget _note(String text) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 4),
    child: Text(text, style: const TextStyle(color: Color(0xFF9AA3AE), fontSize: 11.5, height: 1.4)),
  );

  Widget _qa(String q, String a) => Padding(
    padding: const EdgeInsets.only(bottom: 10),
    child: Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(q, style: const TextStyle(color: Colors.white, fontSize: 12.5, fontWeight: FontWeight.w600)),
        const SizedBox(height: 3),
        Text(a, style: const TextStyle(color: Color(0xFF9AA3AE), fontSize: 11.5, height: 1.4)),
      ],
    ),
  );

  Widget _changelogEntry(ChangelogEntry e) => Padding(
    padding: const EdgeInsets.only(bottom: 8),
    child: Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 64,
          child: Text('v${e.version}', style: const TextStyle(color: Colors.amber, fontSize: 11.5, fontFamily: 'monospace')),
        ),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              for (final n in e.notes)
                Text('• $n', style: const TextStyle(color: Colors.white70, fontSize: 11.5, height: 1.4)),
            ],
          ),
        ),
      ],
    ),
  );

  Widget _sectionHeader(String title) => Padding(
    padding: const EdgeInsets.only(bottom: 8),
    child: Text(
      title,
      style: const TextStyle(color: Colors.amber, fontSize: 10, fontWeight: FontWeight.bold, letterSpacing: 1.2),
    ),
  );

  Widget _settingRow(String title, String subtitle, Widget control) => Container(
    margin: const EdgeInsets.only(bottom: 8),
    padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 11),
    decoration: BoxDecoration(
      color: const Color(0xFF171B21),
      border: Border.all(color: const Color(0xFF232831)),
      borderRadius: BorderRadius.circular(10),
    ),
    child: Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                title,
                style: const TextStyle(color: Colors.white, fontSize: 13.5, fontWeight: FontWeight.w500),
              ),
              const SizedBox(height: 2),
              Text(subtitle, style: const TextStyle(color: Color(0xFF9AA3AE), fontSize: 11)),
            ],
          ),
        ),
        const SizedBox(width: 12),
        control,
      ],
    ),
  );
}

class _Category {
  final String name;
  final IconData icon;
  final String summary;
  final List<Widget> Function() build;
  const _Category(this.name, this.icon, this.summary, this.build);
}

class _Badge extends StatelessWidget {
  final String text;
  const _Badge(this.text);

  @override
  Widget build(BuildContext context) => Container(
    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
    decoration: BoxDecoration(
      color: Colors.amber.withValues(alpha: 0.15),
      borderRadius: BorderRadius.circular(4),
      border: Border.all(color: Colors.amber),
    ),
    child: Text(text, style: const TextStyle(color: Colors.amber, fontSize: 11, fontWeight: FontWeight.w600)),
  );
}

class _FullChangelog extends StatelessWidget {
  const _FullChangelog();

  @override
  Widget build(BuildContext context) => Scaffold(
    backgroundColor: const Color(0xFF0E1013),
    appBar: AppBar(
      backgroundColor: const Color(0xFF14171D),
      foregroundColor: Colors.white,
      title: const Text('Changelog', style: TextStyle(fontSize: 16)),
    ),
    body: ListView(
      padding: const EdgeInsets.all(20),
      children: [
        for (final e in changelog)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('v${e.version}', style: const TextStyle(color: Colors.amber, fontSize: 13, fontFamily: 'monospace')),
                const SizedBox(height: 4),
                for (final n in e.notes) Text('• $n', style: const TextStyle(color: Colors.white70, fontSize: 12.5, height: 1.4)),
              ],
            ),
          ),
      ],
    ),
  );
}
