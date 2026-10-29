#!/usr/bin/env python3
"""Pack a built Mac application into a classic StuffIt (.sit) archive.

    python3 tools/make_sit.py build/mac/XPicoCode.bin -o build/mac/XPicoCode.sit

A Carbon application is a two-fork file, so it cannot travel as a plain
download: a web server, a modern filesystem or an FTP transfer all drop the
resource fork, and without it the app has no 'cfrg' and will not launch. The
build already writes a MacBinary (XPicoCode.bin) for that reason, but the
format classic Mac users expect to download is a StuffIt archive -- every
Mac OS 8/9 install has StuffIt Expander, and it restores both forks along with
the type, creator and Finder flags.

Nothing on a modern Mac can write one: StuffIt is long gone, and The
Unarchiver / XADMaster only read the format. So this writes the archive
directly. The target is the original "SIT!" format (StuffIt 1.5.1), which is
the most widely readable: StuffIt Expander of any vintage, StuffIt Deluxe,
unsit and XADMaster all handle it, where a StuffIt 5 archive needs Expander 5+.

The layout, after unsit.c:

    archive header, 22 bytes
        0   4   "SIT!"
        4   2   number of entries
        6   4   total archive length
        10  4   "rLau"
        14  1   version
        15  7   reserved

    entry header, 112 bytes, one per file
        0   1   resource fork compression method
        1   1   data fork compression method
        2   64  file name, Pascal string in a 63-byte field
        66  4   file type
        70  4   file creator
        74  2   Finder flags
        76  4   creation date       (Mac epoch, 1904)
        80  4   modification date
        84  4   resource fork length, uncompressed
        88  4   data fork length, uncompressed
        92  4   resource fork length, compressed
        96  4   data fork length, compressed
        100 2   resource fork CRC
        102 2   data fork CRC
        104 6   unused
        110 2   header CRC, over the preceding 110 bytes

    then, per entry, the resource fork's bytes followed by the data fork's.

The CRC is CRC-16/ARC -- the reflected 0xA001 polynomial, initial value zero --
over the *uncompressed* fork.

Two compression methods are written: 0, stored, and 1, the run-length coding
that escapes with 0x90. Each fork gets whichever comes out smaller. Those two
are the methods every StuffIt reader implements; the LZW and Huffman methods
would save more on a PEF binary but are not worth the risk of an archive a
vintage Expander cannot open.
"""
import argparse
import os
import struct
import sys

# Finder flags, as in tools/set_bundle_bit.py. The Finder only reads an
# application's BNDL, and so only shows its own icon, when the bundle bit is
# set; kHasBeenInited must stay clear or it decides it has already looked.
K_HAS_BUNDLE = 0x2000
K_HAS_CUSTOM_ICON = 0x0400
K_HAS_BEEN_INITED = 0x0100

RLE_ESCAPE = 0x90
METHOD_STORED = 0
METHOD_RLE = 1

ARCHIVE_HEADER_LEN = 22
ENTRY_HEADER_LEN = 112

# The Mac epoch is 1904-01-01, this many seconds before the Unix one.
MAC_EPOCH_OFFSET = 2082844800


def crc16(data, crc=0):
    """CRC-16/ARC, the CRC StuffIt stores for each fork and for the header."""
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc


def rle_compress(data):
    """StuffIt method 1: runs collapse to <byte> 0x90 <count>.

    The decoder keeps the last literal it emitted; 0x90 followed by a count n
    repeats that byte n-1 more times, and 0x90 0x00 is a literal 0x90. A count
    is one byte, so a run longer than 255 needs more than one escape.
    """
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        byte = data[i]
        run = 1
        while i + run < n and data[i + run] == byte:
            run += 1

        if byte == RLE_ESCAPE:
            # A literal escape costs two bytes, and only then is 0x90 the
            # decoder's "last byte" for a following repeat count.
            out += bytes((RLE_ESCAPE, 0x00))
        else:
            out.append(byte)

        left = run - 1
        while left > 0:
            # A count of 0 means a literal escape and 1 repeats nothing, so a
            # single escape carries at most 254 repeats.
            step = min(left, 254)
            out += bytes((RLE_ESCAPE, step + 1))
            left -= step
        i += run
    return bytes(out)


def rle_decompress(data, expected_len):
    """Inverse of rle_compress, to self-check what we are about to write."""
    out = bytearray()
    last = None
    i, n = 0, len(data)
    while i < n:
        byte = data[i]
        i += 1
        if byte != RLE_ESCAPE:
            out.append(byte)
            last = byte
            continue
        if i >= n:
            raise ValueError("truncated escape")
        count = data[i]
        i += 1
        if count == 0:
            out.append(RLE_ESCAPE)
            last = RLE_ESCAPE
        elif last is None:
            raise ValueError("repeat with no preceding byte")
        else:
            out += bytes((last,)) * (count - 1)
    if len(out) != expected_len:
        raise ValueError(f"decompressed to {len(out)} bytes, expected {expected_len}")
    return bytes(out)


def pack_fork(data, store_only=False):
    """Return (method, bytes) for one fork, taking whichever method is smaller."""
    if store_only or not data:
        return METHOD_STORED, data
    packed = rle_compress(data)
    if len(packed) >= len(data):
        return METHOD_STORED, data
    # Never ship a stream we have not decoded back ourselves: a bad fork only
    # shows up as a failed expansion, on a machine that cannot debug it.
    if rle_decompress(packed, len(data)) != data:
        raise AssertionError("RLE round-trip failed")
    return METHOD_RLE, packed


