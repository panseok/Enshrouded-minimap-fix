"""Build embervale_minimap_icons.bin from the EML icon export.

Input: the folder written by tools/eml-icon-exporter (export/minimap_icon_exporter):
  manifest.tsv  - markerId, ..., icon ("file:WxH:format:levels:name;...") per marker type
  *.raw         - R8G8B8A8 icon textures at full resolution (64 or 128 px)

Output: EMICO001 file
  magic "EMICO001", u32 count, then per icon: u32 key, u32 width, u32 height, RGBA bytes.
Fully transparent pixels get the color of the nearest opaque pixel, so mipmapped GPU
sampling does not darken icon edges.

Small legacy keys (<= 0xFFFF, the DLL's built-in marker kinds) from an older atlas
are re-pointed at the game icon that looks most like them, at full resolution.

usage: python build_icon_atlas.py <icon_export_dir> <out.bin> [--legacy old.bin] [--sheet preview.png]
"""
import argparse
import os
import struct

import numpy as np
from PIL import Image
from scipy import ndimage


def load_icon(export_dir, spec):
    # spec: "minimap_icon_exporter/m....raw:128x128:R8G8B8A8_unorm:1:name"
    parts = spec.split(":")
    path = os.path.join(export_dir, os.path.basename(parts[0]))
    w, h = (int(v) for v in parts[1].split("x"))
    fmt = parts[2]
    if fmt != "R8G8B8A8_unorm":
        raise SystemExit(f"unsupported icon format {fmt} in {path}")
    data = np.fromfile(path, np.uint8)
    if data.size < w * h * 4:
        raise SystemExit(f"{path}: {data.size} bytes, expected {w * h * 4}")
    return data[: w * h * 4].reshape(h, w, 4).copy(), parts[4] if len(parts) > 4 else ""


def bleed(rgba):
    alpha = rgba[..., 3]
    empty = alpha == 0
    if empty.all() or not empty.any():
        return rgba
    _, (iy, ix) = ndimage.distance_transform_edt(empty, return_indices=True)
    out = rgba.copy()
    out[..., :3] = rgba[iy, ix, :3]
    out[..., 3] = alpha
    return out


# Legacy kinds whose old picture has no exact twin in the old atlas.
LEGACY_OVERRIDES = {
    54: 0x4293EAE5,  # water drop -> mapmarker_playerWaterSource
}


def read_emico(path):
    b = open(path, "rb").read()
    assert b[:8] == b"EMICO001", "not an EMICO001 file"
    n = struct.unpack_from("<I", b, 8)[0]
    cur = 12
    out = []
    for _ in range(n):
        k, w, h = struct.unpack_from("<III", b, cur)
        cur += 12
        px = np.frombuffer(b, np.uint8, w * h * 4, cur).reshape(h, w, 4)
        cur += w * h * 4
        out.append((k, px))
    return out


def signature(rgba):
    # premultiplied, 24x24: compares shape and color, ignores the transparent margin color
    img = Image.fromarray(rgba).resize((24, 24), Image.BOX)
    a = np.asarray(img, np.float32)
    return np.concatenate([a[..., :3] * (a[..., 3:4] / 255.0), a[..., 3:4]], -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export_dir")
    ap.add_argument("out")
    ap.add_argument("--legacy")
    ap.add_argument("--sheet")
    args = ap.parse_args()

    icons = []
    with open(os.path.join(args.export_dir, "manifest.tsv"), encoding="utf-8") as fh:
        header = fh.readline().rstrip("\n").split("\t")
        col_icon = header.index("icon")
        for line in fh:
            cols = line.rstrip("\n").split("\t")
            if len(cols) <= col_icon or not cols[col_icon]:
                continue
            key = int(cols[0], 16)
            spec = cols[col_icon].split(";")[0]
            rgba, name = load_icon(args.export_dir, spec)
            icons.append((key, bleed(rgba), name))

    if args.legacy:
        # A legacy kind's picture is usually stored a second time under the real game
        # key in the same old atlas; follow that twin. Otherwise fall back to the
        # visually closest new icon.
        by_key = {key: rgba for key, rgba, _ in icons}
        legacy = read_emico(args.legacy)
        sigs = [(key, signature(rgba)) for key, rgba, _ in icons]
        for key, px in legacy:
            if key > 0xFFFF or key in by_key:
                continue
            twin = LEGACY_OVERRIDES.get(key) if LEGACY_OVERRIDES.get(key) in by_key else None
            twin = twin if twin is not None else next((k for k, other in legacy
                         if k > 0xFFFF and k in by_key and other.shape == px.shape and np.array_equal(other, px)), None)
            if twin is None:
                sig = signature(px.copy())
                twin = min(sigs, key=lambda item: float(np.mean((item[1] - sig) ** 2)))[0]
                how = "closest"
            else:
                how = "twin"
            icons.append((key, by_key[twin], f"legacy->{twin:08x}"))
            print(f"legacy kind {key} -> 0x{twin:08x} ({how})")

    with open(args.out, "wb") as out:
        out.write(b"EMICO001")
        out.write(struct.pack("<I", len(icons)))
        for key, rgba, _ in icons:
            h, w = rgba.shape[:2]
            out.write(struct.pack("<III", key, w, h))
            out.write(np.ascontiguousarray(rgba).tobytes())
    print(f"wrote {args.out}: {len(icons)} icons")

    if args.sheet:
        cols = 10
        cell = 72
        rows = (len(icons) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cell, rows * cell), (205, 196, 176, 255))
        for i, (key, rgba, name) in enumerate(icons):
            img = Image.fromarray(rgba).resize((64, 64), Image.LANCZOS)
            sheet.alpha_composite(img, ((i % cols) * cell + 4, (i // cols) * cell + 4))
        sheet.save(args.sheet)


if __name__ == "__main__":
    main()
