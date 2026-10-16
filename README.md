# r3dss

Working title; the product name has not been chosen (`docs/IMPLEMENTATION_PLAN.md`, D7).

A Windows desktop application and command line for **RAW-to-RAW supersampling**: a camera raw
clip goes in, a smaller raw clip at a lower resolution comes out, and it still grades in DaVinci
Resolve's Camera RAW workflow. The sensor mosaic is resampled per CFA plane in the Bayer domain and
is **never debayered**. Frame rate, duration, timecode, black and white levels, colour metadata and
audio are carried over.

CLI: `r3dss-cli`. GUI: `r3dss-gui`. License: Apache-2.0 for this project's code only (`LICENSE`,
`NOTICE`, `PATENTS.md`, third-party texts in `third_party/licenses`).

## What works, and what cannot

| Source | RAW-to-RAW | Notes |
|---|---|---|
| **CinemaDNG** (lossless JPEG or uncompressed, 8–16 bit, any CFA, tiles or strips, IFD0 or SubIFD layout) | **yes** | Output is lossless CinemaDNG. Verified pixel for pixel by an independent decoder (LibRaw) in CI |
| **REDCODE RAW** (RED KOMODO and other RED cameras) | **no** | RED exposes neither the sensor mosaic nor an encoder in any available SDK (`docs/FEASIBILITY.md`). With the RED R3D SDK installed the clips are identified and decoded to developed RGB for checking; without it they are identified only |
| **Nikon R3D NE / NEV** | not yet | Possible only with a third-party mosaic codec; unproven (`docs/FEASIBILITY.md`, milestone M-B) |

CinemaDNG output is always lossless. Quality profiles (Q1–Q7) have no effect on it; a lossy,
quality-driven CinemaDNG encoder is planned (milestone M-A).

## What a conversion keeps

- **Pictures.** The mosaic is resampled per CFA plane with phase-correct Lanczos-3 (default) or Area
  taps. Next to clipped highlights the resampler switches to Area so it never invents clipping.
  Very fine detail is softer than a full debayer would give (`docs/RESAMPLING.md` §8).
- **Levels and tone.** Black and white levels per CFA site; a source LinearizationTable is kept and
  the output is re-encoded to the same table, so bit depth and tone design are unchanged.
- **Framing.** The masked border (ActiveArea) is cropped away; DefaultCrop is scaled to the output;
  DefaultScale keeps the displayed aspect; Orientation is kept.
- **Colour and camera tags**, copied verbatim per frame: colour, calibration and forward matrices,
  camera profiles, white balance, baseline exposure, make, model, serial, lens, EXIF exposure facts.
- **Time.** FrameRate and per-frame TimeCodes are copied byte for byte; nothing is invented when a
  source lacks them.
- **Audio.** Every WAV sidecar is copied byte for byte (any format, BWF chunks, extra recorder tracks).
- **Opcodes.** Bad-pixel lists are applied before resampling; gain maps and other area opcodes are
  rescaled; normalised ones (lens corrections) are copied.

## Safety

- An output appears under its final name only when it is complete and verified (written into
  `<name>.partial` first). A failed, cancelled or interrupted clip leaves nothing behind;
  `--overwrite` keeps the previous output until the new one is complete.
- Two clips that would write the same output fail instead of overwriting each other. Output names
  carry the size (`A001_2K_lossless`, `A001_960x540_lossless`).
- Verification reads every written file back and compares it with what was written, and decodes
  every frame.
- Archive mode copies non-raw files (including sidecars inside clip folders) and verifies each by
  SHA-256 against an independent read of the source.

## Build

**Windows.** `scripts\bootstrap.ps1` installs missing prerequisites with winget (Git, CMake, Ninja,
Python 3.12, VS 2022 Build Tools with the C++ workload), then vcpkg and Qt 6.8 into `.deps\`.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1               # bootstrap, build, test, dist\ + zip
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Installer    # also the Inno Setup installer
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -FetchSamples # also download the sample clips
```

