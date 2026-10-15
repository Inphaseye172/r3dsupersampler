#!/usr/bin/env python3
"""Survey a RED-container clip (.R3D, R3D NE, and NEV if it shares the wrapper).

Walks the chunk list, prints per-tag statistics, extracts readable metadata
strings, and scans one video frame for JPEG XS marker segments so we can tell a
Nikon TicoRAW / JPEG XS essence from REDCODE RAW.

Findings so far are recorded in docs/CODESTREAM.md.

Container rules learned from a RED KOMODO 6K clip (2026-10-14):
  * Chunk header = 4-byte big-endian size, 3 ASCII tag bytes, 1 version byte.
    (The repo's red_container.cpp treats the 4th byte as ASCII and stops.)
  * Chunks are 4096-byte aligned; the gap is zero-filled.
  * Tags seen: RED (file header), RDI (video frame), RDA (audio), RDX, RII,
    RIA, RIX (indexes), REO (end-of-file header copy).

Usage: r3d_survey.py CLIP [--frame N] [--dump frame.bin] [--json chunks.json]
"""
import argparse, collections, json, math, re, struct, sys

ALIGN = 4096
JXS_MARKERS = [("SOC", b"\xff\x10"), ("CAP", b"\xff\x50"), ("PIH", b"\xff\x12"),
               ("CDT", b"\xff\x13"), ("WGT", b"\xff\x14"), ("NLT", b"\xff\x16"),
               ("CWD", b"\xff\x17"), ("CTS", b"\xff\x18"), ("SLH", b"\xff\x20"),
               ("EOC", b"\xff\x11")]


def walk(f, size):
    off, out = 0, []
    while off + 8 <= size:
        f.seek(off)
        h = f.read(8)
        if len(h) < 8:
            break
        if h == b"\0" * 8:
            off = (off // ALIGN + 1) * ALIGN
            continue
        n = struct.unpack(">I", h[:4])[0]
        tag, ver = h[4:7].decode("latin1"), h[7]
        if n < 8 or off + n > size or not all(0x20 <= c < 0x7F for c in h[4:7]):
            print(f"stop: unparseable header at {off}: {h.hex()}", file=sys.stderr)
            break
        out.append({"offset": off, "size": n, "tag": tag, "ver": ver})
        off += n
    return out, off


def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    return -sum(k / len(b) * math.log2(k / len(b)) for k in c.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--dump")
    ap.add_argument("--json")
    a = ap.parse_args()
    f = open(a.clip, "rb")
    f.seek(0, 2)
    size = f.tell()
    chunks, end = walk(f, size)
    print(f"file {size} bytes, {len(chunks)} chunks, parsed to {end} ({'complete' if end == size else 'INCOMPLETE'})")
    by = collections.defaultdict(list)
    for c in chunks:
        by[(c["tag"], c["ver"])].append(c["size"])
    for (t, v), sz in sorted(by.items(), key=lambda kv: -sum(kv[1])):
        print(f"  {t}v{v}: n={len(sz)} min={min(sz)} max={max(sz)} avg={sum(sz)//len(sz)} total={sum(sz)}")
    for c in chunks:
        if c["tag"] in ("RED", "REO"):
            f.seek(c["offset"])
            d = f.read(min(c["size"], 4096))
            strs = [s.decode() for s in re.findall(rb"[ -~]{4,}", d)]
            print(f"{c['tag']}v{c['ver']} strings: {strs}")
    frames = [c for c in chunks if c["tag"] == "RDI"]
    if not frames:
        print("no RDI video frames found")
        return
    c = frames[min(a.frame, len(frames) - 1)]
    f.seek(c["offset"])
    d = f.read(c["size"])
    print(f"frame {a.frame}: offset {c['offset']} size {c['size']}")
    # payload start = first 256-byte window with entropy > 7.3 bits/byte
    ps = next((w for w in range(0, min(len(d), 1 << 18), 128) if entropy(d[w:w + 256]) > 7.0), None)
    print(f"  compressed payload starts near byte {ps}; payload entropy {entropy(d[ps:ps+65536]):.3f} bits/byte" if ps is not None else "  no high-entropy payload found")
    print(f"  payload head: {d[ps:ps+32].hex() if ps is not None else ''}")
    head = d[:4096]
    for name, m in JXS_MARKERS:
        i = head.find(m)
        print(f"  JPEG XS {name} {m.hex()} in first 4 KiB: {'at ' + str(i) if i >= 0 else 'absent'}")
    soc = d.find(b"\xff\x10\xff\x50")
    print(f"  JPEG XS SOC+CAP sequence anywhere in frame: {'at ' + str(soc) if soc >= 0 else 'absent -> not a JPEG XS codestream'}")
    if a.dump:
        open(a.dump, "wb").write(d)
        print(f"  wrote {a.dump}")
    if a.json:
        json.dump(chunks, open(a.json, "w"), indent=1)
        print(f"  wrote {a.json}")


if __name__ == "__main__":
    main()
