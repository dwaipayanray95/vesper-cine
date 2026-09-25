# Project RawEdge: Architecture, Pipeline & Technical Specifications

This document outlines the low-level technical architecture, mathematical models, memory layouts, and roadmap for **Project RawEdge (R-Camera)**.

---

## 1. Pipeline Architecture & Data Flow

Project RawEdge bypasses standard mobile computational photography algorithms to operate as a digital cinema camera pipeline:

```
               [ Sony / Samsung Sensor on Google Pixel ]
                                  │
                                  │ Direct 10-bit Raw Bayer stream
                                  ▼
                     [ libcamera2ndk Camera Engine ]
                     • TEMPLATE_MANUAL
                     • NR: OFF, Edge: OFF, Tonemap: CONTRAST_CURVE
                     • AE: OFF, AWB: OFF, AF: OFF
                     • Manual Exposure, Shutter Angle, ISO, WB Gains
                                  │
                                  │ AIMAGE_FORMAT_RAW10
                                  ▼
                        [ AImageReader Buffer ]
                     • Usage: GPU_SAMPLED_IMAGE
                     • Zero-copy AHardwareBuffer acquisition
                                  │
                                  ▼
                   [ Vulkan Compute Shader (mhc_rlog) ]
                     • Workgroup size: 16x16
                     • Stage 1: Dynamic Black Level & White Level Normalization
                     • Stage 2: 5x5 Malvar-He-Cutler Gradient-Corrected Demosaic
                     • Stage 3: White Balance Multipliers (Kelvin / Tint)
                     • Stage 4: Sensor RGB -> CIE XYZ -> Rec.2020 Matrix
                     • Stage 5: Rec.2020 Linear -> R-Log OETF Transfer Curve
                     • Stage 6: Viewfinder Monitoring Filter (LUT / Scopes)
                                  │
                 ┌────────────────┴────────────────┐
                 │                                 │
           (Binding 1)                       (Binding 2)
                 │                                 │
                 ▼                                 ▼
         [ AMediaCodec ]                [ Flutter SurfaceProducer ]
     • 10-bit HEVC (Main10)         • Native ANativeWindow surface
     • Clean R-Log master           • WYSIWYG R-Log / Rec.709 live preview
     • Muxed via AMediaMuxer        • Texture(textureId: id) widget in UI
```

---

## 2. Color Science & Transfer Functions

### 2.1 Sensor Linearization
Raw pixel samples $S_{raw}$ from `AIMAGE_FORMAT_RAW10` are normalized based on dynamic black levels $BL_{cfa}$ and calibrated white level $WL$:
$$S_{linear} = \max\left(0, \frac{S_{raw} - BL_{cfa}}{WL - BL_{cfa}}\right)$$

Where $cfa \in \{R, Gr, Gb, B\}$ corresponds to the sensor's Bayer quadrant.

### 2.2 Malvar-He-Cutler (MHC) 5x5 Demosaicing
Rather than naive bilinear interpolation (which produces zipper artifacts and chromatic aberration), we use the Malvar-He-Cutler 5x5 gradient-corrected algorithm:
- Green at Red/Blue pixels is estimated with a 5x5 cross filter plus Laplacian correction.
- Red/Blue at Blue/Red pixels uses diagonal 5x5 interpolation with Laplacian correction.
- Red/Blue at Green pixels uses horizontal/vertical 5x5 interpolation with directional gradient correction.

### 2.3 White Balance Multipliers
Kelvin color temperature ($2000K - 10000K$) and green-magenta tint ($-50$ to $+50$) are converted to RGB gain factors ($k_R, k_G, k_B$) normalized to $k_G = 1.0$:
$$\begin{bmatrix} R_{wb} \\ G_{wb} \\ B_{wb} \end{bmatrix} = \begin{bmatrix} k_R \cdot R \\ k_G \cdot G \\ k_B \cdot B \end{bmatrix}$$

### 2.4 Color Space Transform (CIE XYZ & Rec.2020)
Sensor values are mapped to standard CIE 1931 XYZ (D65) using the sensor's calibrated color matrix, then transformed into wide-gamut Rec.2020 linear coordinates:
$$\begin{bmatrix} X \\ Y \\ Z \end{bmatrix} = M_{sensor \to XYZ} \cdot \begin{bmatrix} R_{wb} \\ G_{wb} \\ B_{wb} \end{bmatrix}$$

$$\begin{bmatrix} R_{2020} \\ G_{2020} \\ B_{2020} \end{bmatrix} = \max\left(0, M_{XYZ \to Rec.2020} \cdot \begin{bmatrix} X \\ Y \\ Z \end{bmatrix}\right)$$

