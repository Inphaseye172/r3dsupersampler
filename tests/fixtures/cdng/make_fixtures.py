"""Builds the tiny CinemaDNG golden fixtures in this folder and prints LibRaw's view of each one.

Every fixture is written by an encoder that shares no code with r3dss (pidng, tifffile, or the
small LJ92 encoder below) and decoded by LibRaw (rawpy). tests/test_cdng_golden.cpp checks that
the r3dss reader returns exactly LibRaw's mosaic for each of them: the SHA-256 of the visible area
(cropped to even width/height, as r3dss does), as little-endian uint16, plus size, CFA, per-site
black and white level.

    python -I make_fixtures.py OUTDIR      (needs numpy, rawpy, tifffile, imagecodecs, pidng)

Each fixture is a one-frame clip folder OUTDIR/<name>/<name>_000000.dng of a few KB. The script
prints the C++ table rows for tests/test_cdng_golden.cpp.
"""
import hashlib
import os
import struct
import sys

import numpy as np
import rawpy
import tifffile

CFA = {"RGGB": [0, 1, 1, 2], "BGGR": [2, 1, 1, 0], "GRBG": [1, 0, 2, 1], "GBRG": [1, 2, 0, 1]}


def mosaic(h, w, bits, seed):
    """Smooth gradient + noise with distinct per-site offsets (plane mix-ups change the hash)."""
    rng = np.random.default_rng(seed)
    maxv = (1 << bits) - 1
    y, x = np.mgrid[0:h, 0:w]
    m = maxv * (0.15 + 0.6 * (x / w) * (0.5 + 0.5 * y / h)) + rng.normal(0, maxv * 0.02, (h, w))
    m[0::2, 1::2] *= 0.85
    m[1::2, 0::2] *= 0.93
    return np.clip(np.round(m), 0, maxv).astype(np.uint16)


def dng_tags(cfa, white, black=None, black_repeat=None, extra=()):
    """DNG tags for tifffile's extratags: (code, dtype, count, value, writeonce)."""
    t = [
        (33421, "H", 2, (2, 2), True),  # CFARepeatPatternDim
        (33422, "B", 4, bytes(CFA[cfa]), True),  # CFAPattern
        (50706, "B", 4, bytes([1, 4, 0, 0]), True),  # DNGVersion
        (50707, "B", 4, bytes([1, 1, 0, 0]), True),  # DNGBackwardVersion
        (50708, "s", 0, "GoldenCam", True),  # UniqueCameraModel
        (50710, "B", 3, bytes([0, 1, 2]), True),  # CFAPlaneColor
        (50711, "H", 1, 1, True),  # CFALayout
        (50717, "I", 1, white, True),  # WhiteLevel
        (50721, "2i", 9, (10000, 10000, 0, 10000, 0, 10000, 0, 10000, 10000, 10000, 0, 10000, 0, 10000, 0, 10000,
                          10000, 10000), True),  # ColorMatrix1
        (50778, "H", 1, 21, True),  # CalibrationIlluminant1
    ]
    if black is not None:
        if black_repeat:
            t.append((50713, "H", 2, tuple(black_repeat), True))
        t.append((50714, "I", len(black), tuple(black), True))
    t.extend(extra)
    return t


def write_tiff(path, codes, bits, cfa, white, byteorder="<", rowsperstrip=None, tile=None, **kw):
    tifffile.imwrite(path, codes, byteorder=byteorder, photometric=32803, bitspersample=bits, compression=None,
                     rowsperstrip=rowsperstrip, tile=tile, metadata=None, extratags=dng_tags(cfa, white, **kw))


