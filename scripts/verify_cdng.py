#!/usr/bin/env python3
"""Independent check of a CinemaDNG RAW-to-RAW conversion (no r3dss code takes part in judging).

Source and output frames are decoded with LibRaw (rawpy) and their tags read with tifffile. For a few
frames (first, middle and last by default) it checks:

  pixels    the output mosaic equals the documented per-CFA-plane resample of the source mosaic
            (docs/RESAMPLING.md: Lanczos-3 or area taps, clamp to edge, clip-aware fallback to area
            next to clipped highlights and wherever Lanczos would reach white-1, then rounded and
            clamped to [0, min(2^bits-1, white)]) within 1 code value; with a LinearizationTable the
            tolerance is half the table step at the expected value, plus 1
  structure output size, CFA pattern, black and white levels
  developed LibRaw-debayered output vs the debayered source scaled down: no colour-balance change and
            no sub-pixel shift per channel (a CFA phase error shows up as about 0.5-1 px)
  tags      every tag of the source frame (IFD0 and raw SubIFD merged, plus the EXIF IFD) must be in the
            output frame unchanged, except the size- and layout-dependent tags in ALLOWED_CHANGES;
            DefaultCropOrigin/Size must be the source values scaled to the output, DefaultScale must keep
            the displayed aspect

Needs: pip install rawpy numpy tifffile. Exit code 0 = passed, 1 = failed, 2 = usage error.
Usage: verify_cdng.py SOURCE_CLIP_DIR OUTPUT_CLIP_DIR [--kernel lanczos3|area] [--no-clip-aware]
                      [--frames 0,12,23] [--report out.json]
"""
import argparse, json, math, os, sys

import numpy as np
import rawpy
import tifffile

# Tags the output may change or drop: image size and storage layout, levels expressed per CFA site,
# the masked border (the output is cropped to the active area), resolution-dependent opcodes and noise
# statistics, and container structure. Framing tags are checked numerically instead.
ALLOWED_CHANGES = {
    254, 256, 257, 258, 259, 273, 277, 278, 279, 282, 283, 284, 296, 305, 322, 323, 324, 325, 330, 34665,
    50713, 50714, 50715, 50716, 50717,  # black/white level representation (checked through LibRaw)
    50781,  # RawDataUniqueID: the raw data did change
    50827,  # OriginalRawFileName
    50829, 50830,  # ActiveArea, MaskedAreas
    51008, 51009, 51022,  # OpcodeList1 (applied), OpcodeList2/3 (rescaled)
    51041,  # NoiseProfile
    50718, 50719, 50720,  # DefaultScale, DefaultCropOrigin, DefaultCropSize: checked below
    33421, 33422, 50710, 50711,  # CFA layout tags: checked through LibRaw's pattern
    50712,  # LinearizationTable: checked through LibRaw's linear values
}


def lanczos3(x):
    x = np.abs(x)
    out = np.zeros_like(x)
    m = (x < 3) & (x > 1e-9)
    px = np.pi * x[m]
    out[m] = 3 * np.sin(px) * np.sin(px / 3) / (px * px)
    out[x <= 1e-9] = 1.0
    return out


def axis_matrix(src_len, dst_len, offset, kernel):
    """Weights (dst plane x src plane) for one CFA plane along one axis (docs/RESAMPLING.md)."""
    sp, dp = src_len // 2, dst_len // 2
    s = src_len / dst_len
    se = max(1.0, s)
    m = np.zeros((dp, sp))
    for i in range(dp):
        t = ((2 * i + offset + 0.5) * s - offset - 0.5) / 2
        if kernel == "lanczos3":
            n = np.arange(math.ceil(t - 3 * se), math.floor(t + 3 * se) + 1)
            w = lanczos3((n - t) / se)
        else:
            lo, hi = t - se / 2, t + se / 2
            n0 = math.floor(lo + 0.5)
            n = np.arange(n0, max(n0, math.ceil(hi - 0.5)) + 1)
            w = np.clip(np.minimum(hi, n + 0.5) - np.maximum(lo, n - 0.5), 0, None)
        np.add.at(m[i], np.clip(n, 0, sp - 1), w)
        m[i] /= m[i].sum()
    return m


