#!/usr/bin/env python3
"""Acceptance checks for an r3dss build or installation.

Runs the binaries in --bin (normally the INSTALLED bin directory) against
release/capabilities.json and reports every advertised capability as passed, failed or skipped.

  --mode release : every advertised capability, sample and RAW-to-RAW check must pass; nothing may
                   be skipped. Used by the tagged-release CI job after a silent install.
  --mode dev     : capabilities whose third-party requirement is absent (e.g. the RED SDK) and
                   missing samples are reported as skipped instead of failing.

Standard library only. Exit code 0 = accepted, 1 = rejected, 2 = usage error.
"""
import argparse, hashlib, importlib.util, json, os, pathlib, shutil, subprocess, sys, tempfile, wave

ROOT = pathlib.Path(__file__).resolve().parent.parent


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class Report:
    def __init__(self, mode):
        self.mode, self.items = mode, []
        self.require_tools = False

    def add(self, check, status, detail):
        self.items.append({"check": check, "status": status, "detail": detail})
        print(f"[{status.upper():7}] {check}: {detail}", flush=True)

    def skip_or_fail(self, check, detail):
        self.add(check, "skipped" if self.mode == "dev" else "failed", detail)

    def tool_missing(self, check, detail):
        # The independent gate's Python tools: optional on a developer machine, required in CI / release.
        self.add(check, "failed" if self.require_tools or self.mode != "dev" else "skipped", detail)

    @property
    def ok(self):
        return all(i["status"] in ("passed", "skipped") for i in self.items) and (
            self.mode == "dev" or all(i["status"] == "passed" for i in self.items))


def exe(bin_dir, name):
    for cand in (bin_dir / (name + ".exe"), bin_dir / name):
        if cand.exists():
            return str(cand)
    raise FileNotFoundError(f"{name} not found in {bin_dir}")


def run(cmd, timeout=3600):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} exited {r.returncode}\nstdout:\n{r.stdout[-2000:]}\nstderr:\n{r.stderr[-2000:]}")
    return r.stdout


def run_json(cmd):
    out = run(cmd)
    start = out.find("{")
    return json.loads(out[start:])


def check_modules(rep, caps, codecs):
    mods = {m["format"]: m for m in codecs["modules"]}
    for cap in caps["capabilities"]:
        for need in cap.get("modules", []):
            m = mods.get(need["format"])
            name = f"{cap['id']} / module {need['format']}"
            if m is None:
                rep.add(name, "failed", "module not registered")
                continue
            problems = [k for k in ("read", "write") if k in need and bool(m.get(k)) != bool(need[k])]
            if m["state"] != "available":
                problems.append(f"state={m['state']}")
            if not problems:
                rep.add(name, "passed", f"{m['name']}: {m['status']}")
            elif cap.get("requires") and rep.mode == "dev":
                rep.add(name, "skipped", f"requirement {cap['requires']} absent in this dev build ({', '.join(problems)}): {m['status']}")
            else:
                rep.add(name, "failed", f"{', '.join(problems)}: {m['status']}")


def check_samples(rep, caps, cli, samples_dir):
    for s in caps.get("samples", []):
        name = f"sample {s['file']}"
        path = pathlib.Path(samples_dir) / s["file"] if samples_dir else None
        if not path or not path.exists():
            rep.skip_or_fail(name, f"not found (R3DSS_SAMPLES={samples_dir}); run scripts/fetch_samples.py")
            continue
        digest = sha256(path)
        if digest != s["sha256"]:
            rep.add(name, "failed", f"sha256 {digest} != {s['sha256']}")
            continue
        j = run_json([cli, "probe", "--chunks", str(path)])
        e = s["expect"]
        problems = []
        if j.get("source") != e["source"]:
            problems.append(f"source {j.get('source')!r} != {e['source']!r}")
        if len(j.get("chunks", [])) != e["chunks"]:
            problems.append(f"{len(j.get('chunks', []))} chunks != {e['chunks']}")
        hist = {k: v["count"] for k, v in j.get("chunkHistogram", {}).items()}
        if hist != e["chunkCounts"]:
            problems.append(f"chunk histogram {hist} != {e['chunkCounts']}")
        tcs = [j.get("vendor", {}).get(f"header.timecode{i}") for i in range(len(e["headerTimecodes"]))]
        if tcs != e["headerTimecodes"]:
            problems.append(f"header timecodes {tcs} != {e['headerTimecodes']}")
        rep.add(name, "failed" if problems else "passed", "; ".join(problems) or f"{j['source']}, {len(j['chunks'])} chunks, hash verified")


