#!/usr/bin/env bash
# Host-side tests for the platform-independent native code.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CPP="$ROOT/android/app/src/main/cpp"
OUT="$(mktemp -d)"
g++ -std=c++20 -O1 -Wall -Wextra -I"$CPP" "$CPP/color_science.cpp" "$ROOT/test/native/color_science_test.cpp" -o "$OUT/color_test"
"$OUT/color_test"

# Type check the Android-side sources: real Vulkan headers, and minimal
# stand-ins for NDK camera/media/AAudio headers in test/native/android_stubs
# (signatures transcribed from the NDK reference). The real check is the
# Gradle NDK build.
if [ -f /usr/include/vulkan/vulkan.h ]; then
  g++ -std=c++20 -Wall -Wextra -Wno-missing-field-initializers -fsyntax-only -I"$ROOT/test/native/android_stubs" -I"$CPP" "$CPP/vulkan_engine.cpp"
  for f in camera_engine recorder native_bridge; do
    g++ -std=c++20 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -fsyntax-only -I"$ROOT/test/native/android_stubs" -I"$CPP" "$CPP/$f.cpp"
  done
  echo "native sources type-check against stub NDK headers"
fi

# Runs the real Vulkan engine + shaders on a host Vulkan driver
# (`apt install mesa-vulkan-drivers libvulkan-dev` provides lavapipe).
if [ -f /usr/include/vulkan/vulkan.h ] && ldconfig -p | grep -q libvulkan.so; then
  g++ -std=c++20 -O1 -Wno-missing-field-initializers -I"$ROOT/test/native/android_stubs" -I"$CPP" \
    "$CPP/vulkan_engine.cpp" "$ROOT/test/native/gpu_pipeline_test.cpp" -lvulkan -o "$OUT/gpu_test"
  "$OUT/gpu_test" | grep -v "^\[Vesper_Vulkan\]" || exit 1
fi
