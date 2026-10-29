#!/usr/bin/env python3
"""Set the bundle bit on the application inside a built HFS disk image.

    python3 tools/set_bundle_bit.py build/mac/XPicoCode.dsk APPL XPCD

Classic Mac OS only reads an application's BNDL -- and so only shows its custom
icon instead of the generic application one -- when the file's bundle bit
(kHasBundle, 0x2000) is set in its Finder info. Nothing in the build chain sets
it: Rez writes the type and creator but not the flags, and hfsutils' "hattrib -b"
blesses a folder, which is an unrelated flag.

Setting it from inside the app at startup is too late to be useful. The Finder
scans a volume's files and builds its desktop database when the volume is
mounted, so by the time the app runs, the icon has already been resolved to the
generic one and is cached until the desktop is rebuilt. It has to be set in the
image, before the volume is ever mounted.

This edits the file's FInfo in the HFS catalog directly. An HFS catalog file
record holds, in order, fdType (4 bytes), fdCreator (4) and fdFlags (2), so
locating the type+creator pair locates the flags. kHasBeenInited (0x0100) is
deliberately left clear: with it set the Finder assumes it has already dealt
with the file and will not look at the BNDL at all.
"""
import sys

K_HAS_BUNDLE = 0x2000
K_HAS_CUSTOM_ICON = 0x0400
K_HAS_BEEN_INITED = 0x0100


def main(argv):
    if len(argv) != 4:
        print(__doc__.strip().split('\n\n')[1].strip(), file=sys.stderr)
        return 2
    path, ftype, creator = argv[1], argv[2].encode(), argv[3].encode()
    if len(ftype) != 4 or len(creator) != 4:
        print("type and creator must be 4 characters", file=sys.stderr)
        return 2

    data = bytearray(open(path, 'rb').read())
    needle = ftype + creator

    hits = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1

    if not hits:
        print(f"{path}: no {ftype.decode()}/{creator.decode()} file found", file=sys.stderr)
        return 1
    if len(hits) > 1:
        # Both forks live in the same image, so a literal in the code could
        # match. Refusing is safer than patching the wrong bytes.
        print(f"{path}: {len(hits)} matches for {ftype.decode()}/{creator.decode()}, "
              f"expected exactly one: {hits}", file=sys.stderr)
        return 1

    at = hits[0] + 8
    flags = int.from_bytes(data[at:at + 2], 'big')
    # kHasCustomIcon makes the Finder read the icon family at ID -16455 from
    # the file's own resource fork, which works even when a cached desktop
    # database would otherwise keep showing the generic application icon.
    new = (flags | K_HAS_BUNDLE | K_HAS_CUSTOM_ICON) & ~K_HAS_BEEN_INITED
    if new == flags:
        print(f"{path}: flags already set (fdFlags=0x{flags:04X})")
        return 0

    data[at:at + 2] = new.to_bytes(2, 'big')
    open(path, 'wb').write(bytes(data))
    print(f"{path}: fdFlags 0x{flags:04X} -> 0x{new:04X} (bundle + custom icon)")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
