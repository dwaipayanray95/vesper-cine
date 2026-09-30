import 'package:flutter/material.dart';
import '../build_flags.dart';
import 'app_log_screen.dart';
import 'value_picker.dart';

/// Full-screen cinema settings page.
/// Houses recording codec, resolution/crop selection, lens correction,
/// noise reduction, chart calibration, autofocus options, etc.
class SettingsScreen extends StatefulWidget {
  final int codec; // 0 = HEVC, 1 = AV1
  final ValueChanged<int> onCodecChanged;
  final int cropMode; // 0 = 16:9, 1 = 4:3
  final ValueChanged<int> onCropModeChanged;
  final bool isRecording;
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
  final ValueChanged<bool> onGpuGuardChanged;
  final ValueChanged<bool> onOversamplingChanged;
  final ValueChanged<int> onSharpeningChanged;
  final bool profileAvailable;
  final bool useProfile;
  final String profileInfo;
  final ValueChanged<bool> onUseProfileChanged;
  final VoidCallback? onCaptureCalibration;
  final VoidCallback? onRunGpuBenchmark; // developer tool
  final bool tapLocks;
  final ValueChanged<bool> onTapLocksChanged;
  final bool tapSetsExposure;
  final ValueChanged<bool> onTapSetsExposureChanged;
  final bool faceDetect;
  final ValueChanged<bool> onFaceDetectChanged;
  final String isoAnalysisSummary; // '' = not analyzed yet
  final VoidCallback? onAnalyzeIso;

  const SettingsScreen({
    super.key,
    required this.codec,
    required this.onCodecChanged,
    required this.cropMode,
    required this.onCropModeChanged,
    required this.isRecording,
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
    required this.onGpuGuardChanged,
    required this.onOversamplingChanged,
    required this.onSharpeningChanged,
    required this.profileAvailable,
    required this.useProfile,
    required this.profileInfo,
    required this.onUseProfileChanged,
    this.onCaptureCalibration,
    this.onRunGpuBenchmark,
    required this.tapLocks,
    required this.onTapLocksChanged,
    required this.tapSetsExposure,
    required this.onTapSetsExposureChanged,
    required this.faceDetect,
    required this.onFaceDetectChanged,
    this.isoAnalysisSummary = '',
    this.onAnalyzeIso,
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
  late int cropMode = widget.cropMode;
  late bool lensCorrection = widget.lensCorrection;
  late bool hotPixelFix = widget.hotPixelFix;
  late double temporalNr = widget.temporalNr;
  late bool nrAlignment = widget.nrAlignment;
  late double chromaNr = widget.chromaNr;
  late int sharpening = widget.sharpening;
  late bool oversampling = widget.oversampling;
  late bool gpuGuard = widget.gpuGuard;
  late bool useProfile = widget.useProfile;
  late bool tapLocks = widget.tapLocks;
  late bool tapSetsExposure = widget.tapSetsExposure;
  late bool faceDetect = widget.faceDetect;

  bool get isRecording => widget.isRecording;
  bool get profileAvailable => widget.profileAvailable;
  String get profileInfo => widget.profileInfo;
  VoidCallback? get onCaptureCalibration => widget.onCaptureCalibration;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF0C0E11),
      appBar: AppBar(
        backgroundColor: const Color(0xFF14171D),
        elevation: 0,
        leading: IconButton(
          icon: const Icon(Icons.arrow_back, color: Colors.white),
          onPressed: () => Navigator.of(context).pop(),
        ),
        title: const Row(
          children: [
            Icon(Icons.tune, color: Colors.amber, size: 18),
            SizedBox(width: 10),
            Text(
              'VESPER CINE · SYSTEM SETTINGS',
              style: TextStyle(color: Colors.white, fontSize: 13, fontWeight: FontWeight.bold, letterSpacing: 1.5),
            ),
          ],
        ),
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 16),
          children: [
            // Section: RECORDING & FORMAT
            _sectionHeader('RECORDING & FORMAT'),
            _settingRow(
              'Recording Codec',
              '10-bit cinema broadcast master',
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
              'Sensor Aspect / Crop',
              cropMode == 0 ? '16:9 Standard widescreen' : '4:3 Open Gate Full Sensor',
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

            const SizedBox(height: 14),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 14),

            // Section: FOCUS & METERING
            _sectionHeader('FOCUS & INTERACTION'),
            _settingRow(
              'Face Priority Detection',
              'Detect and prioritize faces in scene',
              Segmented(
                options: const ['OFF', 'ON'],
                selected: faceDetect ? 1 : 0,
                onSelected: (i) {
                  setState(() => faceDetect = i == 1);
                  widget.onFaceDetectChanged(i == 1);
                },
              ),
            ),
            _settingRow(
              'Viewfinder Tap Mode',
              tapLocks ? 'Focus & Lock (AF-L)' : 'Continuous Track (AF-C)',
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
              'Tap Spot Exposure',
              tapSetsExposure ? 'Tap meters spot exposure & focus' : 'Tap controls focus only',
              Segmented(
                options: const ['FOCUS ONLY', 'FOCUS + EXPOSURE'],
                selected: tapSetsExposure ? 1 : 0,
                onSelected: (i) {
                  setState(() => tapSetsExposure = i == 1);
                  widget.onTapSetsExposureChanged(i == 1);
                },
              ),
            ),

            const SizedBox(height: 14),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 14),

            // Section: SENSOR & IMAGE PROCESSING
            _sectionHeader('SENSOR & IMAGE PROCESSING'),
            _settingRow(
              'Lens Distortion Correction',
              'Radial optical correction via camera HAL',
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
              'Auto-detect and fix defective pixels',
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
              'Temporal Noise Reduction',
              'Multi-frame temporal RAW integration',
              Segmented(
                options: const ['OFF', 'LOW', 'MED', 'HIGH'],
                selected: _nrLevels.indexOf(temporalNr).clamp(0, 3),
                onSelected: (i) {
                  setState(() => temporalNr = _nrLevels[i]);
                  widget.onTemporalNrChanged(_nrLevels[i]);
                },
              ),
            ),
            _settingRow(
              'Motion Alignment',
              'Tile alignment for moving camera',
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
              'HQ Oversampling',
              'Full-sensor luma, anti-alias downscaled: sharper, less moire (more GPU)',
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
              'GPU Performance Guard',
              gpuGuard
                  ? 'AUTO: briefly pauses alignment / HQ / NR if frames would drop, resumes when there is headroom'
                  : 'OFF: your processing choices always run (frames may drop if the GPU can\'t keep up)',
              Segmented(
                options: const ['AUTO', 'OFF'],
                selected: gpuGuard ? 0 : 1,
                onSelected: (i) {
                  setState(() => gpuGuard = i == 0);
                  widget.onGpuGuardChanged(i == 0);
                },
              ),
            ),
            if (kDevTools)
              _settingRow(
                'GPU Benchmark (dev)',
                'Measures each processing option\'s GPU cost on this phone (~25 s)',
                OutlinedButton(
                  style: OutlinedButton.styleFrom(
                    side: const BorderSide(color: Colors.amber),
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  ),
                  onPressed: widget.onRunGpuBenchmark,
                  child: const Text('RUN', style: TextStyle(color: Colors.amber, fontSize: 11)),
                ),
              ),
            if (kDevTools)
              _settingRow(
                'App log (dev)',
                'Engine log since launch: GPU, guard, camera, recorder. Copy it to share',
                OutlinedButton(
                  style: OutlinedButton.styleFrom(
                    side: const BorderSide(color: Colors.amber),
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  ),
                  onPressed: () => Navigator.of(context).push(MaterialPageRoute(builder: (_) => const AppLogScreen())),
                  child: const Text('OPEN', style: TextStyle(color: Colors.amber, fontSize: 11)),
                ),
              ),
            _settingRow(
              'Detail / Sharpening',
              'Noise-aware edge enhancement (recording + viewfinder)',
              Segmented(
                options: const ['OFF', 'LOW', 'MED', 'HIGH'],
                selected: sharpening.clamp(0, 3),
                onSelected: (i) {
                  setState(() => sharpening = i);
                  widget.onSharpeningChanged(i);
                },
              ),
            ),
            _settingRow(
              'Chroma Noise Reduction',
              'High-frequency chroma smoothing',
              Segmented(
                options: const ['OFF', 'LOW', 'HIGH'],
                selected: _chromaLevels.indexOf(chromaNr).clamp(0, 2),
                onSelected: (i) {
                  setState(() => chromaNr = _chromaLevels[i]);
                  widget.onChromaNrChanged(_chromaLevels[i]);
                },
              ),
            ),

            const SizedBox(height: 14),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 14),

