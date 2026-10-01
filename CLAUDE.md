# Vesper Cine — agent notes

Pixel RAW10 → Vulkan → Apple Log cinema camera (Flutter UI + C++ engine). Read
`README.md` (status, testing) and `docs/ARCHITECTURE.md` (pipeline, threads, guard) first.
The owner tests on a **Pixel 10** and is not a programmer: explain results plainly.

## Versioning (required on every change)
After **every** piece of work, bump `version:` in `pubspec.yaml` before committing:
- **minor** (`0.X.0`) for new features or behaviour changes testers will notice;
- **patch** (`0.x.Y`) for fixes, tuning and small UI tweaks;
- always increment the **build number** after `+` by 1.
Mention the new version in the commit message and in the reply to the user.
For user-visible changes also add an entry to `lib/ui/changelog.dart` (shown in Settings › Info › What's new).

## Checks before committing
- `test/native/run_tests.sh` (colour science, native type-check, GPU pipeline on lavapipe, calibration, focus, ISO analysis)
- `flutter analyze` and `flutter test`; then `git checkout analysis_options.yaml pubspec.lock` (the Flutter tool rewrites them). If `flutter` isn't on PATH, look for an SDK (e.g. `/tmp/flutter/bin`) before concluding Dart can't be checked.
- After editing any `.comp`/`.glsl`: `android/app/src/main/cpp/shaders/compile_shaders.sh` to regenerate the `*_spv.h` headers (green.comp has 4 variants: R16F/RGBA16F × fp32/fp16).
- New NDK functions used in native code need a declaration in `test/native/android_stubs/`.
- Git: commit and push directly to `main` (owner's choice).

## Builds and testing
- APKs come from GitHub Actions (**Build arm64 APK**, manual `workflow_dispatch`: build_mode, dev_tools). Pushing does not build — if a log looks like an old version, check which commit the latest run used.
- Ask for logs from **Settings › Developer › App log › Copy** (in-app ring buffer, all native log lines since launch). Native logging goes through `vesperLog` (`app_log.h`); new log lines should use the existing `LOGI`/`VK_LOGI`/… macros.
- Profile builds are the test builds (release-speed UI/GPU, dev tools visible).

## Layout
- `android/app/src/main/cpp/` — native engine: `camera_engine` (Camera2 NDK), `vulkan_engine` (GPU pipeline, submit thread, guard), `shaders/` (unpack, green, align, clean, render), `color_science`, `recorder`, `focus_controller`, `iso_analysis`, `app_log.h`. `native_bridge.cpp` holds the `vesper_*` C exports, `onFrame` and the status JSON.
- `lib/services/vesper_native.dart` — FFI bindings + method channel; `lib/ui/` — Flutter UI (`camera_screen.dart`, `settings_sheet.dart` side-rail settings, `app_log_screen.dart`, `changelog.dart`).
- `android/app/src/main/kotlin/.../MainActivity.kt` — permissions, rotation, viewfinder `SurfaceView` (behind the transparent Flutter surface), `deviceInfo` (version), MediaStore files.
- `tools/calibration/` — per-device colour calibration (ColorChecker).

## Current performance facts (Pixel 10, 1080p, 16:9 4000x2256 readout)
- GPU per frame, everything on: ~34–35 ms (24 fps budget 41.7). Benchmark deltas: base ~16–18, sharpening +3, HQ +14–16 (10.8 measured when paused at 48 fps), chroma NR +3–5, TNR +3–4, alignment ~3 (every other frame).
- 16-bit shader arithmetic is supported (HQ uses the fp16 variant); HQ image format R16F.
- Per-pass GPU timestamps are unreliable on this PowerVR GPU; trust only the frame total. lavapipe timings in tests are meaningless for the phone.
- Camera thread work ~3 ms (memcpy ~2.5 ms). The GPU is the bottleneck, not the CPU.

## Gotchas (learned the hard way)
- Importing RAW10 AHardwareBuffers into Vulkan crashed the gralloc / isn't conformant: keep the memcpy.
- Never block the camera thread on submit/present: that's the submit thread's job. Never let the compositor or Flutter pace the viewfinder.
- Guard costs measured while overloaded are inflated; only restore-time costs are reliable (see ARCHITECTURE).
- Opening Settings pauses processing (not while recording); drop counters reset on pause.
- A 1-quad tile border in green.comp cost +5 ms on the phone: measure on device before assuming a "fold" or merge is faster.
- Quad image alpha: 1 = clipped highlight, else 0.1 × lens-shading gain (used by NR/sharpen noise models).

## UI-only work
Agents doing design/UI work must not edit `android/**`, `lib/services/vesper_native.dart`, `tools/**`, `color_science/**` or `test/native/**`. Keep the engine calls in `camera_screen.dart` (`_start`, `_openAndStream`, lifecycle, `_onPoll`, recording) and their order intact. Tap coordinates are normalised to the viewfinder box (`_vfKey`), whose rect is sent to the native SurfaceView (`_syncViewfinder`); keep that box transparent.

## Build flavours
- `kDevTools` (`lib/build_flags.dart`): developer features (Settings › Developer: GPU guard switch, App log, GPU benchmark, chart calibration capture) in debug/profile builds, hidden in release; force with `--dart-define=VESPER_DEV_TOOLS=true` (workflow input `dev_tools`). Gate new internal/diagnostic tools behind it. Release builds always run the GPU guard on AUTO.
- All builds are signed with the shared tester key `android/app/vesper-dev.jks` so APKs install over each other.

## Next up
4K/UHD output (engine `resolution` setting exists but upsamples the quad image; not in the UI). Needs a full-res path and GPU headroom — get a fresh on-device benchmark first.