def expected_mosaic(src, white, max_code, out_h, out_w, kernel, clip_aware):
    h, w = src.shape
    ref = np.zeros((out_h, out_w))
    thr = white - 1
    for py in (0, 1):
        for px in (0, 1):
            plane = src[py::2, px::2].astype(np.float64)
            my, mx = axis_matrix(h, out_h, py, kernel), axis_matrix(w, out_w, px, kernel)
            main = my @ plane @ mx.T
            if clip_aware and kernel != "area":
                ay, ax = axis_matrix(h, out_h, py, "area"), axis_matrix(w, out_w, px, "area")
                clipped = (plane >= thr).astype(np.float64)
                touch = ((my != 0).astype(np.float64) @ clipped @ (mx != 0).astype(np.float64).T) > 0
                main = np.where(touch | (main >= thr), ay @ plane @ ax.T, main)
            ref[py::2, px::2] = main
    return np.clip(np.rint(ref), 0, min(max_code, white))


def phase_shift(a, b):
    fa, fb = np.fft.fft2(a - a.mean()), np.fft.fft2(b - b.mean())
    r = fa * np.conj(fb)
    r /= np.abs(r) + 1e-12
    c = np.fft.ifft2(r).real
    y, x = np.unravel_index(np.argmax(c), c.shape)
    h, w = c.shape

    def sub(c1, c0, c2):
        d = c1 - 2 * c0 + c2
        return 0.0 if d == 0 else 0.5 * (c1 - c2) / d

    dy = sub(c[(y - 1) % h, x], c[y, x], c[(y + 1) % h, x])
    dx = sub(c[y, (x - 1) % w], c[y, x], c[y, (x + 1) % w])
    y = y - h if y > h // 2 else y
    x = x - w if x > w // 2 else x
    return float(y + dy), float(x + dx)


def raw_page(t):
    """The CFA raw image page: IFD0, or its SubIFD when IFD0 is a preview."""
    p0 = t.pages[0]
    if p0.tags.get(262) is not None and p0.tags.get(262).value == 32803:
        return p0
    for sp in getattr(p0, "pages", []) or []:
        if sp.tags.get(262) is not None and sp.tags.get(262).value == 32803:
            return sp
    return p0


def norm(v):
    if isinstance(v, (bytes, bytearray)):
        return bytes(v).hex()
    if hasattr(v, "tolist"):
        v = v.tolist()
    if isinstance(v, (tuple, list)):
        return [norm(x) for x in v]
    if hasattr(v, "value") and not isinstance(v, (int, float, str)):
        return v.value  # enums
    return v


def tag_map(path):
    """{code: value} of IFD0 overridden by the raw SubIFD, and {code: value} of the EXIF IFD."""
    with tifffile.TiffFile(path) as t:
        p0 = t.pages[0]
        tags = {tg.code: norm(tg.value) for tg in p0.tags.values() if tg.code != 34665}
        rp = raw_page(t)
        if rp is not p0:
            tags.update({tg.code: norm(tg.value) for tg in rp.tags.values()})
        exif = {}
        e = p0.tags.get(34665)
        if e is not None and isinstance(e.value, dict):
            exif = {k: norm(v) for k, v in e.value.items()}
        return tags, exif


def as_pair(v, count=2):
    """Two numbers from a tag that may be SHORT/LONG (2 values) or RATIONAL (4 values)."""
    v = v if isinstance(v, list) else [v]
    if len(v) == 2 * count:
        return [v[i] / v[i + 1] for i in range(0, len(v), 2)]
    return [float(x) for x in v[:count]]


