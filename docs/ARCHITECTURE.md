# Vesper Cine — Architecture

Pixel RAW10 sensor data → GPU → Apple Log (Rec.2020) → 10-bit HEVC/AV1, with a
viewfinder that shows exactly what is being recorded. Nothing from the ISP's
processed path (tone mapping, sharpening, temporal NR, HDR merge) is used.

```
 Camera2 NDK (TEMPLATE_MANUAL, 3A off, fixed frame duration)
   │ RAW10 largest mode = 2x2-binned readout (~4080x3072 on Pixel)
   │ CaptureResult: dynamic black/white level, lens shading map, timestamps
   ▼
 AImageReader (CPU_READ_OFTEN, 5 images) ── reader thread ──────────────────┐
   │ memcpy into ring slot i (3 slots, persistently mapped, write-combined)   │
   ▼                                                                          │
 Vulkan, one queue submission per frame                                        │
   pass 1  unpack.comp   RAW10 → quad image (rgba16f, raw/2 size)            │
           black level (signed), lens shading map, white balance,             │
           highlight clip to lowest channel + chroma fade, Gr/Gb averaged     │
   pass 2  render.comp   quad image → upright crop → resample (per-channel    │
           sub-pixel CFA offsets, bilinear) → DNG camera→XYZ(D50)→Rec.2020     │
           ×k → Apple Log → { P010 (BT.2020 limited), viewfinder RGBA8 }      │
   copy    viewfinder → swapchain image on Flutter's Surface (no CPU readback)│
   ▼                                                                          │
 Recorder thread: wait slot fence → copy P010 into MediaCodec input buffer ◄──┘
   → HEVC Main10 / AV1 Main10 (VBR) ─┐
 AAudio 48 kHz stereo → AAC-LC 320k ─┴→ AMediaMuxer (MP4) → MediaStore fd
```

## Why these choices

| Decision | Why |
|---|---|
| **Binned RAW10 readout**, not full 50 MP | Full field of view, ~2x better SNR per output pixel, the fastest readout (least rolling shutter), and the only mode that can reach 30–60 fps. Full-res quad-Bayer would need remosaicing and still be downscaled. |
| **Quad (2x2 superpixel) debayer** | For 1080p the output is ~half the raw width, so a 2x2 quad already has more than one full RGB sample per output pixel. No demosaic interpolation means no zipper, maze or false-colour artefacts. Averaging Gr/Gb removes green split. The render pass samples R and B at their true sub-pixel positions, so there is no chroma misregistration. This is the approach used by HDR+ (quad-based merge) and MotionCam's preview. A full-res demosaic pass only becomes necessary for true UHD. |
| **CPU memcpy upload** | Importing RAW10 `AHardwareBuffer`s crashes the PowerVR gralloc. Importing as a `VkBuffer` requires `AHARDWAREBUFFER_FORMAT_BLOB`, which RAW10 buffers aren't, so it isn't a conformant path. The copy is ~15 MB/frame (~2–4 ms) into write-combined memory, runs on a 3-slot ring, and never waits on the GPU in the steady state. |
| **Swapchain viewfinder** | The old path read the viewfinder back from GPU to CPU through uncached memory and then used `ANativeWindow_lock` (12–35 ms/frame). The swapchain uses MAILBOX when available and acquire timeout 0, so the display can never stall the camera. If swapchain creation fails, a readback from cached memory is kept as a fallback. |
| **P010 ByteBuffer encoder input** | We choose the exact YCbCr matrix (BT.2020 NCL), the range (limited) and 10-bit quantisation, and every frame carries its sensor timestamp as PTS. An input Surface would hand RGB→YUV conversion to the vendor. |
| **Apple Log / Rec.2020** | A published, C1-continuous log curve with native support in Resolve, Premiere and FCP. It covers linear 0→12 (18% grey = 0.488), which exceeds the sensor's range, so no custom curve is needed. See the README for how the dynamic range is mapped. |

## Threads and locks

| Thread | Work | Locks (in order) |
|---|---|---|
| Camera reader (NDK) | acquire image, meter WB, `processFrame`, hand slot to recorder | `CameraEngine::frameMutex_` → `gStateMutex` (brief) → `VulkanEngine::frameMutex_` → `encoderMutex_` |
| Capture result (NDK) | parse per-frame metadata into a ring | `metaMutex_` |
| Dart / FFI | settings, start/stop | `CameraEngine::mutex_` **or** `gStateMutex`, never both. The bridge never calls into the camera while holding `gStateMutex`. |
| Recorder | encode, audio, mux, thermal/storage guards | `jobMutex_`, `statusMutex_`, `encoderMutex_` |
| JNI (Surface) | `setViewfinderWindow` | `windowMutex_` only |
| Recovery | reopen camera after a fatal device error | `CameraEngine::mutex_` → `frameMutex_` |

`stopCapture` removes the image listener, then takes `frameMutex_` before deleting the reader. An in-flight frame therefore always finishes before its `AImage` is freed.

## Colour pipeline (DNG 1.6 §6)

* `ColorMatrix1/2` (with `CalibrationTransform`) and `ForwardMatrix1/2` are interpolated by **mired** between the two reference illuminants.
* **Kelvin/tint dial:** `xy = Planckian(T) + tint·Duv` (Adobe tint scale, 1 unit = Duv 1/3000). Then `neutral = CM(T)·XYZ(xy)`.
* **Tap-to-WB:** the raw centre patch is averaged per channel (clipped pixels skipped). The DNG xy↔neutral fixed-point iteration then finds the scene white, and the equivalent Kelvin/tint is reported back to the UI.
* `wbGains = 1/neutral` (G = 1), applied **before** highlight clipping.
* `camToRec2020 = k · M(XYZ_D50→Rec.2020, Bradford) · FM · D · CC⁻¹ · diag(neutral)`, so the shader only does `M · (raw · gains)`.
* All of this is covered by `test/native/color_science_test.cpp`.

## Tests

`test/native/run_tests.sh`:
1. Colour-science unit tests.
2. Type-checks every native source against the real Vulkan headers plus NDK header stubs.
3. Runs the real `VulkanEngine` and shaders on a host Vulkan driver (Mesa lavapipe) over a synthetic RAW10 frame. It checks the 18% grey code value, neutral clipped highlights, chroma sign, rotation, lens shading gain and ring-slot reuse.

`flutter analyze` and `flutter test` cover the Dart side. On-device verification still needs the logcat checklist in the README.