class MacFile:
    """One two-fork Mac file: the pieces a StuffIt entry needs."""

    def __init__(self, name, ftype, creator, flags, cdate, mdate, data, rsrc):
        self.name = name
        self.ftype = ftype
        self.creator = creator
        self.flags = flags
        self.cdate = cdate
        self.mdate = mdate
        self.data = data
        self.rsrc = rsrc


def read_macbinary(path):
    """Parse a MacBinary I/II file -- what tools/build_mac.sh writes via Rez."""
    raw = open(path, 'rb').read()
    if len(raw) < 128:
        raise ValueError(f"{path}: too short to be MacBinary")

    namelen = raw[1]
    if raw[0] != 0 or raw[74] != 0 or not 1 <= namelen <= 63:
        raise ValueError(f"{path}: not a MacBinary file")

    name = raw[2:2 + namelen].decode('mac_roman')
    ftype, creator = raw[65:69], raw[69:73]
    # MacBinary splits the Finder flags: the high byte sits in the original
    # header, the low byte was added by MacBinary II.
    flags = (raw[73] << 8) | raw[101]
    cdate, mdate = struct.unpack('>II', raw[91:99])
    datalen, rsrclen = struct.unpack('>II', raw[83:91])

    data_at = 128
    rsrc_at = data_at + (datalen + 127) // 128 * 128
    if rsrc_at + rsrclen > len(raw):
        raise ValueError(f"{path}: forks run past the end of the file")

    data = raw[data_at:data_at + datalen]
    rsrc = raw[rsrc_at:rsrc_at + rsrclen]
    return MacFile(name, ftype, creator, flags, cdate, mdate, data, rsrc)


def read_native(path):
    """Parse a plain file on this Mac, resource fork included if it has one."""
    data = open(path, 'rb').read()
    rsrc = b''
    fork = os.path.join(path, '..namedfork', 'rsrc')
    if os.path.exists(fork):
        rsrc = open(fork, 'rb').read()
    mac_time = max(0, int(os.path.getmtime(path)) + MAC_EPOCH_OFFSET) & 0xFFFFFFFF
    return MacFile(os.path.basename(path), b'????', b'????', 0,
                   mac_time, mac_time, data, rsrc)


def read_input(path):
    try:
        return read_macbinary(path)
    except ValueError:
        return read_native(path)


def build_entry(mac, store_only=False):
    rsrc_method, rsrc_packed = pack_fork(mac.rsrc, store_only)
    data_method, data_packed = pack_fork(mac.data, store_only)

    name = mac.name.encode('mac_roman')[:63]
    header = bytearray(ENTRY_HEADER_LEN)
    header[0] = rsrc_method
    header[1] = data_method
    header[2] = len(name)
    header[3:3 + len(name)] = name
    struct.pack_into('>4s4sHII', header, 66,
                     mac.ftype, mac.creator, mac.flags, mac.cdate, mac.mdate)
    struct.pack_into('>IIIIHH', header, 84,
                     len(mac.rsrc), len(mac.data),
                     len(rsrc_packed), len(data_packed),
                     crc16(mac.rsrc), crc16(mac.data))
    struct.pack_into('>H', header, 110, crc16(bytes(header[:110])))

    return bytes(header) + rsrc_packed + data_packed, rsrc_method, data_method


def build_archive(mac, store_only=False):
    entry, rsrc_method, data_method = build_entry(mac, store_only)
    total = ARCHIVE_HEADER_LEN + len(entry)
    header = struct.pack('>4sHI4sB7x', b'SIT!', 1, total, b'rLau', 1)
    return header + entry, rsrc_method, data_method


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Pack a MacBinary (or two-fork) file into a .sit archive.")
    ap.add_argument('input', help="MacBinary file, e.g. build/mac/XPicoCode.bin")
    ap.add_argument('-o', '--output', help="archive to write (default: input with .sit)")
    ap.add_argument('--name', help="name to store, if not the one in the input")
    ap.add_argument('--flags', help="Finder flags to store, e.g. 0x2400")
    ap.add_argument('--no-bundle-bit', action='store_true',
                    help="do not add the bundle bit to an APPL")
    ap.add_argument('--store', action='store_true', help="no compression at all")
    args = ap.parse_args(argv)

    mac = read_input(args.input)
    if args.name:
        mac.name = args.name

    if args.flags is not None:
        mac.flags = int(args.flags, 0) & 0xFFFF
    elif mac.ftype == b'APPL' and not args.no_bundle_bit:
        # Rez writes the type and creator but no flags, so an app expanded from
        # the archive would show the generic application icon. The .dsk gets
        # this treatment from tools/set_bundle_bit.py; an archive has to carry
        # it in the entry header, because that is all Expander restores.
        mac.flags = (mac.flags | K_HAS_BUNDLE | K_HAS_CUSTOM_ICON) & ~K_HAS_BEEN_INITED

    out_path = args.output or os.path.splitext(args.input)[0] + '.sit'
    archive, rsrc_method, data_method = build_archive(mac, args.store)
    with open(out_path, 'wb') as fh:
        fh.write(archive)

    methods = {METHOD_STORED: 'stored', METHOD_RLE: 'rle'}
    print(f"{out_path}: {len(archive)} bytes")
    print(f"  {mac.name}  {mac.ftype.decode('mac_roman')}/"
          f"{mac.creator.decode('mac_roman')}  fdFlags=0x{mac.flags:04X}")
    print(f"  data fork {len(mac.data)} bytes, {methods[data_method]}")
    print(f"  rsrc fork {len(mac.rsrc)} bytes, {methods[rsrc_method]}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
