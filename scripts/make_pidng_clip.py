#!/usr/bin/env python3
"""Writes a small CinemaDNG clip with PiDNG, an independent DNG writer, as a third-party source for
acceptance tests: make_pidng_clip.py OUT_DIR [--frames 3] [--width 640] [--height 360] [--bits 12]
[--cfa GRBG] [--compress]. Needs: pip install pidng numpy."""
import argparse, os, struct

import numpy as np
from pidng.core import RAW2DNG, DNGTags, Tag
from pidng.defs import CFAPattern, CalibrationIlluminant, DNGVersion, Orientation, PhotometricInterpretation

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--frames", type=int, default=3)
ap.add_argument("--width", type=int, default=640)
ap.add_argument("--height", type=int, default=360)
ap.add_argument("--bits", type=int, default=12)
ap.add_argument("--cfa", default="GRBG")
ap.add_argument("--compress", action="store_true")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
name = os.path.basename(os.path.normpath(a.out))
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:a.height, 0:a.width]
top = (1 << a.bits) - 1
for i in range(a.frames):
    base = 0.1 + 0.6 * xx / a.width * (1 - 0.4 * yy / a.height) + 0.2 * np.exp(-((xx - a.width / 2 - 8 * i) ** 2 + (yy - a.height / 2) ** 2) / 2000.0)
    m = np.clip(base * top + rng.normal(0, 0.004 * top, base.shape), 0, top).astype(np.uint16)
    t = DNGTags()
    t.set(Tag.ImageWidth, a.width)
    t.set(Tag.ImageLength, a.height)
    t.set(Tag.TileWidth, a.width)
    t.set(Tag.TileLength, a.height)
    t.set(Tag.Orientation, Orientation.Horizontal)
    t.set(Tag.PhotometricInterpretation, PhotometricInterpretation.Color_Filter_Array)
    t.set(Tag.SamplesPerPixel, 1)
    t.set(Tag.BitsPerSample, a.bits)
    t.set(Tag.CFARepeatPatternDim, [2, 2])
    t.set(Tag.CFAPattern, getattr(CFAPattern, a.cfa))
    t.set(Tag.BlackLevel, 64 << max(0, a.bits - 10))
    t.set(Tag.WhiteLevel, top)
    t.set(Tag.ColorMatrix1, [[7000, 10000], [-2000, 10000], [-500, 10000], [-4000, 10000], [12000, 10000],
                             [2000, 10000], [-500, 10000], [1500, 10000], [6000, 10000]])
    t.set(Tag.CalibrationIlluminant1, CalibrationIlluminant.D65)
    t.set(Tag.AsShotNeutral, [[1, 2], [1, 1], [7, 10]])
    t.set(Tag.Make, "PiCam")
    t.set(Tag.Model, "PiDNG test")
    t.set(Tag.UniqueCameraModel, "PiDNG test")
    t.set(Tag.DNGVersion, DNGVersion.V1_4)
    t.set(Tag.DNGBackwardVersion, DNGVersion.V1_1)
    t.set(Tag.FrameRate, [[24000, 1001]])
    t.set(Tag.TimeCodes, [0x10 + i, 0x59, 0x59, 0x23, 0, 0, 0, 0])
    r = RAW2DNG()
    r.options(t, path=a.out, compress=a.compress)
    r.convert(m, filename=f"{name}_{i:06d}")
# A 16-bit stereo WAV sidecar matching the clip length.
rate, n = 48000, int(round(a.frames * 48000 * 1001 / 24000))
pcm = (np.sin(np.arange(n * 2) * 0.01) * 12000).astype("<i2").tobytes()
with open(os.path.join(a.out, name + ".wav"), "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, rate, rate * 4, 4, 16))
    f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)
print(a.out)
