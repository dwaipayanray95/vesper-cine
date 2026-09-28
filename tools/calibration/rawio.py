"""Loads linear camera RGB (2x2 quad resolution) from
  * a Vesper calibration capture: <name>.raw10 + <name>.json (PROCESSING ->
    CAPTURE CALIBRATION FRAME in the app), or
  * a DNG (e.g. Google Camera RAW), via the optional `rawpy` package.
Black level is subtracted, the lens-shading gain map applied (Vesper captures),
and each 2x2 Bayer quad becomes one (R, mean G, B) pixel — the same
linearisation the app's GPU pipeline does before its colour matrix.
"""
import json
import os

import numpy as np

CFA_SITES = {  # logical channel 0=R 1=Gr 2=Gb 3=B at raw parity (x&1, y&1) -> index y*2+x
    0: [0, 1, 2, 3], 1: [1, 0, 3, 2], 2: [2, 3, 0, 1], 3: [3, 2, 1, 0],
}


def unpack_raw10(buf, width, height, stride):
    rows = np.frombuffer(buf, np.uint8)[: stride * height].reshape(height, stride)[:, : width * 5 // 4]
    g = rows.reshape(height, width // 4, 5).astype(np.uint16)
    out = np.empty((height, width // 4, 4), np.uint16)
    for i in range(4):
        out[..., i] = (g[..., i] << 2) | ((g[..., 4] >> (2 * i)) & 3)
    return out.reshape(height, width)


def quads(raw, cfa, black, white, shading=None):
    """raw: (H,W) DN. black: [R,Gr,Gb,B]. Returns (H/2, W/2, 3) linear 0..1 of clip, and a clip mask."""
    h, w = raw.shape[0] // 2 * 2, raw.shape[1] // 2 * 2
    raw = raw[:h, :w].astype(np.float64)
    sites = CFA_SITES[cfa]
    planes = {}
    for py in range(2):
        for px in range(2):
            ch = sites[py * 2 + px]
            p = (raw[py::2, px::2] - black[ch]) / (white - black[ch])
            if shading is not None:
                p = p * shading(ch if ch in (0, 3) else (1 if py == 0 else 2), p.shape)
            planes[ch] = p
    clipped = (raw[0::2, 0::2] >= white - 1) | (raw[1::2, 1::2] >= white - 1) | \
              (raw[0::2, 1::2] >= white - 1) | (raw[1::2, 0::2] >= white - 1)
    rgb = np.stack([planes[0], 0.5 * (planes[1] + planes[2]), planes[3]], axis=-1)
    return rgb, clipped


def _shading_sampler(meta):
    cols, rows = meta.get("shadingCols", 0), meta.get("shadingRows", 0)
    data = meta.get("shadingMap")
    if not data or cols < 2 or rows < 2:
        return None
    grid = np.asarray(data, float).reshape(rows, cols, 4)
    arr_w, arr_h = meta.get("arrayWidth", meta["width"]), meta.get("arrayHeight", meta["height"])
    sx = arr_w / meta["width"]
    off_y = max(0.0, (arr_h - meta["height"] * sx) / 2)

    def sample(channel, shape):
        qh, qw = shape
        ax = (np.arange(qw) * 2 + 1) * sx
        ay = (np.arange(qh) * 2 + 1) * sx + off_y
        gx = np.clip(ax / arr_w * (cols - 1), 0, cols - 1)
        gy = np.clip(ay / arr_h * (rows - 1), 0, rows - 1)
        x0, y0 = np.minimum(gx.astype(int), cols - 2), np.minimum(gy.astype(int), rows - 2)
        fx, fy = gx - x0, gy - y0
        g = grid[..., channel]
        top = g[y0][:, x0] * (1 - fx) + g[y0][:, x0 + 1] * fx
        bot = g[y0 + 1][:, x0] * (1 - fx) + g[y0 + 1][:, x0 + 1] * fx
        return top * (1 - fy)[:, None] + bot * fy[:, None]

    return sample


def load(path):
    """Returns (rgb_quads, clip_mask, meta)."""
    base, ext = os.path.splitext(path)
    if ext.lower() in (".raw10", ".json"):
        with open(base + ".json") as f:
            meta = json.load(f)
        with open(base + ".raw10", "rb") as f:
            raw = unpack_raw10(f.read(), meta["width"], meta["height"], meta["rowStride"])
        rgb, clip = quads(raw, meta["cfa"], meta["blackLevel"], meta["whiteLevel"], _shading_sampler(meta))
        return rgb, clip, meta
    if ext.lower() == ".dng":
        try:
            import rawpy
        except ImportError as e:
            raise SystemExit("DNG input needs `pip install rawpy`") from e
        with rawpy.imread(path) as r:
            raw = r.raw_image_visible.copy()
            pattern = r.raw_pattern  # 2x2 indices into r.color_desc
            desc = r.color_desc.decode()
            names = [desc[i] for i in pattern.flatten()]
            order = "".join(names)  # e.g. "RGGB"
            cfa = {"RGGB": 0, "GRBG": 1, "GBRG": 2, "BGGR": 3}[order]
            # Phone DNGs use one black level for all sites; LibRaw's per-channel
            # order differs from ours, so use the mean rather than guess.
            bl = float(np.mean(r.black_level_per_channel))
            logical = [bl] * 4
            meta = {"source": "dng", "cfa": cfa, "blackLevel": logical, "whiteLevel": float(r.white_level)}
        rgb, clip = quads(raw, cfa, logical, meta["whiteLevel"])
        return rgb, clip, meta
    raise SystemExit(f"unsupported input {path} (want .raw10/.json or .dng)")


def write_png(path, rgb):
    """Minimal dependency-free PNG writer for previews (sRGB-ish gamma, 8-bit)."""
    import struct
    import zlib
    img = np.clip(rgb / max(np.percentile(rgb, 99.5), 1e-6), 0, 1) ** (1 / 2.2)
    img = (img * 255 + 0.5).astype(np.uint8)
    h, w, _ = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
