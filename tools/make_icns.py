#!/usr/bin/env python3
"""Generate src/mac-cocoa/XPicoCode.icns from res/icon-mac.png.

Mac OS X 10.4 reads the classic icon family -- a 24-bit RGB image per size,
run-length encoded, with a separate 8-bit mask. The PNG-based entries ('ic08'
and friends) that modern .icns files are made of came later and Tiger ignores
them, so this writes the old chunks and nothing else:

    is32/s8mk   16x16      ih32/h8mk   48x48
    il32/l8mk   32x32      it32/t8mk  128x128

Run this after changing the source icon:

    python3 tools/make_icns.py

Needs Pillow. The output is checked in, so this is not part of the build.
Prefers res/icon-mac.png over res/icon.png when both exist.
"""
import os
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "src", "mac-cocoa", "XPicoCode.icns")

# (size, image chunk, mask chunk)
SIZES = [
    (16, b"is32", b"s8mk"),
    (32, b"il32", b"l8mk"),
    (48, b"ih32", b"h8mk"),
    (128, b"it32", b"t8mk"),
]


def rle(plane):
    """PackBits as the icon family uses it: a literal run is (n-1) then n
    bytes, 1..128; a repeat is (0x80 | n-3) then the byte, 3..130. Runs of two
    are not worth encoding, so they go in the literal."""
    out = bytearray()
    i, n = 0, len(plane)
    literal = bytearray()

    def flush():
        while literal:
            take = literal[:128]
            del literal[:128]
            out.append(len(take) - 1)
            out.extend(take)

    while i < n:
        run = 1
        while i + run < n and plane[i + run] == plane[i] and run < 130:
            run += 1
        if run >= 3:
            flush()
            out.append(0x80 | (run - 3))
            out.append(plane[i])
            i += run
        else:
            literal.append(plane[i])
            i += 1
    flush()
    return bytes(out)


def chunk(kind, data):
    return kind + struct.pack(">I", len(data) + 8) + data


def main():
    src = os.path.join(ROOT, "res", "icon-mac.png")
    if not os.path.exists(src):
        src = os.path.join(ROOT, "res", "icon.png")
    if not os.path.exists(src):
        print("no source icon in res/", file=sys.stderr)
        return 1

    base = Image.open(src).convert("RGBA")
    chunks = []
    for size, img_kind, mask_kind in SIZES:
        im = base.resize((size, size), Image.LANCZOS)
        r, g, b, a = im.split()
        # Each channel is run-length encoded over the whole plane, in turn.
        body = rle(r.tobytes()) + rle(g.tobytes()) + rle(b.tobytes())
        # 128x128 is the one with a four-byte header in front of the data.
        if size == 128:
            body = b"\0\0\0\0" + body
        chunks.append(chunk(img_kind, body))
        chunks.append(chunk(mask_kind, a.tobytes()))

    payload = b"".join(chunks)
    with open(OUT, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(payload) + 8) + payload)

    print("wrote %s (%d bytes) from %s" % (os.path.relpath(OUT, ROOT),
                                           len(payload) + 8,
                                           os.path.relpath(src, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
