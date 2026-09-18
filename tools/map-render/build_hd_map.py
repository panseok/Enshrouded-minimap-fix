"""Render the HD minimap texture from the game's own map data.

Input: the folder written by tools/eml-map-exporter (export/minimap_map_exporter):
  * export-log.txt                      - maps part numbers to debug names
  * ui_map_part_NNN.raw                 - raw texture bytes
      _map_shader_parameters_baseHeight   R16_unorm 1280x1280  (macro elevation)
      DetailHeight(x,y)                   BC4 1024x1024 x 100  (fine elevation, 10240x10240)
      _map_shader_parameters_baseGradient R8G8B8A8 151x1       (the game's elevation color ramp)
      _map_shader_parameters_isolineGradient R8G8B8A8 128x1    (the game's isoline ramp)

Output (default 8192x8192, split into 2048 tiles):
  embervale_realmap_hd_<row>_<col>.rgba   raw RGBA, row 0 = north
  preview_2048.png

Needs: numpy, scipy, scikit-image, pillow.  ~3 GB RAM, ~1 minute.

usage: python build_hd_map.py <export_dir> <out_dir> [--size 8192] [--tile 2048]
"""
import argparse
import gc
import os
import re
import sys
import time

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from skimage import measure

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bc4 import decode_bc4_unorm  # noqa: E402
from r16 import decode_r16_unorm  # noqa: E402

Image.MAX_IMAGE_PIXELS = None

WATER_LEVEL = 9080.0          # dominant R16 value of the sea floor plateau
DETAIL_AMPLITUDE = 450.0      # DetailHeight (0..255) -> +-450 R16 units
GAMMA = 0.17                  # brightens the game's baseGradient toward its light end
CREAM = np.array([222, 210, 188], dtype=np.float32)
CREAM_MIX = 0.20
WATER_COLOR = np.array([132, 152, 154], dtype=np.float32)
CONTOUR_INTERVAL = 1000.0
DETAIL_RELIEF_Z = 0.2          # detail-height slope scale for the structure relief
DETAIL_RELIEF_K = 0.35         # relief contrast
DETAIL_INK_LO = 3.0            # detail step (per texel) where edge ink starts
DETAIL_INK_HI = 8.0            # ... and reaches full strength
DETAIL_INK_ALPHA = 0.45
INK_COLOR = np.array([74, 62, 54], dtype=np.float32)
MAJOR_EVERY = 5

T0 = time.time()


def log(msg):
    print(f"[{time.time() - T0:6.1f}s] {msg}", flush=True)


