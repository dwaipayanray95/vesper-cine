#!/usr/bin/env python3
"""Sensor calibration (no colour chart): noise model, black level, clip point,
linearity, lens shading and defective pixels, from the phone's sweeps.

On the phone (developer build): Settings > Developer > Sensor Calibration:
  DARK  - lens covered; every ISO at 1/fps and 1 ms, 4 frames each.
  WHITE - white paper flat over the lens, aimed at daylight; a 1-stop shutter
          ladder (+3 .. -8 stops around "half of the raw range") at the base
          ISO, and 5 levels at every other ISO.
Each sweep writes Download/Vesper Calibration/VSENSOR_<kind>_<date>.json (a
few MB of per-block statistics computed on the phone, see
android/app/src/main/cpp/sensor_calib.h) plus one reference frame
(..._ref.raw10/.json/.dng).

  python3 sensor.py VSENSOR_dark_*.json VSENSOR_white_*.json [--json out.json]

Everything is in raw DN unless stated; "normalised" = (DN - black) / (white - black),
the unit of SENSOR_NOISE_PROFILE (variance = S * x + O) and of the GPU noise model.
"""
import argparse
import json
import math
import sys

import numpy as np

HEADROOM_STOPS = 5.5          # app default: stops from 18% grey to sensor clip
GREY = 2.0 ** -HEADROOM_STOPS  # 18% grey as a fraction of the raw range (0.0221)
SITES = ("R", "Gr", "Gb", "B")


# --- Apple Log (render.comp, Apple Log Profile White Paper 2023) -------------
def apple_log(x):
    r0, rt, c = -0.05641088, 0.01, 47.28711236
    beta, gamma, delta = 0.00964052, 0.08550479, 0.69336945
    x = np.asarray(x, float)
    return np.where(x >= rt, gamma * np.log2(np.maximum(x, rt) + beta) + delta,
                    np.where(x >= r0, c * (x - r0) ** 2, 0.0))


def log_code(raw_fraction):
    """10-bit limited-range code of a neutral at `raw_fraction` of the raw range (app's exposure scaling)."""
    lin = 0.18 * 2.0 ** HEADROOM_STOPS * np.asarray(raw_fraction, float)
    return 64.0 + 876.0 * apple_log(lin)


# --- Loading -----------------------------------------------------------------
def site_parity(cfa):
    """logical site -> (px, py) raw parity."""
    out = {}
    for py in range(2):
        for px in range(2):
            bit = (py << 1) | px
            s = [bit, bit ^ 1, bit ^ 2, 3 - bit][cfa]
            out[s] = (px, py)
    return out


class Step:
    def __init__(self, d, report):
        self.d = d
        self.report = report
        self.iso = int(d["iso"])
        self.exposure = float(d["exposureNs"])
        self.stop = int(d.get("stop", 0))
        self.ladder = bool(d.get("ladder", False))
        self.black_reported = np.asarray(d["blackLevel"], float)
        self.white = float(d["whiteLevel"])
        self.range = self.white - self.black_reported
        st = d["stats"]
        self.st = st
        self.by, self.bx, self.block = st["blocksY"], st["blocksX"], st["block"]
        shape = (4, self.by, self.bx)
        self.mean = np.asarray(st["mean"], float).reshape(shape)
        self.tvar = np.asarray(st["tvar"], float).reshape(shape)
        self.svar = np.asarray(st["svar"], float).reshape(shape)
        self.clipped = np.asarray(st["clipped"], float).reshape(shape)
        self.top_hist = np.asarray(st["topHist"], float)
        self.max_dn = np.asarray(st["maxDn"], float)
        self.row_var = np.asarray(st["rowVar"], float)
        self.col_var = np.asarray(st["colVar"], float)
        self.row_samples = np.asarray(st["rowSamples"], float)
        self.col_samples = np.asarray(st["colSamples"], float)
        dl = np.asarray(st["defects"], float).reshape(-1, 6)
        self.defects = dl
        self.defect_count = st["defectCount"]
        self.hot_count, self.dead_count = st["hotCount"], st["deadCount"]
        self.hal_s = float(d.get("noiseS", 0))
        self.hal_o = float(d.get("noiseO", 0))
        self.hal_sites = np.asarray(d.get("noiseProfileSites", [0] * 8), float).reshape(4, 2)
        self.eng_s = float(d.get("engineNoiseS", self.hal_s))
        self.eng_o = float(d.get("engineNoiseO", self.hal_o))
        self.analog_iso = float(d.get("analogIso", self.iso))
        self.digital = float(d.get("digitalGain", 1.0))
        self.shading = int(d.get("shading", -1))
        # Block centres in raw px and in frame fractions.
        cx = st["x0"] + (np.arange(self.bx) + 0.5) * self.block
        cy = st["y0"] + (np.arange(self.by) + 0.5) * self.block
        self.cx, self.cy = np.meshgrid(cx, cy)
        self.fx, self.fy = self.cx / report["width"], self.cy / report["height"]
        # Radius normalised to the half width (aspect kept), centre = 0.
        self.u = (self.cx - report["width"] / 2) / (report["width"] / 2)
        self.v = (self.cy - report["height"] / 2) / (report["width"] / 2)

    def label(self):
        return f"ISO {self.iso:5d} {shutter(self.exposure):>9s}"


def shutter(ns):
    s = ns * 1e-9
    return f"1/{1 / s:.0f}" if s < 1 else f"{s:.1f}s"


