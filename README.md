# Vesper Cine

A cinema camera for Google Pixel (8 / 9 / 10). It reads the sensor's RAW10 data directly and develops it on the GPU into **Apple Log / Rec.2020**. Footage is recorded as **10-bit HEVC (or AV1)** with synced audio. The Pixel's video ISP path is bypassed entirely: no tone mapping, no sharpening halos, no temporal smearing.

Architecture, threading and colour maths are described in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Status

| Area | State |
|---|---|
| RAW10 capture (binned readout), fixed frame duration 24/25/30/60 fps | Implemented |
| Per-frame dynamic black/white level, lens shading map (vignetting + colour shading) | Implemented |
| Quad debayer, CFA-aware sub-pixel resampling, Gr/Gb split removal, neutral highlight clipping | Implemented, GPU-tested on host |
| DNG dual-illuminant colour (CM/FM/CC, mired interpolation), Kelvin + tint, raw tap-to-WB | Implemented, unit-tested |
| Apple Log (Rec.2020) master, Rec.709 / false colour / peaking / zebra monitoring | Implemented, GPU-tested on host |
| Swapchain viewfinder (no CPU readback), 3-slot GPU ring | Implemented |
| Recording: HEVC Main10 / AV1 Main10 (P010 input) + AAC, MP4 in `Movies/Vesper Cine` | Implemented |
| Thermal stop (`SEVERE`), low-storage stop, dropped-frame counter | Implemented |
| Hardware AF (PDAF + laser via HAL): continuous, tap-to-focus, lock; face detection | Implemented |
| Google AWB (HAL neutral point, followed live) or manual Kelvin/tint | Implemented |
| Lens distortion correction (Camera2 lens model), hot/dead pixel repair | Implemented, GPU-tested on host |
| Temporal NR (motion-adaptive, sensor noise profile) and chroma NR — optional | Implemented, GPU-tested on host |
| Full-range shutter (angle or speed), ISO, fps pickers; one-shot auto-exposure assist | Implemented |
| OIS toggle | Implemented |
| True UHD (full-res demosaic pass) | Planned. UHD output currently upsamples the ~2040 px quad image. |
| Gyroflow IMU log, external SSD | Planned |

**Not yet verified on a device.** This build environment can't reach the Android SDK/NDK, so the Android build itself hasn't been compiled here. See *First on-device run* below.

## Exposure & Apple Log

Apple Log maps scene-linear 0 → 12 onto code values 0.15 → 1.0, with 18% grey at 0.488. The binned Pixel sensor delivers about 11–12 stops at base ISO in 10 bits, which fits inside that range. So a custom curve isn't needed, and a standard one gets native support in Resolve, Premiere and FCP.

How the sensor's range is placed on the curve is set by **highlight headroom**: the number of stops between 18% grey and sensor clip.

* Default is **5.5 stops**. Clip maps to linear 8.1, which is Apple Log 0.952.
* **Expose so 18% grey reads 0.49** (the green band in false colour). That leaves 5.5 stops above grey and roughly 5–6 stops of usable shadow detail below it.
* `vesper_set_highlight_headroom()` (Dart: `setHighlightHeadroom`) trades highlights for shadows between 3 and 6.3 stops.

Blown highlights are clipped to the lowest channel's saturation and faded to neutral, so they never turn magenta.

**In post:** set the clip's input colour space to **Apple Log** and the gamut to **Rec.2020**. MP4 has no transfer-curve code for Apple Log, so files are tagged BT.2020 primaries/matrix, limited range, transfer unspecified — the same as iPhone. `color_science/` contains Apple Log → Rec.709 LUTs (Gamma 2.4, plus an sRGB version that matches the viewfinder) and a matching DCTL. Regenerate them with `python3 color_science/generate_applelog_lut.py`.

## Building

```bash
flutter pub get
flutter run --release            # Pixel, USB debugging on
```

After editing any shader:

```bash
android/app/src/main/cpp/shaders/compile_shaders.sh   # needs glslangValidator
```

Host-side tests (Linux, needs `g++`, `glslang-tools`, `libvulkan-dev`, `mesa-vulkan-drivers`):

