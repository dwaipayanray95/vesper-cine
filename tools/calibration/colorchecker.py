"""Reference data and colour maths for chart-based camera calibration.

Reference: X-Rite / Calibrite ColorChecker Classic (formulation after Nov 2014),
CIE L*a*b* under D50 / 2 deg, as published by X-Rite. Other charts (e.g.
Datacolor SpyderCheckr 24) ship their own values: pass them with
`--reference my_chart.csv` (24 rows: name,L,a,b in the same patch order).
"""
import csv
import numpy as np

PATCH_NAMES = [
    "dark skin", "light skin", "blue sky", "foliage", "blue flower", "bluish green",
    "orange", "purplish blue", "moderate red", "purple", "yellow green", "orange yellow",
    "blue", "green", "red", "yellow", "magenta", "cyan",
    "white 9.5", "neutral 8", "neutral 6.5", "neutral 5", "neutral 3.5", "black 2",
]

COLORCHECKER_2014_LAB_D50 = np.array([
    [37.54, 14.37, 14.92], [64.66, 19.27, 17.50], [49.32, -3.82, -22.54],
    [43.46, -12.74, 22.72], [54.94, 9.61, -24.79], [70.48, -32.26, -0.37],
    [62.73, 35.83, 56.50], [39.43, 10.75, -45.17], [50.57, 48.64, 16.67],
    [30.10, 22.54, -20.87], [71.77, -24.13, 58.19], [71.51, 18.24, 67.37],
    [28.37, 15.42, -49.80], [54.38, -39.72, 32.27], [42.43, 51.05, 28.62],
    [81.80, 2.67, 80.41], [50.63, 51.28, -14.12], [49.57, -29.71, -28.32],
    [95.19, -1.03, 2.93], [81.29, -0.57, 0.44], [66.89, -0.75, -0.06],
    [50.76, -0.13, 0.14], [35.63, -0.46, -0.48], [20.64, 0.07, -0.46],
])

NEUTRAL_PATCHES = [19, 20, 21, 22]  # neutral 8 .. neutral 3.5 (white may clip, black is noisy)
WHITE_PATCH, GREY_PATCH = 18, 21

D50_WHITE = np.array([0.96422, 1.0, 0.82521])


def load_reference(path=None):
    if path is None:
        return COLORCHECKER_2014_LAB_D50.copy()
    rows = []
    with open(path, newline="") as f:
        for r in csv.reader(f):
            if not r or r[0].startswith("#"):
                continue
            rows.append([float(v) for v in r[-3:]])
    ref = np.array(rows)
    if ref.shape != (24, 3):
        raise ValueError(f"{path}: expected 24 rows of L,a,b, got {ref.shape}")
    return ref


def lab_to_xyz(lab, white=D50_WHITE):
    lab = np.asarray(lab, dtype=float)
    fy = (lab[..., 0] + 16) / 116
    fx = fy + lab[..., 1] / 500
    fz = fy - lab[..., 2] / 200
    eps, kappa = 216 / 24389, 24389 / 27

    def inv(f):
        return np.where(f ** 3 > eps, f ** 3, (116 * f - 16) / kappa)

    yr = np.where(lab[..., 0] > kappa * eps, fy ** 3, lab[..., 0] / kappa)
    return np.stack([inv(fx) * white[0], yr * white[1], inv(fz) * white[2]], axis=-1)


def xyz_to_lab(xyz, white=D50_WHITE):
    xyz = np.asarray(xyz, dtype=float) / white
    eps, kappa = 216 / 24389, 24389 / 27
    f = np.where(xyz > eps, np.cbrt(np.maximum(xyz, 0)), (kappa * xyz + 16) / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], axis=-1)


def delta_e_2000(lab1, lab2):
    """CIEDE2000 (Sharma, Wu & Dalal 2005), vectorised."""
    L1, a1, b1 = np.moveaxis(np.asarray(lab1, float), -1, 0)
    L2, a2, b2 = np.moveaxis(np.asarray(lab2, float), -1, 0)
    C1, C2 = np.hypot(a1, b1), np.hypot(a2, b2)
    Cb = (C1 + C2) / 2
    G = 0.5 * (1 - np.sqrt(Cb ** 7 / (Cb ** 7 + 25.0 ** 7)))
    a1p, a2p = (1 + G) * a1, (1 + G) * a2
    C1p, C2p = np.hypot(a1p, b1), np.hypot(a2p, b2)
    h1p = np.degrees(np.arctan2(b1, a1p)) % 360
    h2p = np.degrees(np.arctan2(b2, a2p)) % 360
    dLp, dCp = L2 - L1, C2p - C1p
    dh = h2p - h1p
    dh = np.where(C1p * C2p == 0, 0, np.where(dh > 180, dh - 360, np.where(dh < -180, dh + 360, dh)))
    dHp = 2 * np.sqrt(C1p * C2p) * np.sin(np.radians(dh / 2))
    Lbp, Cbp = (L1 + L2) / 2, (C1p + C2p) / 2
    hs = h1p + h2p
    hbp = np.where(C1p * C2p == 0, hs,
                   np.where(np.abs(h1p - h2p) <= 180, hs / 2, np.where(hs < 360, (hs + 360) / 2, (hs - 360) / 2)))
    T = (1 - 0.17 * np.cos(np.radians(hbp - 30)) + 0.24 * np.cos(np.radians(2 * hbp))
         + 0.32 * np.cos(np.radians(3 * hbp + 6)) - 0.20 * np.cos(np.radians(4 * hbp - 63)))
    dtheta = 30 * np.exp(-(((hbp - 275) / 25) ** 2))
    Rc = 2 * np.sqrt(Cbp ** 7 / (Cbp ** 7 + 25.0 ** 7))
    Sl = 1 + 0.015 * (Lbp - 50) ** 2 / np.sqrt(20 + (Lbp - 50) ** 2)
    Sc, Sh = 1 + 0.045 * Cbp, 1 + 0.015 * Cbp * T
    Rt = -np.sin(np.radians(2 * dtheta)) * Rc
    return np.sqrt((dLp / Sl) ** 2 + (dCp / Sc) ** 2 + (dHp / Sh) ** 2 + Rt * (dCp / Sc) * (dHp / Sh))