def load(paths):
    dark, white, reports = [], [], []
    for p in paths:
        with open(p) as f:
            r = json.load(f)
        if r.get("format") != "vesper-sensor-calibration/1":
            raise SystemExit(f"{p}: not a Vesper sensor sweep (format {r.get('format')})")
        reports.append(r)
        steps = [Step(s, r) for s in r["steps"]]
        (dark if r["kind"] == "dark" else white).extend(steps)
    return dark, white, reports


# --- Helpers -----------------------------------------------------------------
def _phi(z):
    return math.exp(-0.5 * z * z) / math.sqrt(2 * math.pi)


def _Phi(z):
    return 0.5 * (1 + math.erf(z / math.sqrt(2)))


def unclamp(m, var):
    """(mu, sigma) of X ~ N(mu, sigma) from the mean and variance of max(X, 0):
    the sensor can't output < 0 DN, which lifts the dark mean (and lowers its
    variance) once the noise reaches black at high ISO."""
    sd = math.sqrt(max(var, 1e-12))
    if m / sd > 5:
        return m, sd

    def moments(mu, sigma):
        z = mu / sigma
        e1 = mu * _Phi(z) + sigma * _phi(z)
        e2 = (mu * mu + sigma * sigma) * _Phi(z) + mu * sigma * _phi(z)
        return e1, e2 - e1 * e1

    mu, sigma = m, sd
    for _ in range(30):
        lo, hi = m - 6 * sigma, m
        for _ in range(50):
            mid = 0.5 * (lo + hi)
            if moments(mid, sigma)[0] > m:
                hi = mid
            else:
                lo = mid
        mu = 0.5 * (lo + hi)
        v = moments(mu, sigma)[1]
        sigma *= math.sqrt(max(var, 1e-12) / max(v, 1e-12))
    return mu, sigma


def regions(step):
    fx, fy = step.fx, step.fy
    return {
        "centre": (np.abs(fx - 0.5) < 0.12) & (np.abs(fy - 0.5) < 0.12),
        "left edge": (fx < 0.12) & (np.abs(fy - 0.5) < 0.2),
        "right edge": (fx > 0.88) & (np.abs(fy - 0.5) < 0.2),
        "top edge": (fy < 0.15) & (np.abs(fx - 0.5) < 0.2),
        "bottom edge": (fy > 0.85) & (np.abs(fx - 0.5) < 0.2),
        "corners": ((fx < 0.12) | (fx > 0.88)) & ((fy < 0.15) | (fy > 0.85)),
    }


def shading_gains(step):
    """HAL lens-shading gain at every block centre, per logical site: (4, by, bx)."""
    maps = step.report.get("shadingMaps", [])
    if step.shading < 0 or step.shading >= len(maps):
        return np.ones((4, step.by, step.bx))
    m = maps[step.shading]
    cols, rows = m["cols"], m["rows"]
    grid = np.asarray(m["map"], float).reshape(rows, cols, 4)
    sm = step.report["sensorMap"]
    aw, ah = step.report["arrayWidth"] or step.report["width"], step.report["arrayHeight"] or step.report["height"]
    gx = np.clip((step.cx * sm[0] + sm[2]) / aw * (cols - 1), 0, cols - 1)
    gy = np.clip((step.cy * sm[1] + sm[3]) / ah * (rows - 1), 0, rows - 1)
    x0, y0 = np.minimum(gx.astype(int), cols - 2), np.minimum(gy.astype(int), rows - 2)
    fx, fy = gx - x0, gy - y0
    par = site_parity(step.report["cfa"])
    out = np.empty((4, step.by, step.bx))
    for s in range(4):
        ch = 0 if s == 0 else 3 if s == 3 else (1 if par[s][1] == 0 else 2)  # greens by row parity
        g = grid[..., ch]
        top = g[y0, x0] * (1 - fx) + g[y0, x0 + 1] * fx
        bot = g[y0 + 1, x0] * (1 - fx) + g[y0 + 1, x0 + 1] * fx
        out[s] = top * (1 - fy) + bot * fy
    return out


# --- A. Black level ----------------------------------------------------------
def analyse_black(dark):
    """Per ISO: measured black per site (long + short exposure), dark noise, banding."""
    out = {}
    for iso in sorted({s.iso for s in dark}):
        steps = sorted([s for s in dark if s.iso == iso], key=lambda s: -s.exposure)
        res = {"iso": iso, "reported": steps[0].black_reported.tolist(), "analogIso": steps[0].analog_iso,
               "digitalGain": steps[0].digital}
        for tag, s in (("long", steps[0]), ("short", steps[-1])):
            tv = np.median(s.tvar.reshape(4, -1), axis=1)
            fixed = [unclamp(float(np.median(s.mean[k])), float(tv[k])) for k in range(4)]
            meas = [f[0] for f in fixed]
            sig = np.array([f[1] for f in fixed])
            res[tag] = {"exposureNs": s.exposure, "black": meas, "sigma": sig.tolist(),
                        "spread": float(np.percentile(s.mean[1:3], 99) - np.percentile(s.mean[1:3], 1)),
                        "rowNoise": np.sqrt(np.maximum(s.row_var - s.tvar.reshape(4, -1).mean(1) / s.row_samples, 0)).tolist(),
                        "colNoise": np.sqrt(np.maximum(s.col_var - s.tvar.reshape(4, -1).mean(1) / s.col_samples, 0)).tolist(),
                        "hot": int(s.hot_count), "dead": int(s.dead_count)}
        res["offset"] = (np.asarray(res["long"]["black"]) - np.asarray(res["reported"])).tolist()
        res["darkCurrent"] = (np.asarray(res["long"]["black"]) - np.asarray(res["short"]["black"])).tolist()
        out[iso] = res
    return out