```bash
test/native/run_tests.sh   # colour maths, native type-check, real GPU pipeline on lavapipe
flutter analyze && flutter test
```

## First on-device run — what to send back

Run `adb logcat -s Vesper Vesper_Camera Vesper_Vulkan Vesper_Recorder` and send these lines:

1. `Sensor: white=... black=[...] CFA=... shadingMap=CxR (applied=...)` and `Calibration: illuminants ...K/...K, CM2=.. FM1=.. FM2=..`. These confirm the calibration data and shading map exist.
2. `RAW10 mode WxH max N fps` for each mode. This tells us whether 60 fps is possible at the binned size.
3. `Viewfinder swapchain ...` **or** `Swapchain unavailable ... CPU copy fallback`.
4. `Frame N: avg wait / upload / record+submit, dropped` (logged every 120 frames).
5. After tap-to-WB on a grey card: `WB lock: raw neutral ... -> ...K tint ...`.
6. When recording: `Recording WxH @ fps, <encoder name>, N Mb/s, audio=1, input stride ...`. If it fails instead, send the `10-bit P010 encoder configuration rejected` line.

Also please check: grey card under LED light after tap-to-WB (is the green/yellow cast gone?), corners of a white wall (vignetting/colour shading corrected?), a clip opened in Resolve with Input = Apple Log.

## What changed in the audit

The previous pipeline had several defects that directly caused the reported issues.

**Colour**
- **Green cast from tap-to-WB.** The gains were `grey/r, 1, grey/b` instead of `g/r, 1, g/b`, which left green un-normalised.
- **Kelvin dial.** It always computed the neutral for D50, whatever Kelvin was requested. The tint direction was also inverted, and used a display-colour approximation instead of a proper Duv offset.
- **R-Log curve.** The old curve was discontinuous at 0.01 (a jump of 0.29), and grey actually landed at 0.54, not the 0.40 the docs claimed. It has been replaced by Apple Log.
- **Black level ordering.** Dynamic black level was read in physical CFA order but used as logical R/Gr/Gb/B, which is wrong on this GRBG sensor. Dynamic white level was also never read.
- **Lens shading.** It was never applied (`SHADING_MODE_OFF`, no map requested), so vignetting and colour shading were uncorrected.
- **Magenta highlights.** Nothing clipped the channels after WB gains.

**Image quality**
- **Aliasing and moiré.** Output was point-sampled from the (0,0) Bayer phase, with MHC coefficients that didn't match the published kernel. Replaced by the quad debayer with filtered resampling.

**Performance**
- The viewfinder did a GPU→CPU readback through uncached memory plus `ANativeWindow_lock` on the camera thread.
- A single command buffer/fence meant no pipelining.
- No frame duration was set, so fps was whatever the HAL defaulted to.

**Stability**
- The reader window was double-released.
- Teardown raced with the frame callback, so the image could be freed while in use.
- Unlocked setters raced with request teardown.
- Device-error recovery ran on the camera callback thread.
- Crop changes wrote out of bounds mid-frame.
- `acquireLatestImageAsync` was called with a null fence.

## History (pre-audit, on-device findings)

- **Importing RAW10 `AHardwareBuffer` as a Vulkan image crashes the PowerVR gralloc** (`SIGTRAP` in `gralloc_native_handle_bpp`). This is why frames are copied, not imported.
- **The sensor reports CFA=1 (GRBG).** Every CFA lookup is resolved dynamically.
- **At 4K the old per-pixel MHC shader took ~92 ms/frame.** This is why output is 1080p, and why the new quad pipeline exists.
- **`ForwardMatrix` outputs XYZ relative to D50,** so it is Bradford-adapted to Rec.2020's D65.
- **The static black level pattern is unpopulated on this HAL.** Black level is read per frame from the CaptureResult.

## Roadmap

1. On-device verification of the items above.
2. A true UHD path: full-res demosaic pass (e.g. RCD or MHC with a shared-memory tile) plus an area-filtered downscale.
3. Temporal/spatial denoise driven by `SENSOR_NOISE_PROFILE` (optional, off by default).
4. Gyroflow IMU logging (`ASensorManager`, 200 Hz+, synced to sensor timestamps).
5. External USB-C SSD recording.