The package is self-contained: Qt and the MSVC runtime are installed next to the executables.

The RED R3D SDK is proprietary and not in this repository. To identify and decode REDCODE clips,
download it from RED, accept their license, unpack it and set `R3DSDK_ROOT` before configuring.
Builds without it still identify RED clips.

**Linux (engine, CLI, tests, GUI).**

```bash
scripts/bootstrap.sh
cmake --preset linux-ci && cmake --build --preset linux-ci && ctest --preset linux-ci
```

CMake options: `R3DSS_WITH_R3DSDK`, `R3DSS_WITH_CUDA`, `R3DSS_BUILD_GUI` (all AUTO),
`R3DSS_BUILD_TESTS`, `R3DSS_BUILD_BENCH`, `R3DSS_RELEASE` (production gate, see below).

## Tests and acceptance

```bash
python3 scripts/fetch_samples.py                 # samples-v1 into .deps/samples, SHA-256 verified
export R3DSS_SAMPLES=$PWD/.deps/samples
ctest --preset linux-ci
pip install rawpy numpy tifffile                 # independent decoder (tifffile also writes a third-party source)
python3 scripts/acceptance.py --bin <installed bin dir> --mode dev --require-independent
```

`scripts/acceptance.py` converts a 6K24 clip to 2K24 and a matrix of smaller cases (every CFA
pattern, 23.976 drop and non-drop timecode, 25p, 50p, uncompressed source, Area kernel, a 1.5×
factor, a width that is not a multiple of 16, same size, and a big-endian clip written by tifffile) and checks each
output with `scripts/verify_cdng.py`. That script decodes source and output with LibRaw and checks
that the output mosaic is exactly the documented resample of the source (within 1 code value), that
the developed picture has no colour or phase shift, and that every source tag reached the output.
CI runs it on Linux and on the **installed** Windows package.

`R3DSS_RELEASE=ON` (used for `v*` tags) refuses to configure unless every advertised capability
(`release/capabilities.json`) is present, including the RED SDK, the verified samples and the
maintainer's acknowledgement of the third-party distribution terms. `dev-*` tags and manual runs
publish clearly labelled developer pre-releases.

## Use

GUI: add files or a folder, pick the size and output folder, Start. Each batch is a tab; starting
another batch while one runs queues it. `r3dss-gui --run-job JOB.json` runs a saved job without a
window.

```text
r3dss-cli codecs                                    # module status, SDK state, hardware
r3dss-cli probe C:\cards\A001                       # clip facts as JSON
r3dss-cli convert C:\cards\A001 -o D:\out --size 2K # 6K CinemaDNG -> 2K CinemaDNG
r3dss-cli convert C:\cards\A001 -o D:\out --size 1280x720 --kernel area
r3dss-cli scan D:\cards\day1 -o D:\archive --archive --run   # folder + archive mode
r3dss-cli synth D:\test\A001 --width 6144 --height 3240 --frames 24 --cfa RGGB --timecode 01:00:00:00
```

Exit codes: 0 every clip done (or already complete), 1 a clip failed or the batch was cancelled,
3 nothing was written (for example the size is larger than the source). Ctrl-C cancels cleanly.

## Documents

`docs/IMPLEMENTATION_PLAN.md` (plan and status), `docs/FEASIBILITY.md` (what is possible per camera,
with evidence), `docs/CODESTREAM.md` (what was measured in real files), `docs/ARCHITECTURE.md`,
`docs/CODEC_INTERFACE.md`, `docs/RESAMPLING.md`, `docs/UI_SPEC.md`, `docs/TEST_PLAN.md`,
`docs/RESOLVE_NOTE.md`.

Not verified yet: playback and grading of the outputs in DaVinci Resolve itself, the CUDA resampler
on a GPU, and a Windows PC without development tools. CI proves the Windows build on GitHub's
`windows-latest` runner.