# --- minimal LJ92 encoder (T.81 process 14, predictor 1) with restart markers --------------------
def lj92(samples, ncomp, precision, restart_rows):
    s = samples.astype(np.int64)
    h, w = s.shape
    cols = w // ncomp
    init = 1 << (precision - 1)
    diffs = []
    for y in range(h):
        first = y % restart_rows == 0 if restart_rows else y == 0
        row = []
        for x in range(w):
            if x < ncomp:
                pred = init if first else s[y - 1, x]
            else:
                pred = s[y, x - ncomp]
            d = (s[y, x] - pred) & 0xFFFF
            row.append(d - 65536 if d > 32768 else d)
        diffs.append(row)
    # fixed table: category k has a 5-bit code k (17 symbols, Kraft sum < 1)
    out = bytearray(b"\xff\xd8\xff\xc3" + struct.pack(">HBHHB", 8 + 3 * ncomp, precision, h, cols, ncomp))
    for c in range(ncomp):
        out += bytes([c + 1, 0x11, 0])
    out += b"\xff\xc4" + struct.pack(">HB", 2 + 17 + 17, 0) + bytes([0, 0, 0, 0, 17] + [0] * 11) + bytes(range(17))
    if restart_rows:
        out += b"\xff\xdd" + struct.pack(">HH", 4, restart_rows * cols)
    out += b"\xff\xda" + struct.pack(">HB", 6 + 2 * ncomp, ncomp)
    for c in range(ncomp):
        out += bytes([c + 1, 0])
    out += bytes([1, 0, 0])
    bitbuf = []

    def flush():
        nonlocal out
        while len(bitbuf) % 8:
            bitbuf.append(1)
        for i in range(0, len(bitbuf), 8):
            b = int("".join(map(str, bitbuf[i:i + 8])), 2)
            out.append(b)
            if b == 0xFF:
                out.append(0)
        bitbuf.clear()

    rst = 0
    for y in range(h):
        if restart_rows and y and y % restart_rows == 0:
            flush()
            out += bytes([0xFF, 0xD0 + (rst & 7)])
            rst += 1
        for d in diffs[y]:
            cat = 16 if d == 32768 else int(abs(d)).bit_length()
            bitbuf.extend(int(c) for c in format(cat, "05b"))
            if 0 < cat < 16:
                v = d - 1 if d < 0 else d
                bitbuf.extend(int(c) for c in format(v & ((1 << cat) - 1), "0%db" % cat))
    flush()
    out += b"\xff\xd9"
    return bytes(out)


