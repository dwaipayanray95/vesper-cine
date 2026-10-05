# Vesper Cine — Architecture

Pixel RAW10 sensor data → Vulkan compute → Apple Log (Rec.2020) → 10-bit HEVC/AV1, with a
viewfinder that shows what is being recorded. Nothing from the ISP's processed path
(tone mapping, sharpening, temporal NR, HDR merge) is used.

```
 Camera2 NDK (TEMPLATE_MANUAL, 3A off, fixed frame duration)
   │ RAW10, 16:9 readout 4000x2256 (≤60 fps) or 4:3 4000x3000 (≤30 fps)
   │ CaptureResult: dynamic black/white level, lens shading map, timestamps
   ▼
 AImageReader (CPU_READ_OFTEN, 8 images, acquireLatestImage) ── camera thread
   │ metering / AE / scopes (sparse CPU samples), memcpy into ring slot (3 slots)
   │ waits only for the frame before last (≤2 frames on the GPU)
   ▼
 VulkanEngine::processFrame — records one command buffer, queues it
   unpack.comp  RAW10 → quad image (rgba16f, 2000x1128): black level, lens shading,
                WB, neutral highlight clip; alpha = clip flag or 0.1×shading gain
   green.comp   [HQ] full-res green (Hamilton-Adams) per 32x16-quad tile, 8-tap
                anti-alias filter → luma detail D = G_hq − G_quad (R16F); fp16 variant
   align.comp   [TNR+align, every 2nd frame, every 4th when the guard is tight] 1/4-res luma
                (4 bilinear reads per 4x4 block), per-tile motion search
   clean.comp   hot-pixel repair, chroma NR (cross-bilateral), temporal NR against
                the ping-pong history (motion-warped); noise model × shading gain
   render.comp  crop/rotate, Catmull-Rom luma + bilinear chroma, + HQ detail,
                sharpening, camera→Rec.2020, Apple Log → P010 (when recording)
                + viewfinder RGBA8 (LUT / false colour / peaking / zebra)
   ▼
 Submit thread: vkQueueSubmit; then acquire swapchain image, blit viewfinder
   (scaled to the on-screen box, optional 3x magnifier crop), present (FIFO)
   → SurfaceView under the transparent Flutter UI (system compositor)
   ▼
 Recorder thread: wait slot fence → P010 into MediaCodec → HEVC/AV1 Main10 (VBR)
 AAudio → AAC-LC ─┴→ AMediaMuxer (MP4) → MediaStore fd (Movies/Vesper Cine)
```

## Why these choices

