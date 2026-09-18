"""Build the shroud (FogZone) signed-distance file for the minimap.

Input: the folder written by tools/eml-fog-exporter (export/minimap_fog_exporter):
  export-log.txt, part_NNN.raw  (FogZone(x,y): BC5 1024x1024, 10x10 tiles)

FogZone channels (decoded offline):
  G = shroud zone level: 0 = land without shroud, 1..10 = shroud level, 16 = sea/outside
  R = distance-to-border ramp used by the game's fogZoneBorderGradient
Tile FogZone(x, y) covers world column x and row y counted from the SOUTH edge
(verified: this layout puts 0.3% of the shroud over water, the mirrored one 8.8%).

Output: embervale_shroud_sdf.r8 -- <size>x<size> uint8, row 0 = north.
  value = clamp(128 + d * 127 / RANGE), d = signed distance to the shroud border in
  world units, positive OUTSIDE the shroud, so 255 (the default) means "no shroud".

usage: python build_shroud_sdf.py <fog_export_dir> <out_dir> [--size 4096] [--preview]
Needs: numpy, scipy, pillow.
"""
import argparse
import os
import re
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bc4 import decode_bc4_unorm  # noqa: E402

WORLD = 10240
TILE = 1024
RANGE = 48.0          # world units covered by the 0..255 ramp on each side
WORK = 5120           # distance transform resolution (2 world units per texel)


def load_levels(export_dir):
    log = open(os.path.join(export_dir, "export-log.txt"), encoding="utf-8", errors="replace").read()
    levels = np.full((WORLD, WORLD), 16, dtype=np.uint8)
    count = 0
    for m in re.finditer(r"part (\d+) name=FogZone\((\d+),(\d+)\)", log):
        part, x, y = map(int, m.groups())
        raw = open(os.path.join(export_dir, "part_%03d.raw" % part), "rb").read()
        blocks = np.frombuffer(raw, np.uint8).reshape(-1, 16)
        g = decode_bc4_unorm(blocks[:, 8:].tobytes(), TILE, TILE)
        row = 9 - y
        levels[row * TILE:(row + 1) * TILE, x * TILE:(x + 1) * TILE] = g
        count += 1
    if count != 100:
        raise SystemExit(f"expected 100 FogZone tiles, found {count}")
    return levels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export_dir")
    ap.add_argument("out_dir")
    ap.add_argument("--size", type=int, default=4096)
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)

    levels = load_levels(args.export_dir)
    shroud = (levels >= 1) & (levels <= 10)
    print(f"shroud covers {shroud.mean() * 100:.1f}% of the world square")

    # 2x2 majority down to the working grid
    f = WORLD // WORK
    cover = shroud.reshape(WORK, f, WORK, f).mean(axis=(1, 3))
    inside = cover >= 0.5
    del shroud, levels

    texel = WORLD / WORK
    d_out = ndimage.distance_transform_edt(~inside) * texel   # >0 outside
    d_in = ndimage.distance_transform_edt(inside) * texel     # >0 inside
    sdf = np.where(inside, -(d_in - texel * 0.5), d_out - texel * 0.5).astype(np.float32)
    del d_out, d_in

    sdf_img = Image.fromarray(sdf, mode="F").resize((args.size, args.size), Image.BILINEAR)
    sdf = np.asarray(sdf_img, dtype=np.float32)
    encoded = np.clip(np.round(128.0 + sdf * 127.0 / RANGE), 0, 255).astype(np.uint8)
    out = os.path.join(args.out_dir, "embervale_shroud_sdf.r8")
    encoded.tofile(out)
    print(f"wrote {out} ({encoded.nbytes} bytes, {args.size}x{args.size})")

    if args.preview:
        Image.fromarray(encoded).resize((1024, 1024), Image.BILINEAR).save(os.path.join(args.out_dir, "shroud_sdf_preview.png"))


if __name__ == "__main__":
    main()
