# Project RawEdge (R-Camera)

**Project RawEdge** is an open-source, cinema-grade camera application engineered for modern Google Pixel devices (Pixel 8 / 9 / 10 series). It completely bypasses Google's hardware Image Signal Processor (ISP) post-processing pipeline—eliminating aggressive chroma de-noising, edge-sharpening, and blotchy low-light watercolor smearing.

---

## Key Features

- **Pure Sensor RAW10 Ingestion:** Direct low-level frame capture via `libcamera2ndk` (`AIMAGE_FORMAT_RAW10`).
- **Complete ISP Bypass:** Noise reduction, edge enhancement, and tone-mapping set to `OFF`/manual.
- **Real-Time GPU Debayering:** Malvar-He-Cutler (MHC) $5 \times 5$ gradient-corrected bilinear demosaic running on Vulkan Compute.
- **R-Log Color Science:** Custom logarithmic Opto-Electronic Transfer Function (OETF) calibrated to the Pixel's ~11.5 stop dynamic range.
  - 18% Middle Gray sits at **40% IRE** (code value 553 / 1023 in 10-bit).
  - Sensor clip point sits at **95% IRE** (smooth highlight shoulder).
- **Cinema Standard Exposure Controls:**
  - 180° Shutter Angle Lock (with 90°, 270°, 360° presets).
  - Manual ISO ladder (50 to 3200).
  - Manual White Balance (Kelvin 2000K–10000K & Tint) with **"Tap to Lock Neutral Gray"**.
- **Professional Monitoring Tools:**
  - Viewfinder Rec.709 Preview LUT toggle.
  - False Color (standard IRE color ramp for skin tones and clipping).
  - Focus Peaking (cinema green edge detection overlay).
  - Animated Zebra stripes (95%+ highlight warning).
- **Stabilization Options:**
  - Hardware Voice-Coil Lens OIS toggle.
  - High-rate IMU Gyro logging for post-stabilization in Gyroflow.
- **Framing & Aspect Ratio:**
  - 16:9 4K UHD center crop ($3840 \times 2160$).
  - 4:3 Open-Gate ($3840 \times 2880$ full sensor binned readout).

---

## Repository Structure

```
R-Camera/
├── android/
│   ├── app/src/main/
│   │   ├── cpp/
│   │   │   ├── CMakeLists.txt              # Native build config (C++20)
│   │   │   ├── camera_engine.h/.cpp        # NDK Camera2 RAW10 ingestion & ISP bypass
│   │   │   ├── vulkan_compute.h/.cpp       # Vulkan Compute zero-copy GPU pipeline
│   │   │   ├── native_bridge.cpp           # C-ABI bridge for Dart FFI
│   │   │   └── shaders/
│   │   │       ├── mhc_rlog.comp           # GLSL compute shader (MHC demosaic + R-Log)
│   │   │       ├── mhc_rlog.spv            # Compiled SPIR-V bytecode
│   │   │       └── mhc_rlog_spv.h          # Embedded bytecode header
│   │   └── AndroidManifest.xml             # Camera, audio, sensor permissions
├── color_science/
│   ├── R-Log_to_Rec709.dctl               # DaVinci Resolve Studio DCTL plugin
│   ├── R-Log_to_Rec709.cube               # 33x33x33 3D conversion LUT (Premiere / FCP / Resolve)
│   └── generate_rlog_cube.py              # Precision LUT generator script
├── lib/
│   ├── main.dart                          # Flutter entrypoint
│   ├── services/
│   │   └── rcamera_native.dart            # Dart FFI communication layer
│   └── ui/
│       └── camera_screen.dart             # Cinema UI, dials, scopes & HUD
└── test/
    ├── offline_raw_processor.py           # Desktop raw validation harness
    └── widget_test.dart                   # Smoke test
```

---

## Color Grading in DaVinci Resolve

### Option A: Using the DCTL (Recommended for DaVinci Resolve Studio)
1. Copy `color_science/R-Log_to_Rec709.dctl` to your DaVinci Resolve LUT directory:
   - **Windows:** `%ALLUSERSPROFILE%\Blackmagic Design\DaVinci Resolve\Support\LUT\`
   - **macOS:** `/Library/Application Support/Blackmagic Design/DaVinci Resolve/LUT/`
2. In DaVinci Resolve (Color Page), add a DCTL node and select `R-Log_to_Rec709`.
3. Adjust Exposure Compensation and Highlight Roll-off sliders dynamically!

### Option B: Using the 3D `.cube` LUT (Any Editor)
1. Import `color_science/R-Log_to_Rec709.cube` into Premiere Pro, Final Cut Pro, or DaVinci Resolve.
2. Apply the LUT directly to your clip on a Rec.709 timeline.

---

## How to Build & Run

### Prerequisites
- Flutter SDK 3.24+ (Dart 3.5+)
- Android SDK 34+ with NDK 27+ / 28+
- Android Studio / JDK 21

### Run on Connected Device
```bash
# Verify ADB connection to Pixel device
adb devices

# Build and run
flutter run --release
```