def independent_gate_available():
    return all(importlib.util.find_spec(m) is not None for m in ("rawpy", "numpy", "tifffile"))


def run_gate(rep, name, src, dst, kernel="lanczos3", gate=True):
    """Independent pixel + tag check (scripts/verify_cdng.py, LibRaw + tifffile)."""
    if not gate:
        rep.tool_missing(name + " / independent gate", "rawpy, numpy or tifffile not installed (pip install rawpy numpy tifffile)")
        return
    r = subprocess.run([sys.executable, str(ROOT / "scripts" / "verify_cdng.py"), str(src), str(dst), "--kernel", kernel],
                       capture_output=True, text=True, timeout=3600)
    last = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
    problems = [ln[len("PROBLEM: "):] for ln in r.stdout.splitlines() if ln.startswith("PROBLEM: ")]
    if r.returncode == 0:
        rep.add(name + " / independent gate", "passed", last)
    else:
        rep.add(name + " / independent gate", "failed", "; ".join(problems[:4]) or (r.stdout + r.stderr)[-600:])


def single_output(out):
    produced = sorted(out.iterdir()) if out.exists() else []
    partial = [p.name for p in produced if p.name.endswith(".partial")]
    dirs = [p for p in produced if p.is_dir() and not p.name.endswith(".partial")]
    return dirs, partial


# Small cases that each exercise one dimension; every output goes through the independent gate.
MATRIX = [
    # name, synth args, convert args, kernel
    ("CFA GRBG, 2x", ["--cfa", "GRBG"], ["--size", "960x540"], "lanczos3"),
    ("CFA GBRG, 2x", ["--cfa", "GBRG"], ["--size", "960x540"], "lanczos3"),
    ("CFA BGGR, 2x", ["--cfa", "BGGR"], ["--size", "960x540"], "lanczos3"),
    ("23.976 drop-frame timecode", ["--fps", "24000/1001", "--timecode", "01:00:59;20"], ["--size", "960x540"], "lanczos3"),
    ("23.976 non-drop timecode", ["--fps", "24000/1001", "--timecode", "10:00:00:12"], ["--size", "960x540"], "lanczos3"),
    ("25p", ["--fps", "25", "--timecode", "07:12:30:24"], ["--size", "960x540"], "lanczos3"),
    ("50p (frame-pair timecode)", ["--fps", "50", "--timecode", "07:12:30:24"], ["--size", "960x540"], "lanczos3"),
    ("uncompressed source", ["--uncompressed"], ["--size", "960x540"], "lanczos3"),
    ("area kernel", [], ["--size", "960x540", "--kernel", "area"], "area"),
    ("non-integer factor 1.5", [], ["--size", "1280x720"], "lanczos3"),
    ("output width % 16 != 0", [], ["--size", "1000x562"], "lanczos3"),
    ("same size (bit-exact)", [], ["--size", "same"], "lanczos3"),
]


