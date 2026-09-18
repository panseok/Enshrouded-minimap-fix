"""Turn the round minimap frame into the square one.

Every pixel of the square output at Chebyshev radius s and angle t samples the round
frame at Euclidean radius s and the same angle, so the ring band becomes a square
band with the same inner/outer offsets (the map window size in the DLL is unchanged)
and the N/E/S/W badges stay upright on the edge midpoints.

usage: python make_square_frame.py <round_frame.rgba> <out.rgba> [--preview out.png]
Both files are raw 1024x1024 RGBA.
"""
import argparse

import numpy as np
from PIL import Image
from scipy import ndimage

N = 1024
SS = 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--preview")
    args = ap.parse_args()

    src = np.fromfile(args.src, np.uint8).reshape(N, N, 4).astype(np.float32)
    pm = src.copy()
    pm[..., :3] *= pm[..., 3:4] / 255.0  # premultiplied for clean filtering

    c = (N - 1) / 2.0
    ys, xs = np.mgrid[0:N * SS, 0:N * SS].astype(np.float32)
    x = (xs + 0.5) / SS - 0.5 - c
    y = (ys + 0.5) / SS - 0.5 - c
    s = np.maximum(np.abs(x), np.abs(y))
    t = np.arctan2(y, x)
    sx = c + s * np.cos(t)
    sy = c + s * np.sin(t)

    out = np.zeros((N * SS, N * SS, 4), np.float32)
    for ch in range(4):
        out[..., ch] = ndimage.map_coordinates(pm[..., ch], [sy, sx], order=1, mode="constant")
    out = out.reshape(N, SS, N, SS, 4).mean(axis=(1, 3))
    a = out[..., 3:4]
    rgb = np.where(a > 0.5, out[..., :3] / np.maximum(a, 1e-3) * 255.0, 0.0)
    res = np.concatenate([np.clip(rgb, 0, 255), a], -1).astype(np.uint8)
    res.tofile(args.dst)
    if args.preview:
        Image.fromarray(res).save(args.preview)


if __name__ == "__main__":
    main()
