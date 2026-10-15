Developer build from commit {{COMMIT}} (tag {{TAG}}). **This is not a production release.**

CI built it without the RED R3D SDK and without the production release gate. That gate requires
the SDK and the maintainer's confirmation of the third-party distribution terms
(`release/capabilities.json`).

## What works in this build

- **CinemaDNG RAW to smaller CinemaDNG RAW.** The sensor mosaic is resampled in the Bayer domain
  and never debayered. Frame rate, duration, timecode, black and white levels and audio carry
  over. Output is lossless-compressed CinemaDNG; quality-driven lossy VBR output is not
  implemented yet.
- **Batch work.** Batch tabs, folder mode and archive mode, from the GUI (`r3dss-gui`) or the
  command line (`r3dss-cli`).
- **Clip recognition.** REDCODE (KOMODO), Nikon R3D NE and NEV clips are recognised and labelled,
  for example "REDCODE RAW (KOMODO 6K)".

## What does not

- **No REDCODE or Nikon decoding.** This build has no RED SDK. A build made with your own copy of
  the SDK (`scripts/build.ps1` with `R3DSDK_ROOT` set) decodes them to developed RGB.
- **No RAW-to-RAW for REDCODE.** RED exposes neither the sensor mosaic nor an encoder in any
  available SDK, so REDCODE clips cannot be converted RAW to RAW (`docs/FEASIBILITY.md`).

## How it was checked

On GitHub's `windows-latest` runner: unit tests, a CLI end-to-end conversion, then a silent
install of the attached installer and the developer acceptance checks run from the installed
folder (`acceptance-windows.json`, attached).

Not checked: a clean Windows machine (the runner already has the Visual C++ runtime),
DaVinci Resolve, and the CUDA path.

## Licenses

This project's code is Apache-2.0. The builds bundle Qt (LGPL-3.0) and compile in nlohmann/json
(MIT) and CLI11 (BSD-3-Clause). Their license texts are in the `licenses` folder of the zip and
of the installed app.