def check_matrix(rep, cli, work, gate):
    for i, (name, synth_args, conv_args, kernel) in enumerate(MATRIX):
        label = "matrix: " + name
        src, out = work / f"M{i:02d}" / f"M{i:02d}", work / f"M{i:02d}" / "out"
        try:
            run([cli, "synth", str(src), "--width", "1920", "--height", "1080", "--frames", "4", "--bits", "12",
                 "--pattern", "scene", "--audio-channels", "2"] + synth_args)
            r = subprocess.run([cli, "convert", str(src), "-o", str(out), "-q"] + conv_args, capture_output=True, text=True, timeout=3600)
            dirs, partial = single_output(out)
            if r.returncode != 0 or len(dirs) != 1 or partial:
                rep.add(label, "failed", f"exit {r.returncode}, outputs {[d.name for d in dirs]}, partial {partial}: {(r.stdout + r.stderr)[-400:]}")
                continue
            rep.add(label, "passed", f"{dirs[0].name}")
            run_gate(rep, label, src, dirs[0], kernel, gate)
        except Exception as e:  # noqa: BLE001
            rep.add(label, "failed", str(e)[-600:])
    # A third-party source written by tifffile, an independent TIFF/DNG writer (big-endian, uncompressed).
    label = "matrix: third-party source (tifffile writer, big-endian)"
    if not gate:
        rep.tool_missing(label, "numpy / tifffile not installed (pip install numpy tifffile)")
    else:
        src, out = work / "FC" / "F001", work / "FC" / "out"
        try:
            run([sys.executable, str(ROOT / "scripts" / "make_foreign_clip.py"), str(src)])
            r = subprocess.run([cli, "convert", str(src), "-o", str(out), "-q", "--size", "320x180"], capture_output=True, text=True, timeout=3600)
            dirs, partial = single_output(out)
            if r.returncode != 0 or len(dirs) != 1 or partial:
                rep.add(label, "failed", f"exit {r.returncode}: {(r.stdout + r.stderr)[-400:]}")
            else:
                rep.add(label, "passed", dirs[0].name)
                run_gate(rep, label, src, dirs[0], "lanczos3", gate)
        except Exception as e:  # noqa: BLE001
            rep.add(label, "failed", str(e)[-600:])
    # Exit codes (I2): nothing written because the size is larger than the source -> 3.
    label = "matrix: upscale request writes nothing, exit code 3"
    src = work / "M00" / "M00"
    r = subprocess.run([cli, "convert", str(src), "-o", str(work / "UP"), "-q", "--size", "6K"], capture_output=True, text=True)
    dirs, partial = single_output(work / "UP")
    rep.add(label, "passed" if r.returncode == 3 and not dirs and not partial else "failed", f"exit {r.returncode}")