def check_tags(st, ot, sexif, oexif, sx, sy, src_wh, out_wh, problems, k):
    for code, val in st.items():
        if code in ALLOWED_CHANGES:
            continue
        if code not in ot:
            problems.append(f"frame {k}: tag {code} dropped (source {str(val)[:60]})")
        elif ot[code] != val:
            problems.append(f"frame {k}: tag {code} changed: {str(val)[:60]} -> {str(ot[code])[:60]}")
    for code, val in sexif.items():
        if code in oexif and oexif[code] != val:
            problems.append(f"frame {k}: EXIF {code} changed: {str(val)[:40]} -> {str(oexif[code])[:40]}")
        elif code not in oexif and code not in (37500, 40965, 40961, 41728, 41729):  # MakerNote / pointers
            problems.append(f"frame {k}: EXIF {code} dropped")
    if 50719 in st and 50720 in st:
        if 50719 not in ot or 50720 not in ot:
            problems.append(f"frame {k}: DefaultCrop dropped")
        else:
            so, ss = as_pair(st[50719]), as_pair(st[50720])
            oo, os_ = as_pair(ot[50719]), as_pair(ot[50720])
            for name, a, b in (("origin", so, oo), ("size", ss, os_)):
                want = [a[0] * sx, a[1] * sy]
                if any(abs(x - y) > 1.01 for x, y in zip(want, b)):
                    problems.append(f"frame {k}: DefaultCrop{name} {b} != scaled source {want}")
    sscale = as_pair(st[50718]) if 50718 in st else [1.0, 1.0]
    oscale = as_pair(ot[50718]) if 50718 in ot else [1.0, 1.0]
    src_aspect = src_wh[0] * sscale[0] / (src_wh[1] * sscale[1])
    out_aspect = out_wh[0] * oscale[0] / (out_wh[1] * oscale[1])
    if abs(out_aspect / src_aspect - 1) > 0.005:
        problems.append(f"frame {k}: displayed aspect {out_aspect:.4f} != source {src_aspect:.4f} (DefaultScale)")


