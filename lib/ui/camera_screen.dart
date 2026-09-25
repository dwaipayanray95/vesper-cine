import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../services/rcamera_native.dart';

class CameraScreen extends StatefulWidget {
  const CameraScreen({super.key});

  @override
  State<CameraScreen> createState() => _CameraScreenState();
}

class _CameraScreenState extends State<CameraScreen> with SingleTickerProviderStateMixin {
  final RCameraNative _camera = RCameraNative.instance;

  // Settings State
  bool _isStreaming = false;
  bool _isRecording = false;
  int _monitoringMode = 0; // 0: Flat R-Log, 1: Rec.709 LUT, 2: False Color, 3: Focus Peaking, 4: Zebras
  int _cropMode = 0;       // 0: 16:9 4K UHD, 1: 4:3 Open Gate
  double _shutterAngle = 180.0;
  double _fps = 24.0;
  int _iso = 100;
  int _kelvin = 5600;
  int _tint = 0;
  bool _oisEnabled = true;
  bool _gyroLogging = true;

  // Recording Timer
  late AnimationController _pulseController;
  int _recordedSeconds = 0;
  int? _textureId;

  @override
  void initState() {
    super.initState();
    SystemChrome.setEnabledSystemUIMode(SystemUiMode.immersiveSticky);
    SystemChrome.setPreferredOrientations([
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ]);

    _pulseController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1000),
    )..repeat(reverse: true);

    _initCameraPipeline();
  }

  @override
  void dispose() {
    _pulseController.dispose();
    _camera.destroyViewfinderTexture();
    _camera.close();
    super.dispose();
  }

  String _statusMessage = "INITIALIZING SENSOR...";

  Future<void> _initCameraPipeline() async {
    final ok = _camera.initialize();
    if (!ok) {
      setState(() {
        _statusMessage = "RUNNING ON DESKTOP TEST HARNESS";
      });
      return;
    }

    final cameras = _camera.enumerateCameras();
    if (cameras.isEmpty) {
      setState(() {
        _statusMessage = "NO CAMERA DEVICES DETECTED";
      });
      return;
    }

    final rawCam = cameras.where((c) => c.supportsRaw10).firstOrNull;
    if (rawCam != null) {
      _camera.openCamera(rawCam.id);
      _camera.setShutterAngle(shutterAngle: _shutterAngle, fps: _fps, iso: _iso);
      _camera.setKelvinTint(kelvin: _kelvin, tint: _tint);
      _camera.setOis(_oisEnabled);
      _camera.setCropMode(_cropMode);
      _camera.setMonitoringMode(_monitoringMode);

      final textureId = await _camera.createViewfinderTexture(width: 1920, height: 1080);
      final streamOk = _camera.startStream(rawCam.rawWidth, rawCam.rawHeight);
      setState(() {
        _textureId = textureId;
        _isStreaming = streamOk;
        _statusMessage = "PIXEL SENSOR ACTIVE (RAW10 ${rawCam.rawWidth}x${rawCam.rawHeight})";
      });
    } else {
      final fallback = cameras.first;
      _camera.openCamera(fallback.id);
      setState(() {
        _isStreaming = false;
        _statusMessage = "CAMERA DETECTED (ID: ${fallback.id}) - PHYSICAL PIXEL REQUIRED FOR RAW10";
      });
    }
  }

  void _toggleRecording() {
    setState(() {
      _isRecording = !_isRecording;
      if (_isRecording) {
        _recordedSeconds = 0;
      }
    });
  }

  void _cycleMonitoringMode() {
    setState(() {
      _monitoringMode = (_monitoringMode + 1) % 5;
      _camera.setMonitoringMode(_monitoringMode);
    });
  }

  void _toggleCropMode() {
    setState(() {
      _cropMode = (_cropMode == 0) ? 1 : 0;
      _camera.setCropMode(_cropMode);
    });
  }

  void _cycleShutterAngle() {
    final angles = [90.0, 180.0, 270.0, 360.0];
    final idx = angles.indexOf(_shutterAngle);
    final nextAngle = angles[(idx + 1) % angles.length];
    setState(() {
      _shutterAngle = nextAngle;
      _camera.setShutterAngle(shutterAngle: _shutterAngle, fps: _fps, iso: _iso);
    });
  }

  void _cycleIso() {
    final isos = [50, 100, 200, 400, 800, 1600, 3200];
    final idx = isos.indexOf(_iso);
    final nextIso = isos[(idx + 1) % isos.length];
    setState(() {
      _iso = nextIso;
      _camera.setShutterAngle(shutterAngle: _shutterAngle, fps: _fps, iso: _iso);
    });
  }

  void _cycleFps() {
    final fpsList = [24.0, 25.0, 30.0, 60.0];
    final idx = fpsList.indexOf(_fps);
    final nextFps = fpsList[(idx + 1) % fpsList.length];
    setState(() {
      _fps = nextFps;
      _camera.setShutterAngle(shutterAngle: _shutterAngle, fps: _fps, iso: _iso);
    });
  }

  void _tapToLockNeutralGray() {
    setState(() {
      _kelvin = 5600;
      _tint = 0;
      _camera.setKelvinTint(kelvin: _kelvin, tint: _tint);
    });
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(
        content: Text("Neutral Gray Locked (5600K / Tint 0)"),
        duration: Duration(seconds: 1),
      ),
    );
  }

  String _formatTimecode(int totalSeconds) {
    final hours = (totalSeconds ~/ 3600).toString().padLeft(2, '0');
    final minutes = ((totalSeconds % 3600) ~/ 60).toString().padLeft(2, '0');
    final seconds = (totalSeconds % 60).toString().padLeft(2, '0');
    return "$hours:$minutes:$seconds:00";
  }

  String get _monitoringLabel {
    switch (_monitoringMode) {
      case 0: return "R-LOG (FLAT)";
      case 1: return "REC.709 LUT";
      case 2: return "FALSE COLOR";
      case 3: return "FOCUS PEAKING";
      case 4: return "ZEBRAS (95%)";
      default: return "VIEWFINDER";
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        children: [
          // 1. Live Viewfinder Sensor Canvas (Native Surface Texture / Fallback)
          Center(
            child: AspectRatio(
              aspectRatio: (_cropMode == 0) ? (16 / 9) : (4 / 3),
              child: Container(
                decoration: BoxDecoration(
                  color: const Color(0xFF15181C),
                  border: Border.all(
                    color: _isRecording ? Colors.redAccent : Colors.white12,
                    width: _isRecording ? 2.5 : 1.0,
                  ),
                ),
                child: _textureId != null
                    ? ClipRect(
                        child: Texture(textureId: _textureId!),
                      )
                    : Center(
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            Icon(
                              Icons.camera_rounded,
                              size: 48,
                              color: _isStreaming ? Colors.greenAccent.withValues(alpha: 0.5) : Colors.white.withValues(alpha: 0.15),
                            ),
                            const SizedBox(height: 8),
                            Text(
                              _statusMessage,
                              style: TextStyle(
                                color: Colors.white.withValues(alpha: 0.3),
                                fontSize: 11,
                                fontWeight: FontWeight.w600,
                                letterSpacing: 1.5,
                              ),
                            ),
                          ],
                        ),
                      ),
              ),
            ),
          ),

          // 2. Framing Guide Overlay (Crosshairs & 180° Rule Marker)
          Center(
            child: SizedBox(
              width: 16,
              height: 16,
              child: CustomPaint(
                painter: CrosshairPainter(),
              ),
            ),
          ),

          // 3. Top Status & Telemetry HUD
          SafeArea(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16.0, vertical: 8.0),
              child: Row(
                children: [
                  // R-Log Profile Badge
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    decoration: BoxDecoration(
                      color: Colors.amber.withValues(alpha: 0.15),
                      borderRadius: BorderRadius.circular(4),
                      border: Border.all(color: Colors.amber, width: 1),
                    ),
                    child: const Text(
                      "R-LOG",
                      style: TextStyle(color: Colors.amber, fontWeight: FontWeight.bold, fontSize: 11),
                    ),
                  ),
                  const SizedBox(width: 8),

                  // Aspect Ratio / Crop Toggle
                  GestureDetector(
                    onTap: _toggleCropMode,
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                      decoration: BoxDecoration(
                        color: Colors.white10,
                        borderRadius: BorderRadius.circular(4),
                        border: Border.all(color: Colors.white24),
                      ),
                      child: Text(
                        _cropMode == 0 ? "4K UHD (16:9)" : "OPEN-GATE (4:3)",
                        style: const TextStyle(color: Colors.white, fontSize: 11, fontWeight: FontWeight.w600),
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),

                  // Viewfinder Monitoring LUT & Scopes Toggle
                  GestureDetector(
                    onTap: _cycleMonitoringMode,
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                      decoration: BoxDecoration(
                        color: _monitoringMode == 1 ? Colors.cyan.withValues(alpha: 0.2) : Colors.white10,
                        borderRadius: BorderRadius.circular(4),
                        border: Border.all(color: _monitoringMode == 1 ? Colors.cyan : Colors.white24),
                      ),
                      child: Text(
                        _monitoringLabel,
                        style: TextStyle(
                          color: _monitoringMode == 1 ? Colors.cyanAccent : Colors.white,
                          fontSize: 11,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                    ),
                  ),

                  const Spacer(),

                  // Recording Timecode
                  if (_isRecording)
                    FadeTransition(
                      opacity: _pulseController,
                      child: Container(
                        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                        decoration: BoxDecoration(
                          color: Colors.red.shade900,
                          borderRadius: BorderRadius.circular(4),
                        ),
                        child: Text(
                          _formatTimecode(_recordedSeconds),
                          style: const TextStyle(
                            color: Colors.white,
                            fontFamily: 'monospace',
                            fontWeight: FontWeight.bold,
                            fontSize: 13,
                          ),
                        ),
                      ),
                    ),
                  const SizedBox(width: 12),

                  // Audio VU meter
                  Row(
                    children: [
                      const Icon(Icons.mic, color: Colors.greenAccent, size: 14),
                      const SizedBox(width: 4),
                      Container(
                        width: 48,
                        height: 6,
                        decoration: BoxDecoration(
                          color: Colors.white12,
                          borderRadius: BorderRadius.circular(2),
                        ),
                        child: FractionallySizedBox(
                          alignment: Alignment.centerLeft,
                          widthFactor: 0.65,
                          child: Container(
                            decoration: BoxDecoration(
                              color: Colors.greenAccent,
                              borderRadius: BorderRadius.circular(2),
                            ),
                          ),
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ),

          // 4. Bottom Professional Cinema Control Rack
          Positioned(
            left: 0,
            right: 0,
            bottom: 0,
            child: Container(
              color: Colors.black.withValues(alpha: 0.85),
              padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  // FPS Selector
                  _buildControlPill(
                    label: "FPS",
                    value: "${_fps.toInt()}",
                    onTap: _cycleFps,
                  ),

                  // Shutter Angle
                  _buildControlPill(
                    label: "SHUTTER",
                    value: "${_shutterAngle.toInt()}°",
                    subtitle: "1/${(_fps * 360 / _shutterAngle).round()}s",
                    onTap: _cycleShutterAngle,
                  ),

                  // ISO Sensitivity
                  _buildControlPill(
                    label: "ISO",
                    value: "$_iso",
                    onTap: _cycleIso,
                  ),

                  // White Balance & Kelvin
                  _buildControlPill(
                    label: "WB",
                    value: "${_kelvin}K",
                    subtitle: "TINT $_tint",
                    onTap: () {
                      setState(() {
                        _kelvin = (_kelvin == 5600) ? 3200 : 5600;
                        _camera.setKelvinTint(kelvin: _kelvin, tint: _tint);
                      });
                    },
                  ),

                  // Tap to Lock Neutral Gray
                  IconButton(
                    icon: const Icon(Icons.colorize_rounded, color: Colors.white70, size: 20),
                    tooltip: "Tap to Lock Neutral Gray",
                    onPressed: _tapToLockNeutralGray,
                  ),

                  // Hardware OIS Toggle
                  GestureDetector(
                    onTap: () {
                      setState(() {
                        _oisEnabled = !_oisEnabled;
                        _camera.setOis(_oisEnabled);
                      });
                    },
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                      decoration: BoxDecoration(
                        color: _oisEnabled ? Colors.green.withValues(alpha: 0.2) : Colors.white10,
                        borderRadius: BorderRadius.circular(4),
                        border: Border.all(color: _oisEnabled ? Colors.greenAccent : Colors.white24),
                      ),
                      child: Text(
                        "OIS",
                        style: TextStyle(
                          color: _oisEnabled ? Colors.greenAccent : Colors.white54,
                          fontWeight: FontWeight.bold,
                          fontSize: 11,
                        ),
                      ),
                    ),
                  ),

                  // Gyroflow IMU Logging Toggle
                  GestureDetector(
                    onTap: () {
                      setState(() {
                        _gyroLogging = !_gyroLogging;
                      });
                    },
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                      decoration: BoxDecoration(
                        color: _gyroLogging ? Colors.blue.withValues(alpha: 0.2) : Colors.white10,
                        borderRadius: BorderRadius.circular(4),
                        border: Border.all(color: _gyroLogging ? Colors.blueAccent : Colors.white24),
                      ),
                      child: Text(
                        "GYRO",
                        style: TextStyle(
                          color: _gyroLogging ? Colors.blueAccent : Colors.white54,
                          fontWeight: FontWeight.bold,
                          fontSize: 11,
                        ),
                      ),
                    ),
                  ),

                  // Record Trigger Button
                  GestureDetector(
                    onTap: _toggleRecording,
                    child: Container(
                      width: 54,
                      height: 54,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        border: Border.all(color: Colors.white, width: 3),
                      ),
                      child: Center(
                        child: AnimatedContainer(
                          duration: const Duration(milliseconds: 200),
                          width: _isRecording ? 20 : 40,
                          height: _isRecording ? 20 : 40,
                          decoration: BoxDecoration(
                            color: Colors.redAccent,
                            borderRadius: BorderRadius.circular(_isRecording ? 4 : 20),
                          ),
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildControlPill({
    required String label,
    required String value,
    String? subtitle,
    required VoidCallback onTap,
  }) {
    return GestureDetector(
      onTap: onTap,
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
        decoration: BoxDecoration(
          color: Colors.white.withValues(alpha: 0.08),
          borderRadius: BorderRadius.circular(6),
          border: Border.all(color: Colors.white12),
        ),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            Text(
              label,
              style: TextStyle(color: Colors.white.withValues(alpha: 0.5), fontSize: 9, fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 2),
            Text(
              value,
              style: const TextStyle(color: Colors.white, fontSize: 13, fontWeight: FontWeight.bold),
            ),
            if (subtitle != null) ...[
              const SizedBox(height: 1),
              Text(
                subtitle,
                style: TextStyle(color: Colors.white.withValues(alpha: 0.4), fontSize: 8),
              ),
            ],
          ],
        ),
      ),
    );
  }
}

class CrosshairPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()
      ..color = Colors.white.withValues(alpha: 0.3)
      ..strokeWidth = 1.0;

    final cx = size.width / 2;
    final cy = size.height / 2;

    canvas.drawLine(Offset(0, cy), Offset(size.width, cy), paint);
    canvas.drawLine(Offset(cx, 0), Offset(cx, size.height), paint);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