Where $M_{XYZ \to Rec.2020}$ is:
$$M_{XYZ \to Rec.2020} = \begin{bmatrix} 1.716651 & -0.355671 & -0.253366 \\ -0.666684 & 1.616481 & 0.015769 \\ 0.017640 & -0.042771 & 0.942103 \end{bmatrix}$$

### 2.5 R-Log OETF (Opto-Electronic Transfer Function)
The **R-Log transfer curve** provides a smooth logarithmic compression optimized for the Pixel's ~11.5 stop dynamic range:
- **Cutoff:** $x_{cut} = 0.01$
- **Linear Region ($x < x_{cut}$):**
  $$y = c \cdot x + d$$
- **Logarithmic Region ($x \ge x_{cut}$):**
  $$y = a \cdot \ln(b \cdot x + 1.0) + c$$

Parameters:
- $a = 0.225$
- $b = 5.5555$
- $c = 0.385$
- $d = 0.100$

Key reference points:
- **18% Middle Gray ($x \approx 0.18$):** sits at **0.40 (40% IRE)**.
- **Sensor Clipping ($x = 1.0$):** sits at **0.95 (95% IRE)** with gentle highlight compression.

---

## 3. Viewfinder Monitoring Modes

The compute shader outputs to `outputViewfinderImage` according to `monitoringMode`:

1. **Mode 0: Flat R-Log:** Direct R-Log output for exposure judgment.
2. **Mode 1: Rec.709 Preview LUT:** Filmic S-curve contrast roll-off with Rec.2020 $\to$ Rec.709 color gamut compression.
3. **Mode 2: False Color (IRE Scale):**
   - $\ge 95\%$ IRE: Red (Clipping warning)
   - $85\% - 90\%$ IRE: Yellow (Near-clip highlights)
   - $50\% - 55\%$ IRE: Pink (Optimal Caucasian skin tone reference)
   - $38\% - 42\%$ IRE: Gray (18% Middle Gray reference)
   - $5\% - 10\%$ IRE: Blue (Near-black shadows)
   - $< 5\%$ IRE: Purple (Crushed shadow warning)
4. **Mode 3: Focus Peaking:** Sobel gradient edge detector overlaying high-frequency detail in bright cinema green (`#00FF33`).
5. **Mode 4: Highlight Zebras:** Diagonal alternating stripes on any pixel exceeding $95\%$ IRE ($0.95$).

---

## 4. Problem Analysis & Resolutions

### 4.1 Issue: Viewfinder Bypasses Pipeline (Looks like Standard Video)
- **Root Cause:** `viewfinderWindow_` was wired into `ACaptureSessionOutputContainer` and `ACaptureRequest`. The Camera2 HAL ISP took over rendering on that surface.
- **Fix:** Remove `viewfinderWindow_` from Camera2. Feed only `imageReaderWindow_`. Render to `viewfinderWindow_` directly from the GPU compute pass.

### 4.2 Issue: Flipped / Inverted Viewfinder Orientation
- **Root Cause:** Android rear cameras have a hardware rotation of $90^\circ$ relative to portrait. In landscape orientation, uncorrected coordinate sampling produces an inverted / transposed image.
- **Fix:** Adjust coordinate mapping in the compute shader or apply rotational transform in the Flutter presentation layer.

---

## 5. Development Roadmap

### Phase 1: Viewfinder WYSIWYG & Orientation Fix (Current)
- [x] Ingest RAW10 via `AImageReader`.
- [x] Implement `mhc_rlog.comp` demosaic, R-Log curve, and monitoring filters.
- [x] Implement Flutter `SurfaceProducer` / `Texture` bridge via JNI.
- [ ] Remove `viewfinderWindow_` from Camera2 HAL outputs.
- [ ] Connect Vulkan/render pipeline presentation to `viewfinderWindow_`.
- [ ] Apply orientation compensation for upright landscape preview.

### Phase 2: AMediaCodec 10-Bit Recording Pipeline
- [ ] Set up `AMediaCodec` 10-bit HEVC encoder (`COLOR_FormatSurface`).
- [ ] Direct Vulkan compute output to codec surface.
- [ ] Mux MP4 file via `AMediaMuxer`.
- [ ] Synchronous stereo audio recording via `AAudio` / `Oboe`.

### Phase 3: Gyroflow High-Rate Motion Logging
- [ ] Capture gyro/accel telemetry at 200 Hz via `ASensorManager`.
- [ ] Export motion CSV alongside recorded video for Gyroflow post-stabilization.
