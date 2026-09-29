# Vesper Cine — agent notes

## Versioning (required on every change)
After **every** piece of work, bump `version:` in `pubspec.yaml` before committing:
- **minor** (`0.X.0`) for new features or behaviour changes testers will notice;
- **patch** (`0.x.Y`) for fixes, tuning and small UI tweaks;
- always increment the **build number** after `+` by 1.
Mention the new version in the commit message and in the reply to the user.

## Checks before committing
- `test/native/run_tests.sh` (colour science, native type-check, GPU pipeline on lavapipe, calibration, focus, ISO analysis)
- `flutter analyze` and `flutter test`; then `git checkout analysis_options.yaml pubspec.lock` (the Flutter tool rewrites them)
- After editing any `.comp`/`.glsl`: `android/app/src/main/cpp/shaders/compile_shaders.sh` to regenerate the `*_spv.h` headers

## Layout
- `android/app/src/main/cpp/` — native engine (Camera2 NDK capture, Vulkan GPU pipeline, colour science, recorder, focus/exposure controllers, ISO analysis). `native_bridge.cpp` holds the `vesper_*` C exports.
- `lib/services/vesper_native.dart` — FFI bindings; `lib/ui/` — Flutter UI.
- `tools/calibration/` — per-device colour calibration (ColorChecker).

## UI-only work
Agents doing design/UI work must not edit `android/**`, `lib/services/vesper_native.dart`, `tools/**`, `color_science/**` or `test/native/**`. Keep the engine calls in `camera_screen.dart` (`_start`, `_openAndStream`, lifecycle, `_onPoll`, recording) and their order intact; tap coordinates are normalised to the viewfinder texture rect.

## Build flavours
- `kDevTools` (`lib/build_flags.dart`): developer-only features (chart calibration capture) are shown in debug/profile builds and hidden in release builds; force with `--dart-define=VESPER_DEV_TOOLS=true` (GitHub workflow input `dev_tools`). Gate new internal/diagnostic tools behind it.
- All builds are signed with the shared tester key `android/app/vesper-dev.jks` so APKs install over each other.