def write_lj92_tiles(path, codes, bits, cfa, white, tile, restart_rows):
    """Tiled LJ92 DNG assembled by hand (little-endian TIFF)."""
    h, w = codes.shape
    tw, th = tile
    streams = []
    for y0 in range(0, h, th):
        for x0 in range(0, w, tw):
            blk = np.zeros((th, tw), np.uint16)
            part = codes[y0:y0 + th, x0:x0 + tw]
            blk[:part.shape[0], :part.shape[1]] = part
            blk[:, part.shape[1]:] = blk[:, part.shape[1] - 1:part.shape[1]]
            blk[part.shape[0]:, :] = blk[part.shape[0] - 1:part.shape[0], :]
            streams.append(lj92(blk, 2, bits, restart_rows))
    buf = bytearray(b"II*\0\0\0\0\0")
    offs = []
    for s in streams:
        if len(buf) & 1:
            buf.append(0)
        offs.append(len(buf))
        buf += s
    entries = {
        254: (4, [0]), 256: (4, [w]), 257: (4, [h]), 258: (3, [bits]), 259: (3, [7]), 262: (3, [32803]),
        277: (3, [1]), 322: (4, [tw]), 323: (4, [th]), 324: (4, offs), 325: (4, [len(s) for s in streams]),
        33421: (3, [2, 2]), 33422: (1, CFA[cfa]), 50706: (1, [1, 4, 0, 0]), 50717: (4, [white]),
        50721: (10, [(1, 1), (0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (0, 1), (0, 1), (1, 1)]), 50778: (3, [21]),
    }
    fmt = {1: "B", 3: "H", 4: "I"}
    packed = []
    for tag in sorted(entries):
        typ, vals = entries[tag]
        if typ == 10:
            data = b"".join(struct.pack("<ii", *v) for v in vals)
        else:
            data = struct.pack("<%d%s" % (len(vals), fmt[typ]), *vals)
        off = None
        if len(data) > 4:
            if len(buf) & 1:
                buf.append(0)
            off = len(buf)
            buf += data
        packed.append((tag, typ, len(vals), data, off))
    if len(buf) & 1:
        buf.append(0)
    ifd = len(buf)
    buf += struct.pack("<H", len(packed))
    for tag, typ, cnt, data, off in packed:
        buf += struct.pack("<HHI", tag, typ, cnt) + (data.ljust(4, b"\0") if off is None else struct.pack("<I", off))
    buf += b"\0\0\0\0"
    buf[4:8] = struct.pack("<I", ifd)
    with open(path, "wb") as f:
        f.write(buf)


def write_pidng(path, codes, bits, cfa, white, compress):
    from pidng.core import RAW2DNG, DNGTags, Tag
    from pidng.defs import CalibrationIlluminant, DNGVersion, PhotometricInterpretation
    h, w = codes.shape
    t = DNGTags()
    t.set(Tag.ImageWidth, w)
    t.set(Tag.ImageLength, h)
    t.set(Tag.TileWidth, w)
    t.set(Tag.TileLength, h)
    t.set(Tag.PhotometricInterpretation, PhotometricInterpretation.Color_Filter_Array)
    t.set(Tag.SamplesPerPixel, 1)
    t.set(Tag.BitsPerSample, bits)
    t.set(Tag.CFARepeatPatternDim, [2, 2])
    t.set(Tag.CFAPattern, CFA[cfa])
    t.set(Tag.BlackLevel, [64 << (bits - 12)] if bits >= 12 else [16])
    t.set(Tag.WhiteLevel, [white])
    t.set(Tag.ColorMatrix1, [[1, 1], [0, 1], [0, 1], [0, 1], [1, 1], [0, 1], [0, 1], [0, 1], [1, 1]])
    t.set(Tag.CalibrationIlluminant1, CalibrationIlluminant.D65)
    t.set(Tag.AsShotNeutral, [[1, 2], [1, 1], [3, 4]])
    t.set(Tag.Make, "GoldenCam")
    t.set(Tag.Model, "pidng")
    t.set(Tag.DNGVersion, DNGVersion.V1_4)
    t.set(Tag.DNGBackwardVersion, DNGVersion.V1_2)
    r = RAW2DNG()
    r.options(t, path=os.path.dirname(path), compress=compress)
    r.convert(codes, filename=os.path.splitext(os.path.basename(path))[0])


def lin_table(bits):
    x = np.arange(1 << bits, dtype=np.float64) / ((1 << bits) - 1)
    return np.round(x * x * 65535).astype(np.uint16)


# name -> writer. The mosaics are deterministic, so INTENDED below can rebuild what each one stores.
FIXTURES = {
    "pidng_lj12_rggb": lambda p: write_pidng(p, mosaic(32, 64, 12, 1), 12, "RGGB", 4095, True),
    "pidng_u12_bggr": lambda p: write_pidng(p, mosaic(32, 64, 12, 2), 12, "BGGR", 4095, False),
    "tiff_be16_strips_grbg_odd": lambda p: write_tiff(p, mosaic(33, 51, 16, 3), 16, "GRBG", 65535, byteorder=">",
                                                      rowsperstrip=5, black=[1000], black_repeat=(1, 1)),
    "tiff_le14_tiles_gbrg": lambda p: write_tiff(p, mosaic(38, 50, 14, 4), 14, "GBRG", 16383, tile=(16, 16),
                                                 black=[500, 510, 520, 530], black_repeat=(2, 2)),
    "tiff_le10_w66_strips": lambda p: write_tiff(p, mosaic(24, 66, 10, 5), 10, "RGGB", 1023, rowsperstrip=7),
    "tiff_be16_tiles_bggr": lambda p: write_tiff(p, mosaic(40, 48, 16, 6), 16, "BGGR", 65535, byteorder=">",
                                                 tile=(16, 16), black=[3000, 3100], black_repeat=(2, 1)),
    "tiff_aa_odd_lin12": lambda p: write_tiff(
        p, mosaic(32, 64, 12, 7), 12, "RGGB", 65535, black=[2000, 2100, 2200, 2300], black_repeat=(2, 2),
        extra=[(50829, "I", 4, (1, 3, 31, 61), True), (50712, "H", 4096, tuple(int(v) for v in lin_table(12)), True)]),
    "tiff_aa_even_u16": lambda p: write_tiff(p, mosaic(40, 72, 16, 8), 16, "GBRG", 60000,
                                             extra=[(50829, "I", 4, (4, 8, 36, 64), True)]),
    "lj92_restart_tiles": lambda p: write_lj92_tiles(p, mosaic(32, 64, 14, 9), 14, "GRBG", 16383, (16, 16), 1),
}


# What a correct decoder returns for each fixture: the linear values of the visible area (the
# ActiveArea moved to an even origin, as LibRaw does), before the r3dss crop to even size.
INTENDED = {
    "pidng_lj12_rggb": lambda: mosaic(32, 64, 12, 1),
    "pidng_u12_bggr": lambda: mosaic(32, 64, 12, 2),
    "tiff_be16_strips_grbg_odd": lambda: mosaic(33, 51, 16, 3),
    "tiff_le14_tiles_gbrg": lambda: mosaic(38, 50, 14, 4),
    "tiff_le10_w66_strips": lambda: mosaic(24, 66, 10, 5),
    "tiff_be16_tiles_bggr": lambda: mosaic(40, 48, 16, 6),
    "tiff_aa_odd_lin12": lambda: lin_table(12)[mosaic(32, 64, 12, 7)][2:31, 4:61],
    "tiff_aa_even_u16": lambda: mosaic(40, 72, 16, 8)[4:36, 8:64],
    "lj92_restart_tiles": lambda: mosaic(32, 64, 14, 9),
}


def libraw_view(path):
    with rawpy.imread(path) as r:
        vis = r.raw_image_visible.copy()
        pat = r.raw_pattern
        blpc = list(r.black_level_per_channel)
        white = int(r.white_level)
        desc = r.color_desc.decode()
    h, w = vis.shape
    vis = vis[: h & ~1, : w & ~1]
    names = {"R": 0, "G": 1, "B": 2}
    colors = [names[desc[pat[y][x]]] for y in range(2) for x in range(2)]
    cfa = next(k for k, v in CFA.items() if v == colors)
    black = [int(blpc[pat[y][x]]) for y in range(2) for x in range(2)]
    sha = hashlib.sha256(vis.astype("<u2").tobytes()).hexdigest()
    return dict(w=vis.shape[1], h=vis.shape[0], cfa=cfa, black=black, white=white, sha=sha, mosaic=vis)


def main():
    out = sys.argv[1]
    for name, make in FIXTURES.items():
        d = os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        p = os.path.join(d, name + "_000000.dng")
        make(p)
        v = libraw_view(p)
        want = INTENDED[name]()
        want = want[: want.shape[0] & ~1, : want.shape[1] & ~1]
        if v["mosaic"].shape != want.shape or not np.array_equal(v["mosaic"], want):
            sys.exit("%s: LibRaw does not return the intended mosaic; not a usable golden fixture" % name)
        print('    {"%s", %d, %d, CfaPattern::%s, {%s}, %d, "%s"},  // %d bytes'
              % (name, v["w"], v["h"], v["cfa"], ", ".join(map(str, v["black"])), v["white"], v["sha"],
                 os.path.getsize(p)))


if __name__ == "__main__":
    main()