def black_for(black, iso, fallback):
    """Measured black per site at this ISO (nearest measured ISO in log space)."""
    if not black:
        return np.asarray(fallback, float)
    isos = np.array(sorted(black))
    near = isos[np.argmin(np.abs(np.log(isos / iso)))]
    return np.asarray(black[near]["long"]["black"], float)


# --- B. Noise model ----------------------------------------------------------
def noise_points(white, dark, black):
    """(iso, site, x, var, step, region-mask-index) points in normalised units."""
    pts = []
    for s in white + dark:
        b = black_for(black, s.iso, s.black_reported)
        rng = s.white - b
        for k in range(4):
            x = (s.mean[k] - b[k]) / rng[k]
            v = s.tvar[k] / rng[k] ** 2
            ok = (s.clipped[k] == 0) & (x < 0.85) & (v > 0)
            for xi, vi in zip(x[ok], v[ok]):
                pts.append((s.iso, k, xi, vi))
    return np.asarray(pts, float).reshape(-1, 4)


def fit_sv(x, v):
    """var = S x + O, weighted for relative error (each point's sd ~ its variance)."""
    w = 1.0 / np.maximum(v, 1e-12)
    a = np.stack([x * w, w], axis=1)
    sol, *_ = np.linalg.lstsq(a, v * w, rcond=None)
    return float(sol[0]), float(sol[1])


def analyse_noise(white, dark, black):
    pts = noise_points(white, dark, black)
    out = {}
    for iso in sorted(set(pts[:, 0].astype(int))):
        p = pts[pts[:, 0] == iso]
        ref = next((s for s in white + dark if s.iso == iso), None)
        s_measured = bool(p[:, 2].max() > 0.01)
        if s_measured:
            S, O = fit_sv(p[:, 2], p[:, 3])
            per_site = [fit_sv(p[p[:, 1] == k, 2], p[p[:, 1] == k, 3]) if np.sum(p[:, 1] == k) > 3 else (S, O)
                        for k in range(4)]
        else:
            # Dark frames only: the noise floor O is measured, the slope S needs
            # lit frames (White sweep), so the camera's S stands in.
            S, O = ref.hal_s, float(np.median(p[:, 3]))
            per_site = [(S, float(np.median(p[p[:, 1] == k, 3]))) for k in range(4)]
        res = {"iso": iso, "S": S, "O": O, "sMeasured": s_measured, "perSite": per_site, "points": int(len(p)),
               "xMax": float(p[:, 2].max()), "halS": ref.hal_s, "halO": ref.hal_o, "engS": ref.eng_s, "engO": ref.eng_o,
               "halSites": ref.hal_sites.tolist()}
        res["fitError"] = float(np.median(np.abs(p[:, 3] / np.maximum(S * p[:, 2] + O, 1e-15) - 1)))
        out[iso] = res
    return out


def iso_model(noise):
    """S(iso) = a * iso, O(iso) = b + c * iso^2 (read noise before / after the amplifier)."""
    isos = np.array(sorted(noise), float)
    if len(isos) < 2 or not all(n["sMeasured"] for n in noise.values()):
        return None
    S = np.array([noise[i]["S"] for i in isos.astype(int)])
    O = np.array([noise[i]["O"] for i in isos.astype(int)])
    a = float(np.sum(S * isos) / np.sum(isos * isos))
    A = np.stack([np.ones_like(isos), isos ** 2], axis=1) / O[:, None]
    bc, *_ = np.linalg.lstsq(A, np.ones_like(isos), rcond=None)
    return {"a": a, "b": float(bc[0]), "c": float(bc[1]),
            "maxErrS": float(np.max(np.abs(a * isos / S - 1))),
            "maxErrO": float(np.max(np.abs((bc[0] + bc[1] * isos ** 2) / O - 1)))}


def noise_uniformity(white, noise, black):
    """Measured / modelled noise per frame region (1.0 = the sensor is equally noisy everywhere)."""
    acc = {}
    for s in white:
        n = noise.get(s.iso)
        if not n:
            continue
        b = black_for(black, s.iso, s.black_reported)
        reg = regions(s)
        for k in (1, 2):
            rng = s.white - b[k]
            x = (s.mean[k] - b[k]) / rng
            ok = (s.clipped[k] == 0) & (x > 0.01) & (x < 0.85)
            r = (s.tvar[k] / rng ** 2) / (n["S"] * x + n["O"])
            for name, m in reg.items():
                acc.setdefault(name, []).extend(r[ok & m].tolist())
    return {k: float(np.median(v)) for k, v in acc.items() if v}