def decode(path):
    with rawpy.imread(path) as r:
        s = r.sizes
        vis = r.raw_image_visible.copy()
        # r3dss returns LibRaw's visible area with its origin moved to an even site and an odd trailing
        # row/column dropped (CFA phase is anchored at the visible origin).
        oy, ox = s.top_margin % 2, s.left_margin % 2
        vis = vis[oy:, ox:]
        vis = vis[: vis.shape[0] // 2 * 2, : vis.shape[1] // 2 * 2]
        rgb = r.postprocess(use_camera_wb=True, no_auto_bright=True, output_bps=16, gamma=(1, 1),
                            output_color=rawpy.ColorSpace.raw, user_flip=0).astype(np.float64)
        return {"raw": vis, "pattern": r.raw_pattern.tolist(), "white": int(r.white_level),
                "black": [int(v) for v in r.black_level_per_channel], "rgb": rgb, "shift": (oy, ox)}


def linearization_step(path):
    """Per-value tolerance for a re-encoded LinearizationTable: half the table step at each value."""
    with tifffile.TiffFile(path) as t:
        tg = raw_page(t).tags.get(50712)
        if tg is None:
            return None
        table = np.asarray(tg.value, dtype=np.float64)
    steps = np.diff(table, append=table[-1])
    tol = np.zeros(65536)
    idx = np.searchsorted(table, np.arange(65536), side="left").clip(0, len(table) - 1)
    tol[:] = steps[idx] / 2 + 1
    return tol



def dng_frames(folder):
    # One listing, matched case-insensitively: globbing "*.dng" and "*.DNG" separately counts every
    # frame twice on Windows, where glob ignores case. AppleDouble "._*" files are not frames.
    if not os.path.isdir(folder):
        return []
    return sorted(os.path.join(folder, n) for n in os.listdir(folder)
                  if n.lower().endswith(".dng") and not n.startswith("."))

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("source")
    ap.add_argument("output")
    ap.add_argument("--kernel", choices=["lanczos3", "area"], default="lanczos3")
    ap.add_argument("--no-clip-aware", action="store_true")
    ap.add_argument("--frames", default="", help="comma-separated frame indices (default: first, middle, last)")
    ap.add_argument("--report", default="")
    a = ap.parse_args()
    src, out = dng_frames(a.source), dng_frames(a.output)
    if not src or not out:
        print(f"no DNG frames found (source {len(src)}, output {len(out)})", file=sys.stderr)
        return 2
    problems, frames = [], []
    if len(src) != len(out):
        problems.append(f"frame count {len(out)} != source {len(src)}")
    n = min(len(src), len(out))
    idx = sorted({int(v) for v in a.frames.split(",")}) if a.frames else sorted({0, n // 2, n - 1})
    for k in idx:
        if k >= n:
            problems.append(f"frame {k} does not exist")
            continue
        s, o = decode(src[k]), decode(out[k])
        f = {"frame": k, "source": os.path.basename(src[k]), "output": os.path.basename(out[k])}
        sh, sw = s["raw"].shape
        oh, ow = o["raw"].shape
        f["size"] = f"{sw}x{sh} -> {ow}x{oh}"
        for key in ("pattern", "black", "white"):
            if s[key] != o[key]:
                problems.append(f"frame {k}: {key} {o[key]} != source {s[key]}")
        with tifffile.TiffFile(out[k]) as t:
            bits = int(raw_page(t).tags.get(258).value)
        lin = linearization_step(out[k])
        max_code = 65535 if lin is not None else (1 << bits) - 1
        ref = expected_mosaic(s["raw"], s["white"], max_code, oh, ow, a.kernel, not a.no_clip_aware)
        d = np.abs(ref - o["raw"].astype(np.float64))
        tol = lin[ref.astype(np.int64)] if lin is not None else 1
        bad = d > tol
        f["max_abs_diff"], f["sites_over_tolerance"] = float(d.max()), int(bad.sum())
        if bad.any():
            problems.append(f"frame {k}: output mosaic differs from the expected resample by up to {d.max():.0f} "
                            f"code values at {int(bad.sum())} of {d.size} sites")
        if o["raw"].max() == o["raw"].min() and s["raw"].max() != s["raw"].min():
            problems.append(f"frame {k}: output mosaic is flat ({int(o['raw'].min())})")
        # Developed comparison (skipped for tiny frames, where borders dominate).
        if min(oh, ow) >= 64:
            srgb = s["rgb"][s["shift"][0]: s["shift"][0] + sh, s["shift"][1]: s["shift"][1] + sw]
            if srgb.shape[0] % oh == 0 and srgb.shape[1] % ow == 0:
                fy, fx = srgb.shape[0] // oh, srgb.shape[1] // ow
                down = srgb.reshape(oh, fy, ow, fx, 3).mean(axis=(1, 3))
            else:
                yy = np.clip(((np.arange(oh) + 0.5) * srgb.shape[0] / oh - 0.5).round().astype(int), 0, srgb.shape[0] - 1)
                xx = np.clip(((np.arange(ow) + 0.5) * srgb.shape[1] / ow - 0.5).round().astype(int), 0, srgb.shape[1] - 1)
                down = srgb[yy][:, xx]
            orgb = o["rgb"][: down.shape[0], : down.shape[1]]
            b = max(8, min(oh, ow) // 32)
            dv, ov = down[b:-b, b:-b], orgb[b:-b, b:-b]
            f["channel_ratio"] = [round(float(ov[..., c].mean() / max(dv[..., c].mean(), 1e-9)), 4) for c in range(3)]
            f["shift_px"] = [[round(v, 3) for v in phase_shift(ov[..., c], dv[..., c])] for c in range(3)]
            if any(abs(r - 1) > 0.02 for r in f["channel_ratio"]):
                problems.append(f"frame {k}: colour balance changed by {f['channel_ratio']}")
            if max(abs(v) for pair in f["shift_px"] for v in pair) > 0.25:
                problems.append(f"frame {k}: image shifted by {f['shift_px']} px (CFA phase error)")
        st, sexif = tag_map(src[k])
        ot, oexif = tag_map(out[k])
        check_tags(st, ot, sexif, oexif, ow / sw, oh / sh, (sw, sh), (ow, oh), problems, k)
        frames.append(f)
    verdict = "PASSED" if not problems else "FAILED"
    report = {"verdict": verdict, "source_frames": len(src), "output_frames": len(out), "frames": frames,
              "problems": problems, "decoder": f"LibRaw {'.'.join(map(str, rawpy.libraw_version))}"}
    for f in frames:
        print(f"frame {f['frame']}: {f['size']}, max diff {f['max_abs_diff']:.0f}, "
              f"colour {f.get('channel_ratio', '-')}, shift {f.get('shift_px', '-')}")
    for p in problems:
        print("PROBLEM:", p)
    print(f"{verdict}: {len(frames)} frames checked with {report['decoder']}")
    if a.report:
        with open(a.report, "w") as fh:
            json.dump(report, fh, indent=2)
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
