# Per-device calibration

Two parts: **sensor calibration** (no chart: noise, black level, clip point,
linearity, lens shading, hot pixels; `sensor.py`) and **colour calibration**
(ColorChecker forward matrix; `calibrate.py`, further below).

# Sensor calibration (no chart needed)

Developer / profile build, Settings › Developer. Run at the frame rate you film at
(the dark sweep's long exposure is 1/fps). If Native ISO Analysis
(Settings › Exposure · Color · Focus) hasn't been run, run it first: the white
sweep's shutter ladder then uses the base native ISO.

| Sweep | Setup | Time | What the phone does |
|---|---|---|---|
| **Sensor Calibration · Dark** | Phone face down on a dark cloth (or lens cap + cloth). No light at all. | ~30–60 s | every ISO (doubling from the minimum) at 1/fps and 1 ms, 4 frames each |
| **Sensor Calibration · White** | Two layers of plain white printer paper flat against the lens, aimed at bright **daylight** (window or sky; lamps flicker). Hold still. | ~1–2 min | meters "half of the raw range" at the base ISO, then a **1-stop shutter ladder** +3 … −8 stops, and 5 levels (+2, 0, −2, −4, −6 stops) at every other ISO, 4 frames each |

Each sweep writes `Download/Vesper Calibration/VSENSOR_<dark|white>_<camera>_<date>.json`
(2–4 MB: per 128x128-px block and CFA site the mean, the temporal noise from
frame-pair differences, the clip histogram and a list of defective pixels; see
`android/app/src/main/cpp/sensor_calib.h`) and one full reference frame
`…_ref.raw10/.json/.dng` (only needed if the statistics leave a question open).

```
python3 sensor.py VSENSOR_dark_*.json VSENSOR_white_*.json [--json numbers.json]
```
prints, in DN of the 10-bit raw data:
- **A. Black level** per CFA site and ISO vs the camera's (dynamic) black level, dark noise, row noise,
  dark current. 18 % grey is only ~21 DN above black with 5.5 stops of headroom, so 1 DN matters.
- **B. Noise model** `variance = S·x + O` fitted per ISO vs SENSOR_NOISE_PROFILE and vs what the
  app's GPU passes use (NR, alignment margin, sharpening), plus a per-ISO model (S ∝ ISO,
  O = a + b·ISO²), electrons/DN, read noise, dynamic range, and whether the raw noise is the same
  everywhere in the frame.
- **C. Lens shading**: the flat field times the camera's shading map (should be 1.00 and neutral
  everywhere), split into a straight gradient (usually uneven light) and a radial residual (map error);
  map gains vs the 4.5 cap of the noise model; edge-vs-centre noise at equal brightness.
- **D. Clip point** per site and ISO vs the reported white level (the GPU flags clipping at ≥ white − 1).
- **E. Linearity / tone**: each 1-stop shutter step should double the raw level; the table shows the
  error in stops and the resulting Apple Log codes, with the measured and with the camera's black level.
- **F. Hot / dead pixels** per ISO and exposure, and how many `clean.comp`'s repair would catch at
  black, grey −3 stops, grey and grey +3 stops.
- **SUMMARY**: one OK / FIX line per item.

**On the phone (since 0.16.0):** a Dark sweep also calibrates the phone itself: its noise table and hot-pixel
map are saved in the app's files (the map grows with each run) and used at once; `…_profile.json` is the copy in
Downloads. Hot pixels differ from phone to phone, so every phone runs its own Dark sweep; the noise table below
is the per-model fallback until it has.