def parse_parts(export_dir):
    parts = {}
    current = None
    rx_part = re.compile(r"UiTextureResource part (\d+) data: .*\"debugName\":\"([^\"]+)\"")
    with open(os.path.join(export_dir, "export-log.txt"), encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = rx_part.search(line)
            if m:
                current = int(m.group(1))
                parts.setdefault(m.group(2), current)
    return parts


def read_part(export_dir, part):
    with open(os.path.join(export_dir, f"ui_map_part_{part:03d}.raw"), "rb") as fh:
        return fh.read()


def sample_lut(lut, t):
    n = lut.shape[0]
    pos = np.clip(t, 0.0, 1.0) * (n - 1)
    i0 = np.floor(pos).astype(np.int32)
    i1 = np.minimum(i0 + 1, n - 1)
    f = (pos - i0).astype(np.float32)[..., None]
    return lut[i0] * (1.0 - f) + lut[i1] * f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--size", type=int, default=8192)
    ap.add_argument("--tile", type=int, default=2048)
    args = ap.parse_args()
    size, tile = args.size, args.tile
    assert size % tile == 0, "size must be a multiple of tile"
    os.makedirs(args.out_dir, exist_ok=True)

    parts = parse_parts(args.export_dir)
    base_part = parts["_map_shader_parameters_baseHeight"]
    grad = np.frombuffer(read_part(args.export_dir, parts["_map_shader_parameters_baseGradient"]), np.uint8)
    base_lut = grad.reshape(-1, 4)[:, :3].astype(np.float32)
    iso = np.frombuffer(read_part(args.export_dir, parts["_map_shader_parameters_isolineGradient"]), np.uint8)
    iso_lut = iso.reshape(-1, 4)[:, :3].astype(np.float32)
    log(f"parts: base={base_part} baseGradient={base_lut.shape} isolineGradient={iso_lut.shape}")

    # ---- elevation -------------------------------------------------------------
    base16 = decode_r16_unorm(read_part(args.export_dir, base_part), 1280, 1280).astype(np.float32)
    land = base16[base16 > WATER_LEVEL]
    max_ref = float(np.percentile(land, 85))
    top_level = float(np.percentile(land, 99.5))

    HI = 10240
    detail = np.zeros((HI, HI), dtype=np.uint8)
    rx = re.compile(r"DetailHeight\((\d+),(\d+)\)")
    count = 0
    for name, part in parts.items():
        m = rx.fullmatch(name)
        if not m:
            continue
        x, y = int(m.group(1)), int(m.group(2))
        # DetailHeight(x, y): y counts rows from the SOUTH edge (verified against the
        # base height: gradient correlation 0.64 vs 0.05 for the unflipped order).
        row = 9 - y
        detail[row * 1024:(row + 1) * 1024, x * 1024:(x + 1) * 1024] = decode_bc4_unorm(read_part(args.export_dir, part), 1024, 1024)
        count += 1
    assert count == 100, f"expected 100 DetailHeight tiles, got {count}"
    log("decoded 100 DetailHeight tiles")

    # ---- structure / small-relief layer from the detail height alone ------------
    # Buildings, walls, ruins and cliffs live in DetailHeight as small, sharp steps.
    # The game's map reveals them by shading; do the same, plus a thin ink line on
    # the sharpest steps, at full 10240 resolution before downsampling.
    dd = ndimage.gaussian_filter(detail.astype(np.float32), 0.6)
    gy, gx = np.gradient(dd)
    gx *= -DETAIL_RELIEF_Z
    gy *= -DETAIL_RELIEF_Z
    light = np.array([-0.5, -0.6, 0.62], dtype=np.float32)
    light /= np.linalg.norm(light)
    relief = (gx * light[0] + gy * light[1] + light[2]) / np.sqrt(gx * gx + gy * gy + 1.0)
    relief = np.clip(1.0 + (relief - light[2]) * DETAIL_RELIEF_K, 0.8, 1.1).astype(np.float32)
    del gx, gy
    gc.collect()
    mag = np.hypot(ndimage.sobel(dd, 0), ndimage.sobel(dd, 1)) / 8.0
    del dd
    ink = (np.clip((mag - DETAIL_INK_LO) / (DETAIL_INK_HI - DETAIL_INK_LO), 0.0, 1.0) * DETAIL_INK_ALPHA).astype(np.float32)
    del mag
    gc.collect()
    relief = np.asarray(Image.fromarray(relief, mode="F").resize((size, size), Image.BOX), dtype=np.float32)
    ink = np.asarray(Image.fromarray(ink, mode="F").resize((size, size), Image.BOX), dtype=np.float32)
    log("detail relief + ink")

    elev = np.array(Image.fromarray(base16).resize((HI, HI), Image.BICUBIC), dtype=np.float32)
    elev += (detail.astype(np.float32) - 128.0) * (DETAIL_AMPLITUDE / 128.0)
    del detail
    gc.collect()
    log("combined elevation 10240")

    # smoothed copy for contour tracing (kills texel staircase noise only)
    trace = ndimage.gaussian_filter(elev, sigma=2.0).astype(np.float64)
    log("trace field ready")

    # ---- color at output resolution --------------------------------------------
    elev_out = np.array(Image.fromarray(elev).resize((size, size), Image.BOX), dtype=np.float32)
    del elev
    gc.collect()

    gy, gx = np.gradient(elev_out)
    k = 0.02 * (HI / size)
    gx *= -k
    gy *= -k
    lx, ly, lz = np.array([-0.4, -0.4, 0.82]) / np.linalg.norm([-0.4, -0.4, 0.82])
    shade = (gx * lx + gy * ly + lz) / np.sqrt(gx * gx + gy * gy + 1.0)
    del gx, gy
    shade = (0.95 + 0.10 * (shade * 0.5 + 0.5)).astype(np.float32)
    gc.collect()
    log("hillshade")

    color = np.empty((size, size, 3), dtype=np.uint8)
    step = 1024
    for r0 in range(0, size, step):
        e = elev_out[r0:r0 + step]
        t = np.power(np.clip((e - WATER_LEVEL) / (max_ref - WATER_LEVEL), 0.0, 1.0), GAMMA)
        c = sample_lut(base_lut, t) * (1.0 - CREAM_MIX) + CREAM * CREAM_MIX
        c *= shade[r0:r0 + step, :, None]
        land = (e > WATER_LEVEL)[..., None]
        c = np.where(land, c, WATER_COLOR)
        rl = relief[r0:r0 + step, :, None]
        ik = ink[r0:r0 + step, :, None] * land
        c = np.where(land, c * rl, c)
        c = c * (1.0 - ik) + INK_COLOR * ik
        color[r0:r0 + step] = np.clip(c, 0, 255).astype(np.uint8)
    del elev_out, shade, relief, ink
    gc.collect()
    log("base color")

    # ---- contour lines, drawn on a 2x canvas then box-filtered -------------------
    SS = 2
    canvas_size = size * SS
    canvas = Image.fromarray(color).resize((canvas_size, canvas_size), Image.NEAREST)
    del color
    gc.collect()
    draw = ImageDraw.Draw(canvas)
    line = tuple(int(v * 0.7 + 222 * 0.3) for v in iso_lut[40])
    darkest = iso_lut[int(np.argmin(iso_lut.mean(axis=1)))]
    major = tuple(int(v * 0.8 + 222 * 0.2) for v in darkest)
    scale = canvas_size / HI
    minor_w = max(1, int(round(1.5 * SS * size / 8192)))
    major_w = max(2, int(round(2.5 * SS * size / 8192)))
    levels = np.arange(WATER_LEVEL + CONTOUR_INTERVAL, top_level, CONTOUR_INTERVAL)
    n = 0
    for level in levels:
        is_major = int(round(level / CONTOUR_INTERVAL)) % MAJOR_EVERY == 0
        for c in measure.find_contours(trace, float(level)):
            if len(c) < 3:
                continue
            pts = [(float(x) * scale, float(y) * scale) for (y, x) in c]
            draw.line(pts, fill=major if is_major else line, width=major_w if is_major else minor_w, joint="curve")
            n += 1
    del trace
    gc.collect()
    log(f"drew {n} contour polylines (minor w={minor_w}, major w={major_w} on {canvas_size}px canvas)")

    final = canvas.reduce(SS)
    del canvas, draw
    gc.collect()
    rgba = np.array(final.convert("RGBA"))
    log(f"final {rgba.shape}")

    tiles = size // tile
    for row in range(tiles):
        for col in range(tiles):
            block = np.ascontiguousarray(rgba[row * tile:(row + 1) * tile, col * tile:(col + 1) * tile])
            block.tofile(os.path.join(args.out_dir, f"embervale_realmap_hd_{row}_{col}.rgba"))
    final.resize((2048, 2048), Image.LANCZOS).save(os.path.join(args.out_dir, "preview_2048.png"))
    log(f"wrote {tiles}x{tiles} tiles to {args.out_dir}")


if __name__ == "__main__":
    main()
