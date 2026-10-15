# R3D Supersampler

Windows desktop application (and CLI) that shrinks Nikon raw video by resampling the Bayer
mosaic **without debayering** and re-encoding at a **content-driven variable bitrate**. Targets
Nikon ZR (R3D NE, NEV / N-RAW) and Z9 / Z8 / Z6 III (NEV) sources; outputs 6K, UHD, Full HD or
same-resolution raw.

## Status of this build (v0.1)

| Part | State |
|---|---|
| Bayer-domain resampler (Lanczos-3 / Area, phase-correct, clip-aware), CPU multi-threaded | working, tested |
| CUDA resampler, parity-tested against CPU | built when a CUDA toolkit is present; needs a GPU to run |
| Content-driven VBR controller, Q1 / Q3 / Q5 / Q7 profiles | working, tested |
| Batch pipeline: reader → bounded queues → resample workers → ordered writer, audio + metadata passthrough | working, tested |
| Folder mode (recursive, preserved structure), archive mode (copy + SHA-256 verify + manifest), output verification | working, tested |
| Qt GUI with batch tabs, queue, per-file progress, settings, codec-module status, log pane | working |
| `r3dss-cli` (`codecs`, `probe`, `convert`, `scan`, `synth`) and `r3dss-bench` | working |
| **CinemaDNG** reader/writer (reference codec, opens in DaVinci Resolve) | working, lossless only |
| **NEV / N-RAW** and **R3D NE** decode / encode | **not available**: needs the intoPIX TicoRAW SDK (commercial) and a container spec. Files are recognised and the UI says why. See `docs/FEASIBILITY.md`. |
| R3D NE *output* | not possible with any public SDK; listed as unavailable |

Read `docs/FEASIBILITY.md` first: it is the honest account of what can and cannot be built
around these formats and what you need to obtain.

## Build

**Windows (one command, clean clone).** Needs Visual Studio 2022 with the Desktop C++ workload,
CMake ≥ 3.25, Ninja, Python 3 and git on PATH. CUDA Toolkit 12.x is optional.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1            # bootstrap deps, build, test, dist\ + zip
powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Installer # also builds the Inno Setup installer
```

`scripts\bootstrap.ps1` installs vcpkg and Qt 6.8 into `.deps\` (set `VCPKG_ROOT` / `QT_ROOT`
to reuse existing installs). Then `cmake --preset windows-release && cmake --build --preset
windows-release && ctest --preset windows-release`.

**Linux (engine, CLI, tests, GUI).**

```bash
scripts/bootstrap.sh          # apt packages on Ubuntu 24.04
cmake --preset linux-ci && cmake --build --preset linux-ci && ctest --preset linux-ci
```

CMake options: `R3DSS_BUILD_GUI` (AUTO), `R3DSS_WITH_CUDA` (AUTO), `R3DSS_WITH_TICORAW` (OFF,
needs `TICORAW_ROOT`), `R3DSS_WITH_R3DSDK` (OFF, needs `R3DSDK_ROOT`), `R3DSS_BUILD_TESTS`,
`R3DSS_BUILD_BENCH`.

## Use

GUI: `R3DSupersampler.exe`. Add files or a folder, pick size / container / quality, Start.
Each batch is a tab; starting another batch while one runs queues it.

CLI:

```text
r3dss-cli codecs                                   # module status, hardware
r3dss-cli synth D:\test\A001 --width 6048 --height 3402 --frames 48   # synthetic CinemaDNG clip
r3dss-cli probe D:\test\A001
r3dss-cli convert D:\test\A001 -o D:\out --size UHD --profile Q5
r3dss-cli scan D:\cards\day1 -o D:\archive --archive --run           # folder + archive mode
r3dss-bench --width 8256 --height 4644 --frames 10 --size UHD --json bench.json
```

## Documents

`docs/FEASIBILITY.md`, `docs/ARCHITECTURE.md`, `docs/CODEC_INTERFACE.md`, `docs/RESAMPLING.md`,
`docs/UI_SPEC.md`, `docs/TEST_PLAN.md`, `docs/IMPLEMENTATION_PLAN.md`, `docs/RESOLVE_NOTE.md`.

## DaVinci Resolve note

Half-resolution R3D NE decode can ring near clipped highlights. Use *Full Res Premium* (project
or clip Camera RAW settings). Details in `docs/RESOLVE_NOTE.md`.

## License

See `LICENSE` (not yet chosen) and the third-party table in `docs/FEASIBILITY.md`.