            // Section: SENSOR
            _sectionHeader('SENSOR'),
            _settingRow(
              'Native ISO Analysis',
              widget.isoAnalysisSummary.isEmpty
                  ? 'Measure the sensor\'s native ISOs (lens covered, ~15 s)'
                  : widget.isoAnalysisSummary,
              OutlinedButton.icon(
                icon: const Icon(Icons.grain, size: 14, color: Colors.amber),
                label: Text(
                  widget.isoAnalysisSummary.isEmpty ? 'ANALYZE' : 'RE-RUN',
                  style: const TextStyle(color: Colors.amber, fontSize: 11),
                ),
                style: OutlinedButton.styleFrom(
                  side: const BorderSide(color: Colors.amber),
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                ),
                onPressed: widget.onAnalyzeIso,
              ),
            ),

            const SizedBox(height: 14),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 14),

            // Section: COLOR SCIENCE & CALIBRATION (chart tools: developer builds only)
            if (kDevTools || profileAvailable) _sectionHeader('COLOR SCIENCE & CALIBRATION'),
            if (kDevTools || profileAvailable)
              _settingRow(
                'Calibration Profile',
                profileAvailable
                    ? (useProfile ? 'Chart calibrated: $profileInfo' : 'Factory HAL profile')
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
            if (kDevTools && onCaptureCalibration != null)
              _settingRow(
                'Calibration Frame',
                'Capture raw frame for calibration tool',
                OutlinedButton.icon(
                  icon: const Icon(Icons.camera, size: 14, color: Colors.amber),
                  label: const Text('CAPTURE', style: TextStyle(color: Colors.amber, fontSize: 11)),
                  style: OutlinedButton.styleFrom(
                    side: const BorderSide(color: Colors.amber),
                    padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  ),
                  onPressed: onCaptureCalibration,
                ),
              ),
          ],
        ),
      ),
    );
  }

  Widget _sectionHeader(String title) => Padding(
    padding: const EdgeInsets.only(bottom: 8),
    child: Text(
      title,
      style: const TextStyle(color: Colors.amber, fontSize: 10, fontWeight: FontWeight.bold, letterSpacing: 1.2),
    ),
  );

  Widget _settingRow(String title, String subtitle, Widget control) => Padding(
    padding: const EdgeInsets.symmetric(vertical: 6),
    child: Row(
      mainAxisAlignment: MainAxisAlignment.spaceBetween,
      children: [
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                title,
                style: const TextStyle(color: Colors.white, fontSize: 12, fontWeight: FontWeight.w600),
              ),
              const SizedBox(height: 2),
              Text(subtitle, style: const TextStyle(color: Colors.white38, fontSize: 10)),
            ],
          ),
        ),
        control,
      ],
    ),
  );
}
