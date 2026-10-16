#!/usr/bin/env python3
"""Writes a small CinemaDNG clip with tifffile, an independent TIFF/DNG writer (no r3dss code), as a
third-party source for acceptance: big-endian, uncompressed, one strip per frame, a non-RGGB CFA, black
and white levels, colour matrix, white balance, frame rate and per-frame timecode, plus a WAV.
make_foreign_clip.py OUT_DIR [--frames 3] [--width 640] [--height 360] [--bits 12] [--cfa GRBG]
Needs: pip install numpy tifffile."""
import argparse, os, struct

import numpy as np
import tifffile

CFA = {"RGGB": (0, 1, 1, 2), "BGGR": (2, 1, 1, 0), "GRBG": (1, 0, 2, 1), "GBRG": (1, 2, 0, 1)}

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--frames", type=int, default=3)
ap.add_argument("--width", type=int, default=640)
ap.add_argument("--height", type=int, default=360)
ap.add_argument("--bits", type=int, default=12)
ap.add_argument("--cfa", default="GRBG", choices=sorted(CFA))
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
name = os.path.basename(os.path.normpath(a.out))
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:a.height, 0:a.width]
top = (1 << a.bits) - 1
black = 64 << max(0, a.bits - 10)


def srat(vals, den=10000):
    out = []
    for v in vals:
        out += [int(round(v * den)), den]
    return out


for i in range(a.frames):
    base = 0.1 + 0.6 * xx / a.width * (1 - 0.4 * yy / a.height) + \
        0.2 * np.exp(-((xx - a.width / 2 - 8 * i) ** 2 + (yy - a.height / 2) ** 2) / 2000.0)
    m = np.clip(black + base * (top - black) + rng.normal(0, 0.004 * top, base.shape), 0, top).astype(">u2")
    tags = [
        (33421, "H", 2, (2, 2), True),                        # CFARepeatPatternDim
        (33422, "B", 4, CFA[a.cfa], True),                    # CFAPattern
        (50706, "B", 4, (1, 4, 0, 0), True),                  # DNGVersion
        (50707, "B", 4, (1, 1, 0, 0), True),                  # DNGBackwardVersion
        (271, "s", 0, "ForeignCam", True),                    # Make
        (272, "s", 0, "tifffile writer", True),               # Model
        (50708, "s", 0, "ForeignCam tifffile", True),         # UniqueCameraModel
        (50714, "H", 1, (black,), True),                      # BlackLevel
        (50717, "H", 1, (top,), True),                        # WhiteLevel
        (50721, "2i", 9, srat([0.7, -0.2, -0.05, -0.4, 1.2, 0.2, -0.05, 0.15, 0.6]), True),  # ColorMatrix1
        (50778, "H", 1, (21,), True),                         # CalibrationIlluminant1
        (50728, "2I", 3, srat([0.5, 1.0, 0.7], 1000000), True),  # AsShotNeutral
        (50730, "2i", 1, srat([-0.5]), True),                 # BaselineExposure
        (51044, "2i", 1, (24000, 1001), True),                # FrameRate
        (51043, "B", 8, (0x10 + i, 0x59, 0x59, 0x23, 0, 0, 0, 0), True),  # TimeCodes
    ]
    with tifffile.TiffWriter(os.path.join(a.out, f"{name}_{i:06d}.dng"), byteorder=">") as tw:
        tw.write(m, photometric=32803, compression=None, extratags=tags, metadata=None, rowsperstrip=a.height,
                 subfiletype=0, software=False)
# A 16-bit stereo WAV sidecar matching the clip length.
rate, n = 48000, int(round(a.frames * 48000 * 1001 / 24000))
pcm = (np.sin(np.arange(n * 2) * 0.01) * 12000).astype("<i2").tobytes()
with open(os.path.join(a.out, name + ".wav"), "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16))
    f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)
print(a.out)
