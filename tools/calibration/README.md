# Per-device colour calibration

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
