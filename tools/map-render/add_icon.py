"""Append (or replace) a raw RGBA texture in embervale_minimap_icons.bin under a key.

usage: python add_icon.py <icons.bin> <key hex> <texture.raw> <width> <height>
The file format is EMICO001, u32 count, then per icon: u32 key, u32 width, u32 height, RGBA bytes.
"""
import struct
import sys


def main():
    path, key, raw, width, height = sys.argv[1], int(sys.argv[2], 16), sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
    data = open(path, "rb").read()
    assert data[:8] == b"EMICO001", "not an EMICO001 file"
    count = struct.unpack_from("<I", data, 8)[0]
    off = 12
    entries = []
    for _ in range(count):
        k, w, h = struct.unpack_from("<III", data, off)
        off += 12
        entries.append((k, w, h, data[off:off + w * h * 4]))
        off += w * h * 4
    assert off == len(data), "trailing bytes"
    pixels = open(raw, "rb").read()
    assert len(pixels) == width * height * 4, "raw size does not match width*height*4"
    entries = [e for e in entries if e[0] != key]
    entries.append((key, width, height, pixels))
    out = bytearray(b"EMICO001" + struct.pack("<I", len(entries)))
    for k, w, h, px in entries:
        out += struct.pack("<III", k, w, h) + px
    open(path, "wb").write(out)
    print(f"{path}: {len(entries)} icons, added 0x{key:08x} {width}x{height}")


if __name__ == "__main__":
    main()
