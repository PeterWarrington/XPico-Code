#!/usr/bin/env python3
"""Generate src/mac-carbon/icons.r (the classic Mac icon family) from res/icon.png.

Classic Mac OS takes an application's Finder icon from ICN#/icl8/ics#/ics8
resources, not from any runtime call, which is why the Win32 shim's LoadIcon
has nothing useful to return. Run this after changing the source icon:

    python3 tools/make_mac_icons.py

Needs Pillow. The output is checked in, so this is not part of the build.
Prefers res/icon-mac.png over res/icon.png when both exist.
"""
import os
from PIL import Image

# The classic Mac 8-bit system palette. Entries 0..214 are the 6x6x6 colour
# cube with components from {255,204,153,102,51,0}, red outermost, descending.
# Pure black is at 255. Index 215 is ambiguous between sources, so it is never
# emitted and near-black maps to 255 instead -- correct under either reading.
# Luminance below this becomes a black pixel in the 1-bit icons. Chosen by
# eye against res/icon-mac.png: lower loses the blue shapes, higher floods the
# light platform and swallows the white "p".
BW_THRESHOLD = 140

LEVELS = [255, 204, 153, 102, 51, 0]
PALETTE = {}
for _ri, _r in enumerate(LEVELS):
    for _gi, _g in enumerate(LEVELS):
        for _bi, _b in enumerate(LEVELS):
            _idx = _ri * 36 + _gi * 6 + _bi
            if _idx < 215:
                PALETTE[_idx] = (_r, _g, _b)
PALETTE[255] = (0, 0, 0)


def nearest(rgb):
    r, g, b = rgb
    best, bd = 255, 1 << 30
    for idx, (pr, pg, pb) in PALETTE.items():
        d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
        if d < bd:
            bd, best = d, idx
    return best


def hexblock(data, per=16):
    out = []
    for i in range(0, len(data), per):
        chunk = data[i:i + per]
        groups = [chunk[j:j + 2].hex().upper() for j in range(0, len(chunk), 2)]
        out.append('\t\t$"' + ' '.join(groups) + '"')
    return '\n'.join(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    # res/icon-mac.png is drawn for classic Mac OS (isometric, transparent
    # background); fall back to the Windows icon if it is not present.
    for name in ('icon-mac.png', 'icon.png'):
        path = os.path.join(root, 'res', name)
        if os.path.exists(path):
            break
    print("source:", os.path.relpath(path, root))
    src = Image.open(path).convert('RGBA')
    parts = []

    for size, bw_type, c8_type in ((32, 'ICN#', 'icl8'), (16, 'ics#', 'ics8')):
        im = src.resize((size, size), Image.LANCZOS)
        px = im.load()
        rowbytes = size // 8

        # The mask first, since the 1-bit icon outlines its edge.
        opaque = [[px[x, y][3] > 127 for x in range(size)] for y in range(size)]
        lum = [[(px[x, y][0] * 299 + px[x, y][1] * 587 + px[x, y][2] * 114) // 1000
                for x in range(size)] for y in range(size)]

        icon, mask = bytearray(), bytearray()
        for y in range(size):
            ibits, mbits = bytearray(rowbytes), bytearray(rowbytes)
            for x in range(size):
                if not opaque[y][x]:
                    continue
                mbits[x // 8] |= 0x80 >> (x % 8)
                # A set bit is black. Plain luminance thresholding is not enough
                # on its own: downsampling blends the artwork into its lighter
                # parts, so a shape can come out almost entirely white with no
                # defined edge. Tracing the mask boundary in black gives the
                # outline that a 1-bit icon needs to read at all.
                dark = lum[y][x] < BW_THRESHOLD
                edge = any(nx < 0 or ny < 0 or nx >= size or ny >= size
                           or not opaque[ny][nx]
                           for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
                if dark or edge:
                    ibits[x // 8] |= 0x80 >> (x % 8)
            icon += ibits
            mask += mbits
        blob = hexblock(bytes(icon + mask))
        # ID 128 is what the BNDL maps; -16455 is the "custom icon" ID the
        # Finder reads directly, without consulting the desktop database.
        parts.append("data '%s' (128) {\n%s\n};\n" % (bw_type, blob))
        parts.append("data '%s' (-16455) {\n%s\n};\n" % (bw_type, blob))

        colour = bytearray()
        for y in range(size):
            for x in range(size):
                r, g, b, a = px[x, y]
                colour.append(0 if a <= 127 else nearest((r, g, b)))
        cblob = hexblock(bytes(colour))
        parts.append("data '%s' (128) {\n%s\n};\n" % (c8_type, cblob))
        parts.append("data '%s' (-16455) {\n%s\n};\n" % (c8_type, cblob))

    out = os.path.join(root, 'src', 'mac-carbon', 'icons.r')
    with open(out, 'w') as f:
        f.write("/* Icon family for XPico Code, generated from res/icon.png by\n"
                " * tools/make_mac_icons.py -- edit that, not this file.\n"
                " *\n"
                " * ICN#/ics# are the 1-bit icon plus its mask; icl8/ics8 are the 8-bit\n"
                " * versions. All share local ID 128, which the BNDL maps to the app. */\n\n")
        f.write('\n'.join(parts))
    print("wrote", out)


if __name__ == '__main__':
    main()
