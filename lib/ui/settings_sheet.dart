import 'package:flutter/material.dart';
import 'value_picker.dart';

/// Full-featured cinema settings page / bottom sheet.
/// Houses recording codec, resolution/crop selection, lens correction,
/// noise reduction, chart calibration, autofocus options, etc.
class SettingsSheet extends StatelessWidget {
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
  final bool profileAvailable;
  final bool useProfile;
  final String profileInfo;
  final ValueChanged<bool> onUseProfileChanged;
  final VoidCallback? onCaptureCalibration;
  final bool tapLocks;
  final ValueChanged<bool> onTapLocksChanged;
  final bool tapSetsExposure;
  final ValueChanged<bool> onTapSetsExposureChanged;
  final bool faceDetect;
  final ValueChanged<bool> onFaceDetectChanged;

  const SettingsSheet({
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
    required this.profileAvailable,
    required this.useProfile,
    required this.profileInfo,
    required this.onUseProfileChanged,
    this.onCaptureCalibration,
    required this.tapLocks,
    required this.onTapLocksChanged,
    required this.tapSetsExposure,
    required this.onTapSetsExposureChanged,
    required this.faceDetect,
    required this.onFaceDetectChanged,
  });

  static const _nrLevels = [0.0, 0.5, 0.7, 0.85];
  static const _chromaLevels = [0.0, 0.5, 1.0];

  @override
  Widget build(BuildContext context) {
    return SheetBody(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(20, 14, 20, 16),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                const Row(
                  children: [
                    Icon(Icons.tune, color: Colors.amber, size: 16),
                    SizedBox(width: 8),
                    Text(
                      'VESPER CINE · SYSTEM SETTINGS',
                      style: TextStyle(
                        color: Colors.white,
                        fontSize: 12,
                        fontWeight: FontWeight.bold,
                        letterSpacing: 1.5,
                      ),
                    ),
                  ],
                ),
                IconButton(
                  icon: const Icon(Icons.close, color: Colors.white60, size: 18),
                  padding: EdgeInsets.zero,
                  constraints: const BoxConstraints(),
                  onPressed: () => Navigator.of(context).pop(),
                ),
              ],
            ),
            const SizedBox(height: 12),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 12),

            // Section: RECORDING & FORMAT
            _sectionHeader('RECORDING & FORMAT'),
            _settingRow(
              'Recording Codec',
              '10-bit cinema broadcast master',
              Segmented(
                options: const ['HEVC 10-BIT', 'AV1 10-BIT'],
                selected: codec,
                onSelected: isRecording ? (_) {} : onCodecChanged,
              ),
            ),
            _settingRow(
              'Sensor Aspect / Crop',
              cropMode == 0 ? '16:9 Standard widescreen' : '4:3 Open Gate Full Sensor',
              Segmented(
                options: const ['16:9', '4:3 OPEN GATE'],
                selected: cropMode,
                onSelected: isRecording ? (_) {} : onCropModeChanged,
              ),
            ),

            const SizedBox(height: 12),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 12),

            // Section: FOCUS & METERING
            _sectionHeader('FOCUS & INTERACTION'),
            _settingRow(
              'Face Priority Detection',
              'Detect and prioritize faces in scene',
              Segmented(
                options: const ['OFF', 'ON'],
                selected: faceDetect ? 1 : 0,
                onSelected: (i) => onFaceDetectChanged(i == 1),
              ),
            ),
            _settingRow(
              'Viewfinder Tap Mode',
              tapLocks ? 'Focus & Lock (AF-L)' : 'Continuous Track (AF-C)',
              Segmented(
                options: const ['TRACK', 'FOCUS & LOCK'],
                selected: tapLocks ? 1 : 0,
                onSelected: (i) => onTapLocksChanged(i == 1),
              ),
            ),
            _settingRow(
              'Tap Spot Exposure',
              tapSetsExposure ? 'Tap meters spot exposure & focus' : 'Tap controls focus only',
              Segmented(
                options: const ['FOCUS ONLY', 'FOCUS + EXPOSURE'],
                selected: tapSetsExposure ? 1 : 0,
                onSelected: (i) => onTapSetsExposureChanged(i == 1),
              ),
            ),

            const SizedBox(height: 12),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 12),

            // Section: SENSOR & IMAGE PROCESSING
            _sectionHeader('SENSOR & IMAGE PROCESSING'),
            _settingRow(
              'Lens Distortion Correction',
              'Radial optical correction via camera HAL',
              Segmented(
                options: const ['OFF', 'ON'],
                selected: lensCorrection ? 1 : 0,
                onSelected: (i) => onLensCorrectionChanged(i == 1),
              ),
            ),
            _settingRow(
              'Hot / Dead Pixel Repair',
              'Auto-detect and fix defective pixels',
              Segmented(
                options: const ['OFF', 'ON'],
                selected: hotPixelFix ? 1 : 0,
                onSelected: (i) => onHotPixelFixChanged(i == 1),
              ),
            ),
            _settingRow(
              'Temporal Noise Reduction',
              'Multi-frame temporal RAW integration',
              Segmented(
                options: const ['OFF', 'LOW', 'MED', 'HIGH'],
                selected: _nrLevels.indexOf(temporalNr).clamp(0, 3),
                onSelected: (i) => onTemporalNrChanged(_nrLevels[i]),
              ),
            ),
            _settingRow(
              'Motion Alignment',
              'Tile alignment for moving camera',
              Segmented(
                options: const ['OFF', 'ON'],
                selected: nrAlignment ? 1 : 0,
                onSelected: (i) => onNrAlignmentChanged(i == 1),
              ),
            ),
            _settingRow(
              'Chroma Noise Reduction',
              'High-frequency chroma smoothing',
              Segmented(
                options: const ['OFF', 'LOW', 'HIGH'],
                selected: _chromaLevels.indexOf(chromaNr).clamp(0, 2),
                onSelected: (i) => onChromaNrChanged(_chromaLevels[i]),
              ),
            ),

            const SizedBox(height: 12),
            const Divider(color: Colors.white12, height: 1),
            const SizedBox(height: 12),

            // Section: COLOR SCIENCE & CALIBRATION
            _sectionHeader('COLOR SCIENCE & CALIBRATION'),
            _settingRow(
              'Calibration Profile',
              profileAvailable ? (useProfile ? 'Chart calibrated: $profileInfo' : 'Factory HAL profile') : 'No chart profile found',
              Segmented(
                options: const ['FACTORY', 'CHART PROFILE'],
                selected: (profileAvailable && useProfile) ? 1 : 0,
                onSelected: (i) => onUseProfileChanged(i == 1),
              ),
            ),
            if (onCaptureCalibration != null)
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
      style: const TextStyle(
        color: Colors.amber,
        fontSize: 10,
        fontWeight: FontWeight.bold,
        letterSpacing: 1.2,
      ),
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
                style: const TextStyle(
                  color: Colors.white,
                  fontSize: 12,
                  fontWeight: FontWeight.w600,
                ),
              ),
              const SizedBox(height: 2),
              Text(
                subtitle,
                style: const TextStyle(
                  color: Colors.white38,
                  fontSize: 10,
                ),
              ),
            ],
          ),
        ),
        control,
      ],
    ),
  );
}
