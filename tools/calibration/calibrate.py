#!/usr/bin/env python3
"""Vesper Cine camera calibration.

  1. preview : write a PNG of the capture (quad resolution) to find the chart.
  2. fit     : sample the 24 patches between four corners you give, fit a
               ForwardMatrix, and report CIEDE2000 before (factory) / after.
  3. profile : combine 1-2 fits (e.g. daylight + tungsten) into the per-device
               colour profile JSON the app loads from assets/color_profiles/.

See tools/calibration/README.md for the full workflow.
"""
import argparse
import json
import sys

import numpy as np

from colorchecker import PATCH_NAMES, load_reference
from fit import fit_forward_matrix, score_matrix
import rawio


def sample_patches(rgb, clip, corners, inset=0.3):
    """corners: TL(dark skin), TR(bluish green), BR(black), BL(white) patch
    centres in quad-image pixels. Returns (24,3) medians and a per-patch clip flag."""
    tl, tr, br, bl = [np.asarray(c, float) for c in corners]
    out, clipped = np.zeros((24, 3)), np.zeros(24, bool)
    for i in range(24):
        r, c = divmod(i, 6)
        u, v = c / 5, r / 3
        centre = (1 - u) * (1 - v) * tl + u * (1 - v) * tr + u * v * br + (1 - u) * v * bl
        pitch = np.linalg.norm(tr - tl) / 5
        half = max(2, int(pitch * (1 - inset) / 2))
        x, y = int(round(centre[0])), int(round(centre[1]))
        patch = rgb[y - half:y + half, x - half:x + half].reshape(-1, 3)
        if patch.size == 0:
            raise SystemExit(f"patch {i + 1} ({PATCH_NAMES[i]}) falls outside the image — check the corners")
        out[i] = np.median(patch, axis=0)
        clipped[i] = clip[y - half:y + half, x - half:x + half].any()
    return out, clipped


def parse_corners(s):
    vals = [float(v) for v in s.replace(";", ",").split(",")]
    if len(vals) != 8:
        raise SystemExit("--corners wants 8 numbers: x1,y1,x2,y2,x3,y3,x4,y4 (TL,TR,BR,BL patch centres)")
    return [vals[i:i + 2] for i in range(0, 8, 2)]


def cmd_preview(a):
    rgb, _, meta = rawio.load(a.input)
    rawio.write_png(a.out, rgb)
    print(f"wrote {a.out} ({rgb.shape[1]}x{rgb.shape[0]}); read patch-centre pixel coordinates from it")


def cmd_fit(a):
    rgb, clip, meta = rawio.load(a.input)
    ref = load_reference(a.reference)
    cam, clipped = sample_patches(rgb, clip, parse_corners(a.corners))
    exclude = [i for i in range(24) if clipped[i]]
    if exclude:
        print("excluding clipped patches:", ", ".join(PATCH_NAMES[i] for i in exclude))
    if cam[21].min() < 0.02:
        print("warning: neutral 5 is very dark (<2% of clip) — expose brighter for a cleaner fit")
    fm, scale, de = fit_forward_matrix(cam, ref, exclude=exclude)

    factory = None
    for key in ("forwardMatrix", "forwardMatrix1"):
        if key in meta and np.asarray(meta[key]).size == 9:
            factory = np.asarray(meta[key], float).reshape(3, 3)
            break
    de_factory = score_matrix(factory, cam, ref) if factory is not None else None

    print(f"\n{'patch':<15}{'factory dE':>12}{'fitted dE':>12}")
    for i in range(24):
        f = f"{de_factory[i]:.2f}" if de_factory is not None else "-"
        print(f"{PATCH_NAMES[i]:<15}{f:>12}{de[i]:>12.2f}{'  (clipped)' if clipped[i] else ''}")
    keep = [i for i in range(24) if i not in exclude]
    if de_factory is not None:
        print(f"\nfactory: mean {de_factory[keep].mean():.2f}  max {de_factory[keep].max():.2f}")
    print(f"fitted : mean {de[keep].mean():.2f}  max {de[keep].max():.2f}")

    result = {
        "name": a.name, "cct": a.cct or meta.get("kelvin"),
        "forwardMatrix": fm.flatten().round(6).tolist(),
        "meanDeltaE": round(float(de[keep].mean()), 3), "maxDeltaE": round(float(de[keep].max()), 3),
        "factoryMeanDeltaE": None if de_factory is None else round(float(de_factory[keep].mean()), 3),
        "source": a.input, "camera": {k: meta.get(k) for k in ("device", "cameraId", "width", "height")},
    }
    if result["cct"] is None:
        print("warning: no CCT known — pass --cct (e.g. 5600 daylight, 3200 tungsten)")
    with open(a.out, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nwrote {a.out}")


def cmd_profile(a):
    fits = []
    for p in a.fits:
        with open(p) as f:
            fits.append(json.load(f))
    if not 1 <= len(fits) <= 2:
        raise SystemExit("give one or two fit files (ideally one warm, one daylight)")
    if any(ft.get("cct") is None for ft in fits):
        raise SystemExit("every fit needs a cct")
    fits.sort(key=lambda ft: ft["cct"])
    profile = {
        "format": "vesper-color-profile/1",
        "device": a.device, "cameraId": a.camera,
        "illuminants": [{"name": ft["name"], "cct": ft["cct"], "forwardMatrix": ft["forwardMatrix"],
                         "meanDeltaE": ft["meanDeltaE"], "maxDeltaE": ft["maxDeltaE"],
                         "factoryMeanDeltaE": ft.get("factoryMeanDeltaE")} for ft in fits],
    }
    with open(a.out, "w") as f:
        json.dump(profile, f, indent=2)
    print(f"wrote {a.out}: {a.device} camera {a.camera}, " +
          ", ".join(f"{i['name']} {i['cct']}K dE {i['meanDeltaE']}" for i in profile["illuminants"]))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("preview"); p.add_argument("input"); p.add_argument("--out", default="preview.png")
    p.set_defaults(func=cmd_preview)
    p = sub.add_parser("fit"); p.add_argument("input")
    p.add_argument("--corners", required=True, help="TL,TR,BR,BL patch centres: x1,y1,...,x4,y4 (preview pixels)")
    p.add_argument("--name", default="daylight"); p.add_argument("--cct", type=float)
    p.add_argument("--reference", help="CSV of 24 L,a,b reference values (default ColorChecker Classic 2014)")
    p.add_argument("--out", default="fit.json"); p.set_defaults(func=cmd_fit)
    p = sub.add_parser("profile"); p.add_argument("fits", nargs="+")
    p.add_argument("--device", required=True, help='Android Build.MODEL, e.g. "Pixel 10"')
    p.add_argument("--camera", default="0"); p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_profile)
    a = ap.parse_args(argv)
    a.func(a)


if __name__ == "__main__":
    sys.exit(main())
