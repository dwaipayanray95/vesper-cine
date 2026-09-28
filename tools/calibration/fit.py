"""Fits a DNG-style ForwardMatrix (white-balanced camera RGB -> CIE XYZ D50)
from measured chart patches, minimising mean CIEDE2000.

Constraint (as in the DNG spec): FM @ [1,1,1] == D50 white, so a neutral
stays neutral after white balance. That leaves 6 free matrix entries plus one
exposure scale (chart illumination is never exactly what the reference assumes).
"""
import numpy as np

from colorchecker import D50_WHITE, NEUTRAL_PATCHES, delta_e_2000, lab_to_xyz, xyz_to_lab


def white_balance(camera_rgb, neutral_idx=NEUTRAL_PATCHES):
    """Normalise so the neutral patches average to equal RGB."""
    n = camera_rgb[neutral_idx].mean(axis=0)
    return camera_rgb / n * n[1]


def _matrix_from_params(p):
    """6 params -> 3x3 with each row summing to the D50 white component."""
    m = np.zeros((3, 3))
    for r in range(3):
        m[r, 0], m[r, 1] = p[2 * r], p[2 * r + 1]
        m[r, 2] = D50_WHITE[r] - m[r, 0] - m[r, 1]
    return m


def _params_from_matrix(m):
    return np.array([m[0, 0], m[0, 1], m[1, 0], m[1, 1], m[2, 0], m[2, 1]])


def _nelder_mead(f, x0, step=0.05, iters=4000, tol=1e-10):
    n = len(x0)
    pts = [x0] + [x0 + step * np.eye(n)[i] * max(1.0, abs(x0[i])) for i in range(n)]
    vals = [f(p) for p in pts]
    for _ in range(iters):
        order = np.argsort(vals)
        pts, vals = [pts[i] for i in order], [vals[i] for i in order]
        if abs(vals[-1] - vals[0]) < tol:
            break
        c = np.mean(pts[:-1], axis=0)
        xr = c + (c - pts[-1]); fr = f(xr)
        if fr < vals[0]:
            xe = c + 2 * (c - pts[-1]); fe = f(xe)
            pts[-1], vals[-1] = (xe, fe) if fe < fr else (xr, fr)
        elif fr < vals[-2]:
            pts[-1], vals[-1] = xr, fr
        else:
            xc = c + 0.5 * (pts[-1] - c); fc = f(xc)
            if fc < vals[-1]:
                pts[-1], vals[-1] = xc, fc
            else:
                pts = [pts[0] + 0.5 * (p - pts[0]) for p in pts]
                vals = [f(p) for p in pts]
    i = int(np.argmin(vals))
    return pts[i], vals[i]


def evaluate(fm, cam_wb, ref_lab, scale=1.0, use=None):
    lab = xyz_to_lab((cam_wb * scale) @ fm.T)
    de = delta_e_2000(lab, ref_lab)
    return de if use is None else de[use]


def fit_forward_matrix(camera_rgb, ref_lab, initial_fm=None, exclude=()):
    """camera_rgb: (24,3) linear, black-subtracted patch means (not white-balanced).
    Returns (forward_matrix, exposure_scale, per_patch_deltaE)."""
    cam = white_balance(np.asarray(camera_rgb, float))
    ref_xyz = lab_to_xyz(ref_lab)
    use = np.array([i for i in range(24) if i not in exclude])

    # Exposure: put the camera's neutral 5 at the reference luminance.
    scale0 = ref_xyz[21, 1] / cam[21, 1]
    if initial_fm is None:
        # Unconstrained least squares, then projected onto the white constraint.
        m, *_ = np.linalg.lstsq(cam[use] * scale0, ref_xyz[use], rcond=None)
        initial_fm = m.T
        initial_fm += (D50_WHITE - initial_fm.sum(axis=1))[:, None] / 3
    x0 = np.concatenate([_params_from_matrix(np.asarray(initial_fm)), [np.log(scale0)]])

    def cost(x):
        return float(np.mean(evaluate(_matrix_from_params(x[:6]), cam, ref_lab, np.exp(x[6]), use)))

    best, _ = _nelder_mead(cost, x0)
    for _ in range(3):  # restarts escape the occasional early collapse
        best, _ = _nelder_mead(cost, best, step=0.02)
    fm, scale = _matrix_from_params(best[:6]), float(np.exp(best[6]))
    return fm, scale, evaluate(fm, cam, ref_lab, scale)


def score_matrix(fm, camera_rgb, ref_lab):
    """Delta E of an existing (e.g. factory) forward matrix, with the best exposure scale."""
    cam = white_balance(np.asarray(camera_rgb, float))
    fm = np.asarray(fm, float)
    scales = np.exp(np.linspace(np.log(0.3), np.log(3.0), 400))
    best = min(scales, key=lambda s: float(np.mean(evaluate(fm, cam, ref_lab, s))))
    return evaluate(fm, cam, ref_lab, best)