# --- C. Lens shading ---------------------------------------------------------
def analyse_shading(white, black):
    """Flat field x HAL map: what's left should be flat and neutral."""
    cands = [s for s in white if s.ladder and s.stop in (0, -1)] or [s for s in white if s.stop == 0]
    if not cands:
        return None
    out = {"steps": []}
    for s in cands:
        b = black_for(black, s.iso, s.black_reported)
        gain = shading_gains(s)
        sig = (s.mean - b[:, None, None])
        if np.any(s.clipped > 0) or np.nanmax(sig[1] / (s.white - b[1])) > 0.9:
            continue
        corr = sig * gain
        reg = regions(s)
        c = reg["centre"]
        norm = np.array([np.median(corr[k][c]) for k in range(4)])
        rel = corr / norm[:, None, None]
        g = 0.5 * (rel[1] + rel[2])
        rg, bg = rel[0] / g, rel[3] / g
        u, v = s.u.ravel(), s.v.ravel()
        r2 = u * u + v * v
        A = np.stack([np.ones_like(u), u, v, r2, r2 * r2], axis=1)
        rcorner = float(r2.max())
        fits = {}
        for name, z in (("G", g), ("R/G", rg), ("B/G", bg)):
            co, *_ = np.linalg.lstsq(A, z.ravel(), rcond=None)
            fits[name] = {"tiltLR": float(2 * co[1]), "tiltTB": float(2 * co[2]),
                          "radialCorner": float((co[3] * rcorner + co[4] * rcorner ** 2) / (co[0] + 1e-12)),
                          "coef": co.tolist()}
        raw_rel = sig / np.array([np.median(sig[k][c]) for k in range(4)])[:, None, None]
        res = {"label": s.label(), "regions": {}, "fits": fits, "rotation": s.report.get("rotation", 0),
               "mapMax": [float(gain[k].max()) for k in range(4)],
               "engineClamp": 4.5}
        for name, m in reg.items():
            if not np.any(m):
                continue
            res["regions"][name] = {
                "vignettingStops": float(np.log2(np.median(0.5 * (raw_rel[1] + raw_rel[2])[m]))),
                "gainG": float(np.median(0.5 * (gain[1] + gain[2])[m])),
                "gainR": float(np.median(gain[0][m])), "gainB": float(np.median(gain[3][m])),
                "G": float(np.median(g[m])), "R/G": float(np.median(rg[m])), "B/G": float(np.median(bg[m])),
                "shadeGainEngine": float(np.median(np.clip(gain.mean(0), 1, 4.5)[m])),
                "shadeGainTrue": float(np.median(gain.mean(0)[m])),
            }
        out["steps"].append(res)
    return out if out["steps"] else None


def edge_noise_ratio(g, S, O, y):
    """Noise at equal output brightness y (normalised), lens-shading gain g vs centre (g = 1)."""
    return math.sqrt(max(g * S * y + g * g * O, 1e-30) / max(S * y + O, 1e-30))


# --- D. Clip point -----------------------------------------------------------
def analyse_clip(white):
    out = {}
    for iso in sorted({s.iso for s in white}):
        over = [s for s in white if s.iso == iso and s.stop >= 2]
        if not over:
            continue
        hist = sum(s.top_hist for s in over)              # (4, 128) codes 896..1023
        max_dn = np.max([s.max_dn for s in over], axis=0)
        total = sum(float(s.mean.size / 4 * s.block * s.block / 4 * s.st["frames"]) for s in over)
        sites = []
        for k in range(4):
            h = hist[k]
            peak = int(np.argmax(h))
            frac = float(h[peak] / total)
            sites.append({"clipDn": 896 + peak if frac > 0.002 else None, "pileUp": frac, "maxDn": int(max_dn[k])})
        white_level = over[0].white
        out[iso] = {"sites": sites, "white": white_level}
    return out


# --- E. Linearity (exposure ladder) ------------------------------------------
def analyse_linearity(white, black):
    lad = sorted([s for s in white if s.ladder], key=lambda s: -s.exposure)
    if len(lad) < 4:
        return None
    iso = lad[0].iso
    b_meas = black_for(black, iso, lad[0].black_reported)
    rows = []
    for s in lad:
        c = regions(s)["centre"]
        ok = c & np.all(s.clipped == 0, axis=0)
        if not np.any(ok):
            rows.append({"step": s, "clipped": True})
            continue
        g_meas = float(np.mean([np.median(s.mean[k][ok]) - b_meas[k] for k in (1, 2)]))
        g_rep = float(np.mean([np.median(s.mean[k][ok]) - s.black_reported[k] for k in (1, 2)]))
        rows.append({"step": s, "clipped": False, "meas": g_meas, "rep": g_rep, "range": s.white - b_meas[1]})
    good = [r for r in rows if not r["clipped"] and 0.03 < r["meas"] / r["range"] < 0.6]
    if len(good) < 2:
        return None
    t = np.array([r["step"].exposure for r in good])
    m = np.array([r["meas"] for r in good])
    slope = float(np.sum(m * t) / np.sum(t * t))
    # Offset that the reported black leaves: fit rep = a t + c over all unclipped steps.
    allr = [r for r in rows if not r["clipped"] and r["rep"] / r["range"] < 0.6]
    T = np.array([r["step"].exposure for r in allr])
    R = np.array([r["rep"] for r in allr])
    A = np.stack([T, np.ones_like(T)], axis=1)
    (a_rep, c_rep), *_ = np.linalg.lstsq(A, R, rcond=None)
    for r in rows:
        if r["clipped"]:
            continue
        ideal = slope * r["step"].exposure
        r["dev"] = r["meas"] / ideal - 1
        r["devRep"] = r["rep"] / ideal - 1
        r["frac"] = r["meas"] / r["range"]
    return {"iso": iso, "rows": rows, "slope": slope, "offsetReported": float(c_rep)}