| Decision | Why |
|---|---|
| **Binned RAW10 readout** | Full field of view, better SNR, fastest readout (least rolling shutter), the only modes reaching 30–60 fps. The 16:9 mode is read directly (no 4:3 crop waste). |
| **Quad (2x2) debayer + HQ detail** | For 1080p the quad image (2000 px) already has more than one RGB sample per output pixel: no zipper/maze artefacts, no Gr/Gb split. HQ adds the extra luma detail from a full-res green, anti-aliased, as a difference image. |
| **CPU memcpy upload** | Importing RAW10 `AHardwareBuffer`s as Vulkan images crashed the PowerVR gralloc, and importing as a `VkBuffer` requires `AHARDWAREBUFFER_FORMAT_BLOB` (RAW10 isn't). ~15 MB/frame, ~2.5 ms on the camera thread. |
| **Submit/present thread** | `vkQueueSubmit`/`vkQueuePresentKHR` blocked the camera thread for ~46 ms on PowerVR. The camera thread now only records and queues (<1 ms). |
| **SurfaceView viewfinder** | Through a Flutter texture every frame waited for a Flutter frame (85–100 ms gaps). Now presented straight to SurfaceFlinger; Flutter draws the UI on a transparent surface on top and reports the viewfinder box (`setViewfinderRect`). The swapchain is sized to that box (the GPU scales; compositor scaling aliased noise). FIFO so no frame is replaced. |
| **Ping-pong TNR history** | Clean writes image P[parity] and reads P[1−parity] as history; descriptor sets exist per slot per parity. No per-frame history copy. |
| **P010 ByteBuffer encoder input** | We choose BT.2020 NCL matrix, limited range and quantisation; every frame carries its sensor timestamp as PTS. |
| **Apple Log / Rec.2020** | Published, C1-continuous, natively supported in Resolve/Premiere/FCP; covers the sensor's range. |

## GPU performance guard (`VulkanEngine::readTimestamps`)

The frame budget is the camera's frame interval. The guard pauses optional stages when the
smoothed GPU frame time stays > 85% of budget (~0.5 s): first the motion search drops from every
2nd to every 4th frame (stage 3; clean reuses the field, assuming steady motion), then alignment
off (0), then HQ (1), then NR (2); restored in reverse. The alignment stages are skipped when
temporal NR + alignment isn't in use.
It measures what each pause saved; that number is inflated while overloaded, so it is only a
placeholder until a **restore** measures the stage's real cost (`costReliable_`). A paused stage
comes back when `gpu + cost < 85% − 0.5 ms` (~1 s), or by trial when its cost isn't reliable and
the GPU is under 85% − 1.5 ms (~3 s). A stage that overflows again is paused and backs off.
**A frame-rate change resets everything**: all stages on, ~1 s settle, then re-evaluate.
While recording the guard is always active; release builds always run it on AUTO.
`overloaded()` (status `gpuOverloaded`) = over budget with nothing left to pause → UI warns.

**Heat** (`setThermalLevel`, fed once a second by `pollHeat()` in native_bridge from Android's
10 s thermal-headroom forecast, 1.0 = "severe"): level 1 (≥ 0.8) holds — nothing is restored;
level 2 (≥ 0.9, or status moderate without a forecast) pauses the next stage in the same order
every ~15 s; level 3 (status severe) pauses every optional stage at once and the take continues.
The recorder stops a take only at "critical".

Per-pass GPU timestamps on PowerVR are unreliable (they lump into "unpack"); only the frame total
(`gpuFrameMs_`) is meaningful.

## Threads and locks

| Thread | Work | Locks |
|---|---|---|
| Camera reader (NDK) | acquire newest image, metering, `processFrame`, hand slot to recorder | `CameraEngine::frameMutex_` → `gStateMutex` (brief) → `VulkanEngine::frameMutex_` → `encoderMutex_` / `submitMutex_` |
| Submit (VulkanEngine) | queue submit, swapchain acquire/blit/present | `submitMutex_` (queue), `queueMutex_` (all `vkQueue*`) |
| Capture result (NDK) | per-frame metadata ring | `metaMutex_` |
| Dart / FFI | settings, status, start/stop | `CameraEngine::mutex_` **or** `gStateMutex`, never both |
| Recorder | encode, audio, mux, thermal/storage guards | `jobMutex_`, `statusMutex_`, `encoderMutex_` |
| JNI (Surface) | `setViewfinderWindow` | `windowMutex_` only |
| Recovery | reopen camera after a device error | `CameraEngine::mutex_` → `frameMutex_` |

Swapchain destroy / window change / resource reallocation call `flushSubmits()` first, then
`vkQueueWaitIdle` under `queueMutex_`.

## Colour pipeline (DNG 1.6 §6)

* `ColorMatrix1/2` (+ `CalibrationTransform`) and `ForwardMatrix1/2` interpolated by **mired** between the reference illuminants.
* Kelvin/tint: `xy = Planckian(T) + tint·Duv`; `neutral = CM(T)·XYZ(xy)`. Eyedropper: raw patch average → xy↔neutral iteration → Kelvin/tint.
* `wbGains = 1/neutral` (G = 1), applied before highlight clipping.
* `camToRec2020 = k · M(XYZ_D50→Rec.2020, Bradford) · FM · D · CC⁻¹ · diag(neutral)`.
* Optional per-device chart profile (`assets/color_profiles/`, `tools/calibration/`).
* Unit tests: `test/native/color_science_test.cpp`.

## Exposure

`solveCleanExposure` (`iso_analysis.cpp`): base native ISO first, shutter up to 180°, then the
HCG native ISO, then gain (never into digital gain). Native ISOs come from a dark-frame ISO sweep
(`analyzeIsoSweep`). Auto-exposure glides in log space (no jumps).

## Tests

`test/native/run_tests.sh`: colour science; native type-check against NDK stubs
(`test/native/android_stubs/`); the real `VulkanEngine` + shaders on Mesa lavapipe (grey code
value, clipping, rotation, shading, sharpening, HQ detail/moiré/overshoot/noise, TNR ghosting,
hot pixels, ring reuse); calibration tool; focus controller; ISO analysis; sensor calibration
statistics, DNG writer and `tools/calibration/sensor.py` on a simulated sensor with planted faults. lavapipe timings are
CPU-emulated and noisy — never use them as phone performance numbers.
`flutter analyze` and `flutter test` cover the Dart side.
