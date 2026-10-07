# Vesper Cine

A cinema camera app for Google Pixel phones (developed on a **Pixel 10**). It reads the sensor's **RAW10** data directly, develops it on the GPU (Vulkan) with its own colour science into **Apple Log / Rec.2020**, and records **10-bit HEVC or AV1** with audio. The phone's video ISP path is bypassed entirely: no tone mapping, no sharpening halos, no temporal smearing.

- Architecture, threading, GPU passes, colour maths: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Notes for AI agents / contributors (rules, checks, gotchas, current numbers): [`CLAUDE.md`](CLAUDE.md)
- Per-device colour calibration: [`tools/calibration/README.md`](tools/calibration/README.md)
- User-facing changelog: [`lib/ui/changelog.dart`](lib/ui/changelog.dart) (shown in the app under Settings › Info)

## Status (v0.11)

| Area | State |
|---|---|
| RAW10 capture, 16:9 readout 4000×2256 (up to 60 fps) or 4:3 4000×3000 (up to 30 fps); 23.976–60 fps | Working on Pixel 10 |
| Dynamic black/white level, lens shading map, DNG dual-illuminant colour, Kelvin/tint, Google AWB follow, WB eyedropper | Working |
| Apple Log (Rec.2020) master; viewfinder Rec.709 LUT, false colour, peaking, zebra, histogram/waveform, 3× magnifier | Working |
| HQ oversampling (full-sensor luma, anti-aliased to 1080p), noise-aware sharpening, lens distortion correction, hot-pixel repair | Working |
| Temporal NR (motion-adaptive, tile alignment) and chroma NR, noise model from the sensor profile and lens-shading gain | Working |
| Recording: HEVC/AV1 Main10 1080p (P010 input) + AAC, MP4 in `Movies/Vesper Cine`; thermal / storage stops | Working |
| GPU performance guard: alignment at reduced rate → alignment off → HQ → NR when frames would drop, restores them when they fit; always on while recording | Working |
| Heat safeguard: Android's thermal forecast steps processing down gently before the phone gets too hot; at "severe" the take continues with minimal processing, it stops only at "critical" | Working |
| Native viewfinder (Android `SurfaceView` under a transparent Flutter UI), submit/present on its own thread | Working |
| Screen kept on while the app is open; recording power saver (dim screen / pause viewfinder, tap to wake) | Working |
| PDAF/laser tap AF (track or lock), face detection; clean auto-exposure (native ISO first, shutter to 180°, then gain); native ISO analysis | Working |
| Side-rail Settings with Info tab (version, changelog, how-to, FAQ); in-app log (developer builds) | Working |
| **4K (UHD) output** | Engine has a `resolution` setting that upsamples the 2000-px quad image; not exposed in the UI. A real UHD path is the next big feature (see Roadmap). |
| Gyroflow IMU log, external SSD, audio levels | Planned |

Performance at 1080p on Pixel 10 (GPU time per frame, everything on): **~29–32 ms** — fits 24/25 fps (41.7/40 ms budgets). At 30 fps the guard first runs alignment at a reduced rate (then pauses it if needed); at 48/60 fps it pauses HQ and NR as well. Details in `CLAUDE.md`.

## Using it

- **Tap** the viewfinder to focus (TRACK = AF-C, FOCUS & LOCK = AF-L); **long-press** to focus and lock. Optionally a tap also meters exposure.
- Top bar: Apple Log / LUT, aspect, **FC** false colour, **PEAK**, **MAG**, **ZEBRA**, settings. Left rack: FPS, shutter (angle or speed), ISO, WB, focus, scopes.
- **Settings** (gear): Recording · Audio · Image & NR · Exposure · Color · Focus · Developer (debug/profile builds) · Info.
- Recordings: `Movies/Vesper Cine/VESPER_<date>_<time>.mp4`.

### Exposure & Apple Log

Apple Log maps scene-linear 0 → 12 onto code values 0.15 → 1.0, 18% grey at 0.488. Highlight headroom (stops between 18% grey and sensor clip) defaults to **5.5 stops**; expose so 18% grey reads ~0.49 (green band in false colour). Clipped highlights are faded to neutral (never magenta).

**In post:** input colour space **Apple Log**, gamut **Rec.2020**. Files are tagged BT.2020 primaries/matrix, limited range, transfer unspecified (as iPhone does). `color_science/` has Apple Log → Rec.709 LUTs and a DCTL (`python3 color_science/generate_applelog_lut.py`).

## Building

Android APKs are built by GitHub Actions: **Actions → BUILD MASTER → Run workflow** on `main`, choose `release` / `profile` / `debug`, optionally `dev_tools`. BUILD MASTER first runs all the checks (native tests, analyze, Dart tests, licence + vulnerability scan) and builds only if they pass. Builds do **not** run automatically on push (the checks alone do). All builds are signed with the shared tester key (`android/app/vesper-dev.jks`), so new APKs install over old ones.

Locally:

```bash
flutter pub get
flutter run --profile                                   # Pixel with USB debugging
android/app/src/main/cpp/shaders/compile_shaders.sh     # after editing shaders (needs glslangValidator)
test/native/run_tests.sh                                # colour maths, native type-check, GPU pipeline on lavapipe, …
flutter analyze && flutter test
```

Host tests need `g++`, `glslang-tools`, `libvulkan-dev`, `mesa-vulkan-drivers`, `python3` + `numpy`.

**Build types:** profile is the one to test with — as fast as release for the UI and GPU, keeps logs and developer tools. `kDevTools` (`lib/build_flags.dart`) shows developer tools (GPU guard switch, App log, GPU benchmark, chart calibration capture) in debug/profile builds and hides them in release.

## Testing on a phone

Use **Settings › Developer › App log** (debug/profile builds): it keeps the engine's log since launch and copies it in one tap — no adb needed. Useful lines:

- `Stream mode WxH … @ fps` and `16-bit shader arithmetic: yes/no` (startup)
- every 5 s: `Camera: N fps measured, N sensor drops`, `Frame N: … (max wait) … vf hitches`, `GPU per frame: … = X ms (budget Y ms), guard paused: …`
- guard decisions: `paused stage N`, `paused stage N saved X ms`, `re-enabled stage N costs X ms`, `frame rate changed … all stages back on`
- heat: `Heat: status S, forecast headroom H -> level L`, `Phone hot (heat level L): paused stage N`
- **Settings › Developer › GPU Benchmark** measures each option's GPU cost on the phone.

With adb: `adb logcat -s Vesper Vesper_Camera Vesper_Vulkan Vesper_Recorder VesperBench Vesper_UI`.

## Roadmap

1. **4K / UHD output**: needs a full-resolution demosaic path (the quad image is 2000 px wide) and enough GPU budget — at 1080p the GPU already uses ~34 of 41.7 ms at 24 fps, so 4K needs a cheaper pipeline (pass merging, 16-bit everywhere it's safe) and probably reduced NR.
2. Audio levels / mic selection; Gyroflow IMU logging; external USB-C SSD.
3. Mark unsustainable frame rates in the FPS picker (needs per-setting cost estimates from the guard).
4. Public release; self-calibration for other phones with per-model profiles; monetisation.
5. Pending calibration on the Pixel 10: corner brightness after the lens shading map (needs a White sweep with paper over the lens, outdoors), whether ISO 30 is extended, ColorChecker colour (needs a chart). Details in `CLAUDE.md`.