# --- F. Defects vs the clean.comp hot-pixel repair ---------------------------
def clean_catch_rate(excess, is_green, x, S_true, O_true, S_eng, O_eng, clip=1.0, trials=48, seed=1):
    """Fraction of trials in which clean.comp's rule repairs a defect of
    `excess` (normalised) at raw level x, per defect. Mirrors clean.comp:
    v > max(8 neighbours) + 4 * model sigma at the neighbours' mean (+ G
    factor 0.5 on variance) + 0.25 * (max - min); the quad G is the mean of
    two photosites, and the defective photosite can't exceed the sensor clip.
    (Until 0.15.1: 6 sigma at v itself, which caught ~12% on the Pixel 10.)"""
    rng = np.random.default_rng(seed)
    excess = np.asarray(excess, float)
    g = np.asarray(is_green, bool)
    n = len(excess)
    px_sd = math.sqrt(max(S_true * x + O_true, 1e-15))
    sd = px_sd * np.where(g, math.sqrt(0.5), 1.0)
    nb = x + rng.standard_normal((trials, n, 8)) * sd[None, :, None]
    bad = np.clip(x + excess[None, :] + rng.standard_normal((trials, n)) * px_sd, 0, clip)
    good = x + rng.standard_normal((trials, n)) * px_sd
    v = np.where(g[None, :], 0.5 * (bad + good), bad)
    e_q = np.where(g, 0.5, 1.0) * (np.clip(x + excess, 0, clip) - x)
    hi, lo = nb.max(axis=2), nb.min(axis=2)
    model_var = np.maximum(S_eng * np.maximum(nb.mean(axis=2), 0) + O_eng, 1e-7) * np.where(g, 0.5, 1.0)[None, :]
    t = 4 * np.sqrt(model_var) + 0.25 * (hi - lo)
    hot = e_q >= 0
    caught = np.where(hot[None, :], v > hi + t, v < lo - t)
    return caught.mean(axis=0), np.abs(e_q) / sd  # catch rate, excess in noise sigmas


def analyse_defects(dark, noise, black, clip_dn=None):
    if not dark:
        return None
    worst = max(dark, key=lambda s: (s.iso, s.exposure))
    n = noise.get(worst.iso)
    out = {"counts": [], "worst": worst.label()}
    for s in sorted(dark, key=lambda s: (s.iso, -s.exposure)):
        out["counts"].append({"label": s.label(), "hot": s.hot_count, "dead": s.dead_count})
    d = worst.defects
    if len(d) == 0 or not n:
        out["levels"] = []
        return out
    b = black_for(black, worst.iso, worst.black_reported)
    site = d[:, 2].astype(int)
    excess = d[:, 4] / (worst.white - b[site])
    is_green = (site == 1) | (site == 2)
    levels = []
    clip = min(1.0, (clip_dn - b[1]) / (worst.white - b[1])) if clip_dn else 1.0
    for name, x in (("black", 0.0), ("grey -3 stops", GREY / 8), ("18% grey", GREY), ("grey +3 stops", GREY * 8)):
        rate, sigmas = clean_catch_rate(excess, is_green, x, n["S"], n["O"], n["engS"], n["engO"], clip)
        visible = sigmas > 3
        levels.append({"level": name, "x": x, "caught": float(np.mean(rate > 0.5)),
                       "visibleMissed": int(np.sum(visible & (rate <= 0.5))), "visible": int(np.sum(visible))})
    out["levels"] = levels
    out["listed"] = int(len(d))
    out["excessDn"] = [float(np.percentile(np.abs(d[:, 4]), q)) for q in (50, 90, 99)] if len(d) else []
    return out


def defect_maps(dark_reports):
    """Per dark sweep: every pixel flagged in any step, in pre-correction array coordinates."""
    maps = []
    for r in dark_reports:
        sm = r.get("sensorMap", [1, 1, 0, 0])
        m = set()
        for s in r["steps"]:
            d = np.asarray(s["stats"]["defects"], float).reshape(-1, 6)
            for x, y in d[:, :2]:
                m.add((int(round(x * sm[0] + sm[2])), int(round(y * sm[1] + sm[3]))))
        maps.append(m)
    return maps


def map_crossval(dark_reports):
    """Static map from the other sweeps vs this sweep's worst step: share of its defects covered."""
    maps = defect_maps(dark_reports)
    out = []
    for i, r in enumerate(dark_reports):
        others = set().union(*(m for j, m in enumerate(maps) if j != i))
        sm = r.get("sensorMap", [1, 1, 0, 0])
        worst = max(r["steps"], key=lambda s: (s["iso"], s["exposureNs"]))
        d = np.asarray(worst["stats"]["defects"], float).reshape(-1, 6)
        inm = np.array([(int(round(x * sm[0] + sm[2])), int(round(y * sm[1] + sm[3]))) in others for x, y in d[:, :2]])
        strong = np.abs(d[:, 4]) > 10 * np.maximum(d[:, 5], 1e-6)
        out.append({"date": r.get("date", ""), "iso": worst["iso"], "all": float(inm.mean()) if len(d) else 1.0,
                    "strong": float(inm[strong].mean()) if strong.any() else 1.0, "n": int(len(d))})
    return out


def write_profile(path, reports, noise, device=None, camera=None, defects=True):
    """assets/sensor_profiles/*.json for the app: per-ISO dark-noise correction
    (measured / HAL O; the app multiplies the camera's O by it) and the static
    hot-pixel map (union of every dark sweep, pre-correction array x, y)."""
    dark_reports = [r for r in reports if r["kind"] == "dark"]
    table = [{"iso": int(iso), "O": float(n["O"]), "halO": float(n["halO"]), "factor": round(float(n["O"] / n["halO"]), 4)}
             for iso, n in sorted(noise.items()) if n["halO"] > 0]
    defects = sorted(set().union(*defect_maps(dark_reports))) if dark_reports and defects else []
    prof = {"format": "vesper-sensor-profile/1", "device": device or reports[0].get("device"),
            "cameraId": str(camera if camera is not None else reports[0].get("cameraId")),
            "source": [f"{r['kind']} sweep {r.get('date', '')}" for r in reports],
            "darkNoise": table, "defectCount": len(defects), "defects": [v for xy in defects for v in xy]}
    with open(path, "w") as f:
        json.dump(prof, f, separators=(",", ":"))
    return prof