def wav_info(path):
    with open(path, "rb") as f:
        data = f.read()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def check_raw_to_raw(rep, cli, work, frames, width, height, gate):
    name = "cinemadng-raw-to-raw 6K24 -> 2K24"
    src, out = work / "A001", work / "out"
    run([cli, "synth", str(src), "--width", str(width), "--height", str(height), "--frames", str(frames),
         "--fps", "24", "--audio-channels", "2", "--bits", "12", "--pattern", "scene"])
    srcp = run_json([cli, "probe", str(src)])
    run([cli, "convert", str(src), "-o", str(out), "--size", "2K", "--profile", "Q5", "--container", "cdng", "-q"])
    produced, partial = single_output(out)
    if len(produced) != 1 or partial:
        rep.add(name, "failed", f"expected one output clip directory in {out}, found {[p.name for p in produced]}, partial {partial}")
        return
    dst = produced[0]
    outp = run_json([cli, "probe", str(dst)])
    problems = []
    expect_h = round(2048 * height / width / 2) * 2
    checks = [
        ("format", outp.get("format"), "CinemaDNG"),
        ("width", outp.get("width"), 2048),
        ("height", outp.get("height"), expect_h),
        ("cfa (still a sensor mosaic)", outp.get("cfa"), srcp.get("cfa")),
        ("bits", outp.get("bits"), srcp.get("bits")),
        ("black", outp.get("black"), srcp.get("black")),
        ("white", outp.get("white"), srcp.get("white")),
        ("frames", outp.get("frames"), srcp.get("frames")),
        ("fps", outp.get("fps"), srcp.get("fps")),
        ("duration_s", outp.get("duration_s"), srcp.get("duration_s")),
        ("timecode", outp.get("timecode"), srcp.get("timecode")),
        ("audio", outp.get("audio"), srcp.get("audio")),
        ("iso", outp.get("iso"), srcp.get("iso")),
        ("asShotNeutral", outp.get("asShotNeutral"), srcp.get("asShotNeutral")),
    ]
    for label, got, want in checks:
        if got != want:
            problems.append(f"{label}: {got!r} != {want!r}")
    src_wavs = sorted(src.glob("*.wav"))
    dst_wavs = sorted(dst.glob("*.wav"))
    if len(src_wavs) != 1 or len(dst_wavs) != 1:
        problems.append(f"audio sidecars: source {len(src_wavs)}, output {len(dst_wavs)}")
    else:
        a, b = wav_info(src_wavs[0]), wav_info(dst_wavs[0])
        if a["sha256"] != b["sha256"]:
            problems.append("audio sidecar is not bit-identical to the source")
    if outp.get("bytes", 0) >= srcp.get("bytes", 0):
        problems.append(f"output {outp.get('bytes')} bytes is not smaller than source {srcp.get('bytes')}")
    ratio = outp.get("bytes", 0) / max(1, srcp.get("bytes", 1))
    rep.add(name, "failed" if problems else "passed",
            "; ".join(problems) or f"{width}x{height} -> {outp['width']}x{outp['height']} mosaic ({outp['cfa']}, {outp['bits']}-bit), "
            f"{outp['frames']} frames @ {outp['fps']} fps, TC {outp.get('timecode')}, audio bit-identical, size ratio {ratio:.3f}")
    run_gate(rep, name, src, dst, "lanczos3", gate)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="directory with r3dss-cli (installed bin dir for release acceptance)")
    ap.add_argument("--mode", choices=["dev", "release"], default="dev")
    ap.add_argument("--capabilities", default=str(ROOT / "release" / "capabilities.json"))
    ap.add_argument("--samples", default=os.environ.get("R3DSS_SAMPLES", ""))
    ap.add_argument("--work", default="", help="scratch directory (default: a new temp dir, removed afterwards)")
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--width", type=int, default=6144)
    ap.add_argument("--height", type=int, default=3240)
    ap.add_argument("--report", default="", help="write the JSON report here")
    ap.add_argument("--require-independent", action="store_true",
                    help="fail (not skip) when rawpy/numpy/tifffile are missing; implied by --mode release")
    ap.add_argument("--no-matrix", action="store_true", help="only the 6K24 -> 2K24 case")
    a = ap.parse_args()
    caps = json.loads(pathlib.Path(a.capabilities).read_text())
    rep = Report(a.mode)
    bin_dir = pathlib.Path(a.bin)
    try:
        cli = exe(bin_dir, "r3dss-cli")
    except FileNotFoundError as e:
        rep.add("binaries", "failed", str(e))
        cli = None
    work = pathlib.Path(a.work) if a.work else pathlib.Path(tempfile.mkdtemp(prefix="r3dss-accept-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        if cli:
            codecs = run_json([cli, "codecs", "--json"])
            if a.mode == "release" and not codecs.get("release"):
                rep.add("release build", "failed", "binaries were not built with R3DSS_RELEASE=ON")
            check_modules(rep, caps, codecs)
            wanted = {c["acceptance"] for c in caps["capabilities"]}
            if "probe-samples" in wanted:
                check_samples(rep, caps, cli, a.samples)
            gate = independent_gate_available()
            rep.require_tools = a.require_independent
            if "raw-to-raw-cdng" in wanted:
                try:
                    check_raw_to_raw(rep, cli, work, a.frames, a.width, a.height, gate)
                except Exception as e:  # noqa: BLE001
                    rep.add("cinemadng-raw-to-raw 6K24 -> 2K24", "failed", str(e))
                if not a.no_matrix:
                    check_matrix(rep, cli, work, gate)
    finally:
        if not a.work:
            shutil.rmtree(work, ignore_errors=True)
    verdict = "ACCEPTED" if rep.ok else "REJECTED"
    print(f"{verdict} ({a.mode}): {sum(i['status'] == 'passed' for i in rep.items)} passed, "
          f"{sum(i['status'] == 'skipped' for i in rep.items)} skipped, {sum(i['status'] == 'failed' for i in rep.items)} failed")
    if a.report:
        pathlib.Path(a.report).write_text(json.dumps({"mode": a.mode, "verdict": verdict, "items": rep.items}, indent=2))
    return 0 if rep.ok else 1


if __name__ == "__main__":
    sys.exit(main())