**Shipping a per-model noise table:** `python3 sensor.py VSENSOR_dark_*.json [VSENSOR_white_*.json] --profile
../../assets/sensor_profiles/<phone>_cam<id>.json` writes the per-ISO dark-noise correction (measured / camera
noise floor O; the app multiplies the camera's O by it) and the static hot-pixel map (every pixel flagged in any
dark sweep, pre-correction array coordinates). The app loads the file matching `Build.MODEL` + camera id at start;
the map is applied when Hot Pixel Fix is on. For a shipped asset use `--no-defects` (the app ignores a
shipped map: it would be another phone's hot pixels). Use two or more dark sweeps: the report then shows how much of one
sweep's defects the others' map covers.

`python3 test_sensor.py` checks the chain end to end: `test/native/sensor_calib_test.cpp`
simulates a sensor with planted faults and writes sweeps in the phone's format; `sensor.py` must find them.

**Calibration Frame** (Settings › Developer) saves the next frame as `CAL_….raw10/.json/.dng`. The JSON
(format `vesper-calibration-capture/2`) and the DNG carry the per-channel noise profile, the
analog/digital ISO split (from SENSOR_MAX_ANALOG_SENSITIVITY), the HAL's (Google) AWB neutral
(DNG AsShotNeutral), colour matrices, black/white levels and the lens-shading map (DNG GainMap opcodes).

# Colour calibration (ColorChecker)

Fits a ColorChecker-based ForwardMatrix (white-balanced camera RGB → XYZ D50)
per phone and camera, which replaces the factory DNG matrices. Apple Log
encoding is not touched: calibration happens *before* the log curve.

## You need
- Calibrite/X-Rite ColorChecker Classic (Mini is fine) + a grey card.
- Real light: daylight (shade or overcast, ~5600–6500K) and ideally one
  warm tungsten/halogen source (~2800–3200K). Monitors or paper prints are not valid.
- Python 3 with numpy (`pip install numpy`; `rawpy` only for DNG input).

## 1. Capture (on the phone)
1. Chart fills ~1/3 of the frame, evenly lit, no glare, no shadow. Camera parallel to the chart.
2. WB: MANUAL, set Kelvin roughly to the light (or tap AUTO then switch to MANUAL).
3. Tap **AE**, then make sure the white patch isn't clipped (use zebras, lower ISO/shutter if it is).
4. Settings › **Developer › Calibration Frame → CAPTURE** (debug/profile builds). The `.raw10` + `.json` pair goes to
   `Download/Vesper Calibration/` (and stays in the app's private `files/calibration`).
5. Repeat under a second light if you have one.

## 2. Fit (on a computer)
```
python3 calibrate.py preview CAL_0_5600K_....raw10 --out day.png
# open day.png, note the pixel centres of the 4 corner patches:
#   TL = dark skin (1), TR = bluish green (6), BR = black (24), BL = white (19)
python3 calibrate.py fit CAL_0_5600K_....raw10 --corners x1,y1,x2,y2,x3,y3,x4,y4 \
    --name daylight --cct 5600 --out day_fit.json
python3 calibrate.py fit CAL_0_3000K_....raw10 --corners ... --name tungsten --cct 3000 --out tung_fit.json
python3 calibrate.py profile day_fit.json tung_fit.json --device "Pixel 10" --camera 0 \
    --out ../../assets/color_profiles/pixel10_cam0.json
```
`fit` prints the mean/max ΔE2000 for the fitted matrix vs the factory one. Mean ΔE < 2 is good.

## 3. Ship it
Rebuild the app. On start it loads `assets/color_profiles/*.json`, picks the file whose
`device` equals Android `Build.MODEL` and whose `cameraId` equals the open camera, and
enables it. Settings › **Exposure · Color · Focus › Calibration Profile** switches between FACTORY and CHART PROFILE.

## Adding another phone
1. The phone must expose RAW10 via Camera2 (the app says so on start).
2. Capture + fit exactly as above on that phone; `--device` must match its `Build.MODEL`
   (it's in the capture JSON's `device` field), `--camera` its camera id (also in the JSON).
3. Drop the profile in `assets/color_profiles/`. One file per phone/camera; keep the rest.
   The chart is reusable — keep it in its sleeve out of sunlight and replace after ~2 years.

`python3 test_calibration.py` runs a synthetic end-to-end check of the fitter.
