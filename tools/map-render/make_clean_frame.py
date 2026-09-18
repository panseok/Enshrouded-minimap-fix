"""Build the thin, semi-transparent square minimap frame.

The frame texture covers the square of side (radius + frameExtra) * 2 that the DLL
draws it into; the map window inside it ends at 188/256 of the half size
(MinimapRasterMapRadius), so the opening edge sits at that Chebyshev radius.

usage: python make_clean_frame.py <out.rgba> [--preview out.png]
Output is raw 1024x1024 RGBA.
"""
import argparse

import numpy as np
from PIL import Image, ImageDraw, ImageFont

N = 1024
SS = 4                      # supersampling for clean edges
WINDOW = 188.0 / 256.0      # map opening, as a fraction of the half size

# Band geometry in half-size units, measured from the centre.
INNER_DARK = 0.005          # dark hairline just inside the opening
LIGHT = 0.016               # main light line
OUTER_DARK = 0.007          # dark hairline outside it

LIGHT_RGB = (226.0, 236.0, 245.0)
DARK_RGB = (8.0, 12.0, 18.0)
LIGHT_ALPHA = 0.62
DARK_ALPHA = 0.42

LABEL_FONTS = (
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
)
LABEL_SIZE = 34             # letter height in pixels at 1024
LABEL_GAP = 13              # gap between the band and the letters
LABEL_ALPHA = 200


def layer(mask, rgb, alpha, out_rgb, out_a):
    """Alpha-composite a flat colour over the accumulator."""
    a = (mask * alpha)[..., None]
    out_rgb[:] = np.array(rgb, np.float32) * a + out_rgb * (1.0 - a)
    out_a[:] = (a[..., 0] + out_a * (1.0 - a[..., 0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dst")
    ap.add_argument("--preview")
    args = ap.parse_args()

    c = (N - 1) / 2.0
    ys, xs = np.mgrid[0:N * SS, 0:N * SS].astype(np.float32)
    x = ((xs + 0.5) / SS - 0.5 - c) / (N / 2.0)
    y = ((ys + 0.5) / SS - 0.5 - c) / (N / 2.0)
    s = np.maximum(np.abs(x), np.abs(y))        # Chebyshev radius, 1.0 at the edge

    inner = WINDOW
    light0 = inner + INNER_DARK
    light1 = light0 + LIGHT
    outer1 = light1 + OUTER_DARK

    rgb = np.zeros((N * SS, N * SS, 3), np.float32)
    a = np.zeros((N * SS, N * SS), np.float32)

    # Dark hairlines first, light line on top.
    layer(((s >= inner) & (s < light0)).astype(np.float32), DARK_RGB, DARK_ALPHA, rgb, a)
    layer(((s >= light1) & (s < outer1)).astype(np.float32), DARK_RGB, DARK_ALPHA, rgb, a)
    layer(((s >= light0) & (s < light1)).astype(np.float32), LIGHT_RGB, LIGHT_ALPHA, rgb, a)

    # Downsample (premultiplied so the edges stay clean).
    pm = rgb * a[..., None]
    pm = pm.reshape(N, SS, N, SS, 3).mean(axis=(1, 3))
    a = a.reshape(N, SS, N, SS).mean(axis=(1, 3))
    out_rgb = np.where(a[..., None] > 1e-4, pm / np.maximum(a, 1e-4)[..., None], 0.0)
    out = np.concatenate([np.clip(out_rgb, 0, 255), np.clip(a * 255.0, 0, 255)[..., None]], -1)
    out = out.astype(np.uint8)

    # N / E / S / W just outside the band, on the edge midpoints.
    image = Image.fromarray(out, "RGBA")
    labels = Image.new("RGBA", image.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(labels)
    font = None
    for path in LABEL_FONTS:
        try:
            font = ImageFont.truetype(path, LABEL_SIZE)
            break
        except OSError:
            continue
    if font is not None:
        edge = outer1 * (N / 2.0)
        centre = (N - 1) / 2.0
        colour = tuple(int(v) for v in LIGHT_RGB) + (LABEL_ALPHA,)
        for text, (dx, dy) in (("N", (0, -1)), ("S", (0, 1)), ("E", (1, 0)), ("W", (-1, 0))):
            box = draw.textbbox((0, 0), text, font=font)
            w = box[2] - box[0]
            h = box[3] - box[1]
            cx = centre + dx * (edge + LABEL_GAP + w / 2.0)
            cy = centre + dy * (edge + LABEL_GAP + h / 2.0)
            draw.text(
                (cx - w / 2.0 - box[0], cy - h / 2.0 - box[1]),
                text,
                font=font,
                fill=colour,
                stroke_width=2,
                stroke_fill=tuple(int(v) for v in DARK_RGB) + (150,),
            )
    out = np.asarray(Image.alpha_composite(image, labels))

    out.tofile(args.dst)
    if args.preview:
        Image.fromarray(out).save(args.preview)


if __name__ == "__main__":
    main()
