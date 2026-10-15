# Supersampler

Windows desktop application and command line that shrink cinema raw clips: **RED KOMODO REDCODE
RAW (.R3D)**, Nikon **R3D NE** and **NEV** are decoded through the RED R3D SDK to 16-bit RGB,
resampled to 6K / UHD / Full HD with a phase-correct Lanczos-3 or Area filter, and written as
re-mosaiced lossy **CinemaDNG** (Camera Raw workflow in DaVinci Resolve) or as **ProRes 4444 / XQ**
and **DNxHR 444 / HQX**. CinemaDNG sources are resampled directly in the Bayer domain.

CLI name: `r3dss-cli`. License: Apache-2.0 (`LICENSE`, `NOTICE`, `PATENTS.md`).

## Status (milestone 1 of 6, see `docs/IMPLEMENTATION_PLAN.md`)

| Part | State |
|---|---|
| RED container walker (3-char tags + version, 4 KiB alignment), `r3dss-cli probe --chunks` | done; verified on the `samples-v1` KOMODO clip (98 chunks) |
| Source detection from content (`REDCODE RAW (KOMODO 6K)`, `R3D NE (NIKON …)`), GUI Format column | done |
| RED R3D SDK module skeleton: builds with or without the SDK, runtime discovery, honest status | done; decoder in milestone 2 |
| Bayer-domain resampler (CPU, optional CUDA), VBR controller, batch pipeline, folder + archive mode, verification | done (from v0.1) |
| CinemaDNG lossless reader/writer | done; lossy DNG in milestone 3 |
| RGB resample + re-mosaic, lossy DNG | milestone 3 |
| ProRes 4444 / XQ, DNxHR 444 / HQX | milestone 4 |
| Recording-rate vs timebase option | milestone 5 |
| Installer bundling the RED SDK runtime and VC++ redistributable, tagged releases | milestone 6 |
| REDCODE / R3D NE / NEV **output** | never: no public encoder exists |

Everything Windows- or GPU-related is proven only by the `windows-latest` GitHub Actions job and
by the maintainer; this repository's development happens on Linux.

## Build

**Windows (one command from a clean clone).** `scripts\bootstrap.ps1` installs missing
prerequisites with winget (Git, CMake, Ninja, Python 3.12, VS 2022 Build Tools with the C++
workload), then vcpkg and Qt 6.8 into `.deps\`.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1               # bootstrap, build, test, dist\ + zip
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Installer    # also the Inno Setup installer
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -FetchSamples # also download the sample clips
```

The RED R3D SDK is proprietary and not in this repository: download it from RED, accept their
license, unpack it and set `R3DSDK_ROOT` before configuring. The build detects it (`R3DSS_WITH_R3DSDK`
AUTO), links `Lib\win64\R3DSDK-2017MD.lib` (`/MD`) and installs `Redistributable\win` next to the
executables as `bin\redsdk\`. Without it REDCODE clips are recognised but cannot be decoded.

**Linux (engine, CLI, tests, GUI).**

```bash
scripts/bootstrap.sh
cmake --preset linux-ci && cmake --build --preset linux-ci && ctest --preset linux-ci
```

Linux links `Lib/linux64/libR3DSDKPIC-cpp11.a` with `-ldl -lpthread` and loads
`Redistributable/linux/REDR3D-x64.so` + `REDDecoder-x64.so` from `bin/redsdk/` or `$R3DSDK_LIBDIR`.

CMake options: `R3DSS_WITH_R3DSDK` (AUTO), `R3DSS_WITH_CUDA` (AUTO), `R3DSS_BUILD_GUI` (AUTO),
`R3DSS_BUILD_TESTS`, `R3DSS_BUILD_BENCH`.

## Sample clips and tests

```bash
python3 scripts/fetch_samples.py            # downloads release samples-v1 into .deps/samples, verifies SHA-256
export R3DSS_SAMPLES=$PWD/.deps/samples     # tests tagged [samples] run; otherwise they skip
ctest --preset linux-ci
```

No camera footage is committed. CI caches the samples and fetches the RED SDK from the private
secrets `R3DSDK_ZIP_URL` / `R3DSDK_TOKEN`; without them the SDK module and its tests are skipped.

## Use

GUI: `Supersampler.exe`. Add files or a folder, pick size / container / quality, Start. Each
batch is a tab; starting another batch while one runs queues it.

```text
r3dss-cli codecs                                   # module status, SDK state, hardware
r3dss-cli probe --chunks A060_A027_0912VZ_001.R3D  # source label, metadata, container chunks
r3dss-cli synth D:\test\A001 --width 6048 --height 3402 --frames 48   # synthetic CinemaDNG clip
r3dss-cli convert D:\test\A001 -o D:\out --size UHD --profile Q5 --container cdng
r3dss-cli scan D:\cards\day1 -o D:\archive --archive --run           # folder + archive mode
r3dss-bench --width 8256 --height 4644 --frames 10 --size UHD --json bench.json
```

## Documents

`docs/IMPLEMENTATION_PLAN.md` (current plan), `docs/CODESTREAM.md` (what was measured in real files),
`docs/FEASIBILITY.md`, `docs/ARCHITECTURE.md`, `docs/CODEC_INTERFACE.md`, `docs/RESAMPLING.md`,
`docs/UI_SPEC.md`, `docs/TEST_PLAN.md`, `docs/RESOLVE_NOTE.md`.

## DaVinci Resolve note

Half-resolution R3D decode can ring near clipped highlights. Use *Full Res Premium* (project or
clip Camera RAW settings). Details in `docs/RESOLVE_NOTE.md`.
