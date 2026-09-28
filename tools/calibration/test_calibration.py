#!/usr/bin/env python3
"""Synthetic end-to-end test: render a ColorChecker into a Vesper RAW10
capture through a known camera, then check the tool recovers it.
Run: python3 tools/calibration/test_calibration.py"""
import json
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import calibrate  # noqa: E402
from colorchecker import COLORCHECKER_2014_LAB_D50, D50_WHITE, delta_e_2000, lab_to_xyz, xyz_to_lab  # noqa: E402

# A plausible camera: its (white-balanced) camera->XYZ D50 matrix. Rows sum to D50.
TRUE_FM = np.array([[0.62, 0.23, 0.11], [0.21, 0.84, -0.05], [0.01, -0.24, 1.05]])
TRUE_FM += ((D50_WHITE - TRUE_FM.sum(axis=1)) / 3)[:, None]
NEUTRAL = np.array([0.52, 1.0, 0.68])  # camera response to the illuminant's white


def render_capture(folder, noise=0.002, seed=1):
    """Chart 6x4 patches, 60 quad px pitch, at (100,80) on a 480x360-quad (960x720 raw) sensor, GRBG."""
    rng = np.random.default_rng(seed)
    xyz = lab_to_xyz(COLORCHECKER_2014_LAB_D50)
    cam_wb = xyz @ np.linalg.inv(TRUE_FM).T          # XYZ -> WB'd camera
    cam = cam_wb * NEUTRAL * 0.9                      # un-white-balance, expose (white patch ~0.86)
    qh, qw = 360, 480
    quad = np.full((qh, qw, 3), 0.05) * NEUTRAL
    for i in range(24):
        r, c = divmod(i, 6)
        x0, y0 = 100 + c * 60 - 25, 80 + r * 60 - 25
        quad[y0:y0 + 50, x0:x0 + 50] = cam[i]
    quad = np.clip(quad + rng.normal(0, noise, quad.shape), 0, 1)
    black, white = 64, 1023
    raw = np.zeros((qh * 2, qw * 2))
    raw[0::2, 0::2] = quad[..., 1]; raw[0::2, 1::2] = quad[..., 0]   # G R
    raw[1::2, 0::2] = quad[..., 2]; raw[1::2, 1::2] = quad[..., 1]   # B G
    dn = np.clip(np.round(black + raw * (white - black)), 0, white).astype(np.uint16)
    h, w = dn.shape
    stride = w * 5 // 4
    packed = np.zeros((h, stride), np.uint8)
    g = dn.reshape(h, w // 4, 4)
    for i in range(4):
        packed[:, i::5][:, : w // 4] = (g[..., i] >> 2).astype(np.uint8)
    lsb = sum(((g[..., i] & 3) << (2 * i)) for i in range(4)).astype(np.uint8)
    packed[:, 4::5][:, : w // 4] = lsb
    base = os.path.join(folder, "cap")
    with open(base + ".raw10", "wb") as f:
        f.write(packed.tobytes())
    factory = TRUE_FM + np.array([[0.05, -0.03, -0.02], [0.02, -0.04, 0.02], [0.0, 0.06, -0.06]])  # a worse matrix
    with open(base + ".json", "w") as f:
        json.dump({"width": w, "height": h, "rowStride": stride, "cfa": 1, "blackLevel": [black] * 4,
                   "whiteLevel": white, "kelvin": 5600, "forwardMatrix": factory.flatten().tolist(),
                   "device": "Test Phone", "cameraId": "0"}, f)
    return base + ".json"


def main():
    with tempfile.TemporaryDirectory() as d:
        cap = render_capture(d)
        out = os.path.join(d, "fit.json")
        corners = "100,80,400,80,400,260,100,260"
        calibrate.main(["preview", cap, "--out", os.path.join(d, "p.png")])
        calibrate.main(["fit", cap, "--corners", corners, "--out", out])
        fit = json.load(open(out))
        fm = np.asarray(fit["forwardMatrix"]).reshape(3, 3)
        err = np.abs(fm - TRUE_FM).max()
        assert fit["meanDeltaE"] < 0.6, fit["meanDeltaE"]
        assert fit["factoryMeanDeltaE"] > fit["meanDeltaE"] * 2, fit
        assert err < 0.02, (err, fm)
        assert np.allclose(fm.sum(axis=1), D50_WHITE, atol=1e-6)
        prof = os.path.join(d, "profile.json")
        calibrate.main(["profile", out, "--device", "Test Phone", "--camera", "0", "--out", prof])
        p = json.load(open(prof))
        assert p["format"] == "vesper-color-profile/1" and len(p["illuminants"]) == 1
    # CIEDE2000 against the published Sharma et al. test pair.
    de = delta_e_2000(np.array([50.0, 2.6772, -79.7751]), np.array([50.0, 0.0, -82.7485]))
    assert abs(de - 2.0425) < 1e-3, de
    assert np.allclose(xyz_to_lab(lab_to_xyz(COLORCHECKER_2014_LAB_D50)), COLORCHECKER_2014_LAB_D50, atol=1e-6)
    print("calibration tool tests passed")


if __name__ == "__main__":
    main()
