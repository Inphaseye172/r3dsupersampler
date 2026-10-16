Developer build from commit {{COMMIT}} (tag {{TAG}}). **This is not a production release.**

CI built it without the RED R3D SDK and without the production release gate. That gate requires
the SDK and the maintainer's confirmation of the third-party distribution terms
(`release/capabilities.json`).

## What works in this build

- **CinemaDNG RAW to smaller CinemaDNG RAW.** The sensor mosaic is resampled per CFA plane and
  never debayered. Output is lossless CinemaDNG; quality-driven lossy output is not implemented yet,
  so the quality setting is disabled for it.
- **Faithful metadata.** Colour, calibration and forward matrices, camera profiles, white balance
  (per frame), baseline exposure, camera and lens identity, orientation, frame rate and per-frame
  timecode are copied verbatim. The masked border is cropped, the default crop is scaled and the
  displayed aspect is kept. Bad-pixel lists are applied; gain maps are rescaled.
- **Audio.** Every WAV sidecar is copied byte for byte.
- **Foreign CinemaDNG.** Tiles or strips, either byte order, 8–16 bit, every CFA pattern,
  LinearizationTable, preview-plus-SubIFD layouts and AppleDouble (`._*`) files from macOS volumes.
- **Safe batches.** Outputs appear only when complete and verified; failed, cancelled or interrupted
  clips leave nothing behind; clips can no longer overwrite each other. Folder and archive mode,
  batch tabs, `r3dss-cli`, `r3dss-gui` and `r3dss-gui --run-job JOB.json`.
- **Clip recognition.** REDCODE (KOMODO), Nikon R3D NE and NEV clips are identified and labelled.

## What does not

- **No REDCODE or Nikon decoding** in this build (no RED SDK). A build made with your own copy of
  the SDK (`scripts/build.ps1` with `R3DSDK_ROOT` set) decodes them to developed RGB.
- **No RAW-to-RAW for REDCODE.** RED exposes neither the sensor mosaic nor an encoder in any
  available SDK (`docs/FEASIBILITY.md`).

## How it was checked

On Linux and on GitHub's `windows-latest` runner: unit tests, then the acceptance run from the
installed program (on Windows after a silent install of the attached installer). That converts a 6K24 clip to 2K24 and
a matrix of cases (every CFA pattern, 23.976 drop and non-drop timecode, 25p, 50p, uncompressed
source, area filter, a 1.5x factor, a width that is not a multiple of 16, same size, and a clip
written by PiDNG, an independent DNG writer). Each output is decoded with LibRaw and must equal the
documented resample of the source within 1 code value, with no colour or phase shift and every
source tag carried over (`acceptance-windows.json`, `acceptance-linux.json`). CI also checks that
every DLL the package needs is inside it, including the Visual C++ runtime, and runs the installed
GUI headless.

Not checked: DaVinci Resolve itself, the CUDA path on a GPU, and a physical Windows PC.

## Licenses

This project's code is Apache-2.0. The builds bundle Qt (LGPL-3.0) and the Microsoft Visual C++
runtime, and compile in nlohmann/json (MIT) and CLI11 (BSD-3-Clause). License texts are in the
`licenses` folder of the zip and of the installed app.