# --- Report ------------------------------------------------------------------
def pct(x):
    return f"{100 * x:+.1f}%"


def verdict(ok, text):
    return f"  [{'OK ' if ok else 'FIX'}] {text}"


def report(paths, out_json=None, file=sys.stdout, profile=None, device=None, camera=None, profile_defects=True):
    dark, white, reports = load(paths)
    p = lambda *a: print(*a, file=file)  # noqa: E731
    r0 = reports[0]
    p(f"Sensor calibration: {r0.get('device')} camera {r0.get('cameraId')}, {r0['width']}x{r0['height']} RAW10, "
      f"ISO {r0['minIso']}-{r0['maxIso']} (analog up to {r0.get('maxAnalogIso') or '?'})")
    for r in reports:
        p(f"  {r['kind']:5s} sweep {r.get('date', '')}: {len(r['steps'])} steps" +
          (f"; warnings: {'; '.join(r['warnings'])}" if r.get("warnings") else ""))
    summary, result = [], {}

    black = analyse_black(dark) if dark else {}
    result["black"] = black
    if black:
        p("\nA. BLACK LEVEL (lens covered)  DN; offset = measured - reported by the camera")
        p("   ISO  reported   measured R / Gr / Gb / B          offset              dark noise  row noise  hot")
        worst = 0.0
        for iso, b in black.items():
            m, o = b["long"]["black"], b["offset"]
            worst = max(worst, max(abs(x) for x in o))
            p(f"  {iso:5d}  {b['reported'][1]:7.2f}   " + " ".join(f"{x:7.2f}" for x in m) + "   " +
              " ".join(f"{x:+5.2f}" for x in o) + f"   {np.mean(b['long']['sigma'][1:3]):6.2f}   "
              f"{np.mean(b['long']['rowNoise'][1:3]):6.2f}  {b['long']['hot']:5d}")
        lo = min(black)
        dn_grey = GREY * (1023 - black[lo]["reported"][1])
        p(f"   (18% grey sits only {dn_grey:.0f} DN above black with {HEADROOM_STOPS} stops of headroom: a 1 DN error "
          f"moves grey -3 stops by {math.log2((dn_grey / 8 + 1) / (dn_grey / 8)):.2f} stop and grey -5 stops by "
          f"{math.log2((dn_grey / 32 + 1) / (dn_grey / 32)):.2f} stop)")
        tint = max(max(b["offset"]) - min(b["offset"]) for b in black.values())
        summary.append(verdict(worst < 0.3, f"black level: worst offset {worst:.2f} DN, site-to-site {tint:.2f} DN"))
        dc = max(max(b["darkCurrent"]) for b in black.values())
        p(f"   dark current (1/fps vs 1 ms): up to {dc:+.2f} DN")

    noise = analyse_noise(white, dark, black) if (white or dark) else {}
    result["noise"] = noise
    if noise:
        p("\nB. NOISE MODEL  variance = S*x + O, x = normalised signal (SENSOR_NOISE_PROFILE units)")
        p("   ISO   measured S   measured O |  camera S     camera O  |  app uses S/O     | app sigma / real sigma at:"
          " black  grey-3  grey  half")
        worst = 0.0
        levels = lambda n: (0.0, GREY / 8, GREY) if n["sMeasured"] else (0.0,)  # noqa: E731
        for iso, n in noise.items():
            ratios = []
            for x in (0.0, GREY / 8, GREY, 0.5):
                real = math.sqrt(max(n["S"] * x + n["O"], 1e-15))
                eng = math.sqrt(max(n["engS"] * x + n["engO"], 1e-15))
                ratios.append(eng / real)
            worst = max(worst, max(abs(math.log(ratios[i])) for i in range(len(levels(n)))))
            s_txt = f"{n['S']:.3e}" if n["sMeasured"] else "   (white)"
            r_txt = "  ".join(f"{r:5.2f}" for r in ratios) if n["sMeasured"] else f"{ratios[0]:5.2f}   (lit levels need the White sweep)"
            p(f"  {iso:5d}  {s_txt}  {n['O']:.3e} | {n['halS']:.3e}  {n['halO']:+.2e} | "
              f"{n['engS']:.2e}/{n['engO']:.1e} |   {r_txt}")
        m = iso_model(noise)
        result["isoModel"] = m
        if m:
            p(f"   across ISOs: S = {m['a']:.4g} * ISO (max error {100 * m['maxErrS']:.0f}%), "
              f"O = {m['b']:.4g} + {m['c']:.4g} * ISO^2 (max error {100 * m['maxErrO']:.0f}%)")
        lo = min(noise)
        n = noise[lo]
        rng = 1023 - (black[lo]["reported"][1] if black else 64)
        if n["sMeasured"]:
            p(f"   at ISO {lo}: {1 / (n['S'] * rng):.2f} electrons per DN, full well ~{1 / n['S']:.0f} e-, read noise "
              f"{math.sqrt(max(n['O'], 0)) / n['S']:.1f} e-")
        p("   engineering dynamic range (clip / dark noise): " +
          ", ".join(f"ISO {i} {math.log2(1 / math.sqrt(max(v['O'], 1e-15))):.1f}" for i, v in noise.items()) + " stops")
        fit_err = max(n["fitError"] for n in noise.values())
        p(f"   (median misfit of the model to the measured points: {100 * fit_err:.0f}%)")
        lo_r, hi_r = math.inf, 0.0
        for n in noise.values():
            for x in levels(n):
                r = math.sqrt(max(n["engS"] * x + n["engO"], 1e-15) / max(n["S"] * x + n["O"], 1e-15))
                lo_r, hi_r = min(lo_r, r), max(hi_r, r)
        summary.append(verdict(worst < math.log(1.15),
                               f"noise model: the app assumes {lo_r:.2f}x to {hi_r:.2f}x the real noise "
                               "(measured levels, all ISOs; 1.00 = right)"))
        uni = noise_uniformity(white, noise, black)
        result["noiseUniformity"] = uni
        if uni:
            p("   raw noise vs model by frame region (1.00 = same everywhere): " +
              ", ".join(f"{k} {v:.2f}" for k, v in uni.items()))

    shading = analyse_shading(white, black) if white else None
    result["shading"] = shading
    if shading:
        p("\nC. LENS SHADING  flat field x the camera's shading map; should be 1.00 everywhere and neutral")
        for st in shading["steps"]:
            p(f"   {st['label']} (rotation {st['rotation']}): map max gain R {st['mapMax'][0]:.2f} "
              f"G {max(st['mapMax'][1:3]):.2f} B {st['mapMax'][3]:.2f} (engine noise model clamps the average at 4.5)")
            p("   region        vignetting  map gain G   after map: G     R/G     B/G")
            for name, rg in st["regions"].items():
                p(f"   {name:12s}   {rg['vignettingStops']:+5.2f} st   x{rg['gainG']:5.2f}       "
                  f"{rg['G']:6.3f}   {rg['R/G']:6.3f}  {rg['B/G']:6.3f}")
            f = st["fits"]
            p(f"   fitted: brightness left->right {pct(f['G']['tiltLR'])}, top->bottom {pct(f['G']['tiltTB'])} "
              "(a straight gradient is usually uneven light, not the lens)")
            p(f"   radial residual at the corners: G {pct(f['G']['radialCorner'])}, R/G {pct(f['R/G']['radialCorner'])}, "
              f"B/G {pct(f['B/G']['radialCorner'])}")
        st = shading["steps"][0]
        f = st["fits"]
        worst_lum = abs(f["G"]["radialCorner"])
        worst_col = max(abs(f["R/G"]["radialCorner"]), abs(f["B/G"]["radialCorner"]))
        summary.append(verdict(worst_lum < 0.05 and worst_col < 0.03,
                               f"lens shading map: corners {pct(f['G']['radialCorner'])} brightness, colour "
                               f"R/G {pct(f['R/G']['radialCorner'])} B/G {pct(f['B/G']['radialCorner'])} after correction"))
        if noise:
            iso = min(noise)
            n = noise[iso]
            le = st["regions"].get("left edge")
            if le:
                p(f"   noise at equal brightness, left edge vs centre (map gain x{le['gainG']:.2f}): "
                  f"{edge_noise_ratio(le['gainG'], n['S'], n['O'], GREY):.1f}x at 18% grey, "
                  f"{edge_noise_ratio(le['gainG'], n['S'], n['O'], GREY / 16):.1f}x at grey -4 stops (ISO {iso})")
                hi_iso = max(noise)
                nh = noise[hi_iso]
                p(f"     at ISO {hi_iso}: {edge_noise_ratio(le['gainG'], nh['S'], nh['O'], GREY):.1f}x at grey, "
                  f"{edge_noise_ratio(le['gainG'], nh['S'], nh['O'], GREY / 16):.1f}x at grey -4 stops")
            worst_gain = max(r["shadeGainTrue"] for r in st["regions"].values())
            summary.append(verdict(worst_gain <= 4.5, f"NR shading clamp: largest average map gain {worst_gain:.2f} "
                                   "(the noise model caps it at 4.5)"))

    clip = analyse_clip(white) if white else {}
    result["clip"] = clip
    if clip:
        p("\nD. CLIP POINT  (overexposed steps)  DN where the sensor saturates vs the reported white level")
        bad = []
        for iso, c in clip.items():
            s = c["sites"]
            p(f"  ISO {iso:5d}: " + "  ".join(f"{SITES[k]} {s[k]['clipDn'] or '-'} (max {s[k]['maxDn']})" for k in range(4)) +
              f"   reported white {c['white']:.0f}")
            for k in range(4):
                if s[k]["clipDn"] is not None and s[k]["clipDn"] < c["white"] - 1:
                    bad.append((iso, SITES[k], s[k]["clipDn"]))
        summary.append(verdict(not bad, "sensor clips at the reported white level" if not bad else
                               f"sensor clips below the reported white at {len(bad)} ISO/site(s), e.g. ISO {bad[0][0]} "
                               f"{bad[0][1]} at {bad[0][2]} DN: the app's clip test (>= white-1) misses them"))

    lin = analyse_linearity(white, black) if white else None
    clip_g = [c["sites"][1]["clipDn"] for c in clip.values() if c["sites"][1]["clipDn"]]
    result["linearity"] = None
    if lin:
        p(f"\nE. LINEARITY / TONE  1-stop shutter ladder at ISO {lin['iso']} (centre, green)")
        p("   shutter     level    vs 1 stop   error   error with  | Apple Log code (10-bit) of that level")
        p("               (raw)    brighter    (stop)  camera black | ideal  measured  with camera black")
        prev = None
        worst = worst_rep = 0.0
        lrows = []
        for r in lin["rows"]:
            s = r["step"]
            if r["clipped"]:
                p(f"   {shutter(s.exposure):>9s}  clipped")
                prev = None
                continue
            if clip_g and r["frac"] >= (min(clip_g) - 2 - (s.white - r["range"])) / r["range"]:
                p(f"   {shutter(s.exposure):>9s}  {r['frac']:8.5f}   at the sensor clip ({min(clip_g)} DN)")
                prev = None
                continue
            ratio = (prev / r["meas"]) if prev else float("nan")
            ideal = lin["slope"] * s.exposure / r["range"]
            err = math.log2(1 + r["dev"]) if r["dev"] > -1 else -9
            err_rep = math.log2(1 + r["devRep"]) if r["devRep"] > -1 else -9
            if r["frac"] < 0.8:
                worst = max(worst, abs(err) if r["frac"] > 0.001 else 0)
                worst_rep = max(worst_rep, abs(err_rep) if r["frac"] > 0.001 else 0)
            p(f"   {shutter(s.exposure):>9s}  {r['frac']:8.5f}   x{ratio:5.3f}   {err:+6.3f}   {err_rep:+6.3f}     | "
              f"{log_code(ideal):6.1f}  {log_code(r['frac']):6.1f}   {log_code(r['rep'] / r['range']):6.1f}")
            lrows.append({"exposureNs": s.exposure, "level": r["frac"], "errStops": err, "errStopsCameraBlack": err_rep})
            prev = r["meas"]
        p(f"   offset left by the camera's black level: {lin['offsetReported']:+.2f} DN")
        result["linearity"] = {"iso": lin["iso"], "rows": lrows, "offsetReported": lin["offsetReported"]}
        summary.append(verdict(worst < 0.05, f"linearity: worst step error {worst:.3f} stop with measured black "
                               f"({worst_rep:.3f} stop with the camera's black level)"))

    clips = [c["clipDn"] for v in clip.values() for c in v["sites"] if c["clipDn"]]
    defects = analyse_defects(dark, noise, black, min(clips) if clips else None) if dark else None
    result["defects"] = defects
    if defects:
        p("\nF. HOT / DEAD PIXELS  (burst mean vs its 8 same-colour neighbours, > 6 sigma)")
        for c in defects["counts"]:
            p(f"   {c['label']}: {c['hot']:6d} hot {c['dead']:6d} dead")
        if defects["levels"]:
            p(f"   worst case {defects['worst']}: excess median {defects['excessDn'][0]:.0f} DN, 90% {defects['excessDn'][1]:.0f},"
              f" 99% {defects['excessDn'][2]:.0f} ({defects['listed']} listed)")
            p("   clean.comp hot-pixel repair on those, by scene brightness: caught / visible but missed")
            for lv in defects["levels"]:
                p(f"     {lv['level']:14s}: {100 * lv['caught']:5.1f}% caught, {lv['visibleMissed']} of {lv['visible']} visible ones missed")
            dark_reports = [r for r in reports if r["kind"] == "dark"]
            if len(dark_reports) >= 2:
                cv = map_crossval(dark_reports)
                result["defectMapCrossval"] = cv
                p("   static hot-pixel map (pixels found in the OTHER sweep(s)) covers, at the worst step:")
                for c in cv:
                    p(f"     sweep {c['date']} ISO {c['iso']}: {100 * c['all']:.0f}% of its {c['n']} defects, "
                      f"{100 * c['strong']:.0f}% of the strong ones (> 10 sigma)")
            missed = max(lv["visibleMissed"] for lv in defects["levels"])
            summary.append(verdict(missed == 0, f"hot pixels: up to {missed} visible ones missed by the repair "
                                   f"({defects['worst']})"))

    if profile:
        prof = write_profile(profile, reports, noise, device, camera, profile_defects)
        p(f"\nSensor profile written: {profile} ({len(prof['darkNoise'])} noise points, {prof['defectCount']} hot pixels)")
    p("\nSUMMARY")
    for line in summary:
        p(line)
    if out_json:
        def clean(o):
            if isinstance(o, dict):
                return {str(k): clean(v) for k, v in o.items() if k != "step"}
            if isinstance(o, (list, tuple)):
                return [clean(v) for v in o]
            if isinstance(o, (np.floating, np.integer)):
                return o.item()
            return o
        with open(out_json, "w") as f:
            json.dump(clean(result), f, indent=1)
    return result


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="VSENSOR_dark_*.json / VSENSOR_white_*.json")
    ap.add_argument("--json", help="also write the numbers as JSON")
    ap.add_argument("--profile", help="write an app sensor profile (assets/sensor_profiles/<phone>_cam<id>.json)")
    ap.add_argument("--device", help="profile: Android Build.MODEL (default: from the sweep)")
    ap.add_argument("--camera", help="profile: camera id (default: from the sweep)")
    ap.add_argument("--no-defects", action="store_true",
                    help="profile: noise table only (a shipped asset: hot pixels differ per phone; the app ignores them there)")
    a = ap.parse_args(argv)
    return report(a.files, a.json, profile=a.profile, device=a.device, camera=a.camera, profile_defects=not a.no_defects)


if __name__ == "__main__":
    main()
