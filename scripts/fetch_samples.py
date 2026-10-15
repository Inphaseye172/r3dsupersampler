#!/usr/bin/env python3
"""Fetch the test sample clips (GitHub release samples-v1) into $R3DSS_SAMPLES.

Standard library only. Idempotent: when the expected clip already exists with the right SHA-256
nothing is downloaded. The zip is cached next to the samples directory.

Usage: fetch_samples.py [--dest DIR] [--cache DIR] [--check]
Exit codes: 0 ok, 1 download/unzip failure, 2 hash mismatch.
"""
import argparse, hashlib, os, pathlib, shutil, sys, urllib.request, zipfile

RELEASE_URL = "https://github.com/Inphaseye172/r3dsupersampler/releases/download/samples-v1/A060_A027_0912VZ_samples-v1.zip"
CLIPS = {  # file name inside the zip -> SHA-256
    "A060_A027_0912VZ_001.R3D": "b03dfab13093fd9bb1878a3af8bddbb81672fbc8256823b7141df42d41ae9a6e",
}


def sha256(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def verify(dest: pathlib.Path) -> bool:
    ok = True
    for name, digest in CLIPS.items():
        p = dest / name
        if not p.exists():
            print(f"missing: {p}")
            ok = False
            continue
        actual = sha256(p)
        if actual != digest:
            print(f"hash mismatch: {p}\n  expected {digest}\n  actual   {actual}")
            ok = False
        else:
            print(f"ok: {p} ({p.stat().st_size} bytes, sha256 verified)")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    repo = pathlib.Path(__file__).resolve().parent.parent
    ap.add_argument("--dest", default=os.environ.get("R3DSS_SAMPLES") or str(repo / ".deps" / "samples"))
    ap.add_argument("--cache", default=None, help="zip cache directory (default: <dest>/../samples-cache)")
    ap.add_argument("--check", action="store_true", help="only verify, never download")
    a = ap.parse_args()
    dest = pathlib.Path(a.dest)
    cache = pathlib.Path(a.cache) if a.cache else dest.parent / "samples-cache"
    if all((dest / n).exists() and sha256(dest / n) == d for n, d in CLIPS.items()):
        print(f"samples present in {dest}")
        return 0
    if a.check:
        return 2 if not verify(dest) else 0
    dest.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    zip_path = cache / "samples-v1.zip"
    if not zip_path.exists():
        print(f"downloading {RELEASE_URL}")
        tmp = zip_path.with_suffix(".part")
        try:
            with urllib.request.urlopen(RELEASE_URL, timeout=120) as r, open(tmp, "wb") as out:
                shutil.copyfileobj(r, out, 1 << 20)
        except Exception as e:  # noqa: BLE001
            print(f"download failed: {e}")
            return 1
        tmp.replace(zip_path)
    try:
        with zipfile.ZipFile(zip_path) as z:
            for member in z.namelist():
                base = pathlib.PurePosixPath(member).name
                if not base or member.endswith("/"):
                    continue
                with z.open(member) as src, open(dest / base, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
    except zipfile.BadZipFile as e:
        print(f"bad zip {zip_path}: {e}")
        zip_path.unlink(missing_ok=True)
        return 1
    if not verify(dest):
        return 2
    print(f"R3DSS_SAMPLES={dest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
