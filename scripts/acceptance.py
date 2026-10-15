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
import argparse, hashlib, json, os, pathlib, shutil, subprocess, sys, tempfile, wave

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

    def add(self, check, status, detail):
        self.items.append({"check": check, "status": status, "detail": detail})
        print(f"[{status.upper():7}] {check}: {detail}", flush=True)

    def skip_or_fail(self, check, detail):
        self.add(check, "skipped" if self.mode == "dev" else "failed", detail)

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


def wav_info(path):
    with open(path, "rb") as f:
        data = f.read()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def check_raw_to_raw(rep, cli, work, frames, width, height):
    name = "cinemadng-raw-to-raw 6K24 -> 2K24"
    src, out = work / "A001", work / "out"
    run([cli, "synth", str(src), "--width", str(width), "--height", str(height), "--frames", str(frames),
         "--fps", "24", "--audio-channels", "2", "--bits", "12", "--pattern", "scene"])
    srcp = run_json([cli, "probe", str(src)])
    run([cli, "convert", str(src), "-o", str(out), "--size", "2K", "--profile", "Q5", "--container", "cdng", "-q"])
    produced = [p for p in out.iterdir()]
    if len(produced) != 1 or not produced[0].is_dir():
        rep.add(name, "failed", f"expected one output clip directory in {out}, found {[p.name for p in produced]}")
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
            if "raw-to-raw-cdng" in wanted:
                try:
                    check_raw_to_raw(rep, cli, work, a.frames, a.width, a.height)
                except Exception as e:  # noqa: BLE001
                    rep.add("cinemadng-raw-to-raw 6K24 -> 2K24", "failed", str(e))
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
