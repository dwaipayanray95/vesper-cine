#!/usr/bin/env python3
"""End-to-end check of the sensor calibration: test/native/sensor_calib_test.cpp
simulates a sensor with known faults (black offsets, clip at 1008 DN instead of
1023, red shading under-corrected 6% at the corners, hot pixels, a negative HAL
noise offset), runs the phone's statistics code and writes Dark / White sweeps
in the phone's format; sensor.py must find every fault.

Run: python3 tools/calibration/test_sensor.py [dir with VSENSOR_*_synthetic.json]
(without a directory it builds and runs the C++ simulation itself with g++)."""
import io
import json
import math
import os
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sensor  # noqa: E402


def build_synthetic(out_dir):
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    cpp = os.path.join(root, "android", "app", "src", "main", "cpp")
    exe = os.path.join(out_dir, "sensor_test")
    subprocess.run(["g++", "-std=c++20", "-O2", f"-I{cpp}", os.path.join(cpp, "sensor_calib.cpp"),
                    os.path.join(cpp, "dng_writer.cpp"), os.path.join(root, "test", "native", "sensor_calib_test.cpp"),
                    "-o", exe], check=True)
    subprocess.run([exe, out_dir], check=True, stdout=subprocess.DEVNULL)


def main(argv):
    build_dir = tempfile.TemporaryDirectory()  # kept until the end: later checks read the phone-side profile
    d = argv[1] if len(argv) > 1 else build_dir.name
    if len(argv) <= 1:
        build_synthetic(d)
    with tempfile.TemporaryDirectory() as tmp:
        files = [os.path.join(d, f"VSENSOR_{k}_synthetic.json") for k in ("dark", "white")]
        truth = json.load(open(files[1]))["synthTruth"]
        text = io.StringIO()
        r = sensor.report(files, os.path.join(tmp, "out.json"), file=text)
        out = text.getvalue()
        json.load(open(os.path.join(tmp, "out.json")))  # the JSON export is valid

    # A. Black level per site, at ISOs where the noise stays clear of 0 DN.
    for iso in (50, 100, 200, 400):
        got = r["black"][iso]["long"]["black"]
        assert np.allclose(got, truth["black"], atol=0.2), (iso, got, truth["black"])
    assert "[FIX] black level" in out

    # B. Noise model recovered per ISO; the app's floor (O fallback) is flagged.
    for i, iso in enumerate(truth["isos"]):
        n = r["noise"][iso]
        assert abs(n["S"] / truth["S"][i] - 1) < 0.05, (iso, n["S"], truth["S"][i])
        if iso <= 800:
            assert abs(n["O"] / truth["O"][i] - 1) < 0.15, (iso, n["O"], truth["O"][i])
    assert r["isoModel"]["maxErrS"] < 0.05
    for name, v in r["noiseUniformity"].items():
        assert 0.9 < v < 1.1, (name, v)
    assert "[FIX] noise model" in out

    # C. Lens shading: the red under-correction is found, green / blue are fine.
    f = r["shading"]["steps"][0]["fits"]
    assert -0.075 < f["R/G"]["radialCorner"] < -0.025, f["R/G"]
    assert abs(f["B/G"]["radialCorner"]) < 0.015, f["B/G"]
    assert abs(f["G"]["radialCorner"]) < 0.025 and abs(f["G"]["tiltLR"]) < 0.01, f["G"]

    # D. Clip point at 1008 everywhere, flagged against the reported 1023.
    for iso, c in r["clip"].items():
        assert all(s["clipDn"] == truth["clip"] for s in c["sites"]), (iso, c)
    assert "[FIX] sensor clips below the reported white" in out

    # E. Linear sensor: < 0.03 stop per step with the measured black; the
    # camera's black leaves the planted green offset (+0.6 DN).
    lin = r["linearity"]
    assert all(abs(x["errStops"]) < 0.03 for x in lin["rows"] if x["level"] > 0.001), lin["rows"]
    assert abs(lin["offsetReported"] - 0.6) < 0.2, lin["offsetReported"]
    assert "[OK ] linearity" in out

    # F. Hot pixels: all found (3 at the long exposure), repaired in the dark.
    counts = [c for c in r["defects"]["counts"] if "1/33" in c["label"]]
    assert all(c["hot"] == truth["hot"] for c in counts), counts
    assert r["defects"]["levels"][0]["caught"] == 1.0, r["defects"]["levels"][0]

    # Sensor profile for the app (from the dark sweep alone: O measured, S from the camera).
    with tempfile.TemporaryDirectory() as tmp:
        prof_path = os.path.join(tmp, "profile.json")
        rd = sensor.report([files[0]], file=io.StringIO(), profile=prof_path, device="Synthetic", camera="0")
        prof = json.load(open(prof_path))
    assert prof["format"] == "vesper-sensor-profile/1" and prof["device"] == "Synthetic"
    assert not rd["noise"][50]["sMeasured"] and abs(rd["noise"][50]["O"] / truth["O"][0] - 1) < 0.15
    assert prof["defectCount"] == truth["hot"] and len(prof["defects"]) == 2 * truth["hot"], prof["defectCount"]
    # The synthetic camera reports 0.4x the real dark noise: correction factor 2.5
    # (ISOs where the dark noise stays clear of 0 DN), from the tool and from the phone.
    for name, pr in (("sensor.py", prof), ("phone", json.load(open(os.path.join(d, "VSENSOR_dark_synthetic_profile.json"))))):
        f = {e["iso"]: e["factor"] for e in pr["darkNoise"]}
        assert pr["format"] == "vesper-sensor-profile/1" and len(f) == 6, (name, f)
        for iso in (50, 100, 200, 400, 800):
            assert abs(f[iso] / 2.5 - 1) < 0.1, (name, iso, f[iso])
    phone = json.load(open(os.path.join(d, "VSENSOR_dark_synthetic_profile.json")))
    assert {tuple(phone["defects"][i:i + 2]) for i in range(0, len(phone["defects"]), 2)} == \
        {tuple(prof["defects"][i:i + 2]) for i in range(0, len(prof["defects"]), 2)}, "phone and tool hot-pixel maps differ"

    # Helpers.
    assert abs(float(sensor.apple_log(0.18)) - 0.4883) < 2e-4
    rng = np.random.default_rng(2)
    x = np.clip(np.round(30 + 20 * rng.standard_normal(400000)), 0, None)
    mu, sd = sensor.unclamp(float(x.mean()), float(x.var()))
    assert abs(mu - 30) < 0.15 and abs(sd - 20) < 0.3, (mu, sd)
    assert abs(sensor.edge_noise_ratio(4.0, 1e-3, 0.0, 0.02) - 2.0) < 1e-9  # shot noise: sqrt(gain)
    assert abs(sensor.edge_noise_ratio(4.0, 0.0, 1e-6, 0.02) - 4.0) < 1e-9  # read noise: gain
    print("sensor calibration analysis tests passed")


if __name__ == "__main__":
    main(sys.argv)
