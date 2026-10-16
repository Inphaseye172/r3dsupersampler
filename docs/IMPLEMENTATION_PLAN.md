# Implementation plan (revised 2026-10-15, status 2026-10-16)

Replaces the 2026-10-15 "RED SDK → RGB → re-mosaic / ProRes / DNxHR" plan, which wrongly treated the
no-debayer requirement as withdrawn. That plan's milestones 3–5 (RGB resample, re-mosaiced DNG,
ProRes / DNxHR, recording-rate option) are cancelled. Evidence for every claim below is in
`docs/FEASIBILITY.md` §0 and `docs/CODESTREAM.md`.

## Requirement

Genuine RAW-to-RAW: a camera raw clip (sensor mosaic, compressed) in, a smaller raw clip at a lower
resolution out, with quality-driven variable bitrate, keeping frame rate, duration, audio, timecode
and the RAW grading workflow in DaVinci Resolve (for example 6K24 → 2K24). The mosaic is resampled in
the Bayer domain and never debayered.

## Status

| Milestone | Content | State |
|---|---|---|
| M1 | RED container walker, source detection by content, SDK module skeleton, samples, build hygiene | done (merged) |
| M2 | RED R3D SDK decoder: identify, verify and decode REDCODE / R3D NE / NEV to developed RGB (not a RAW-to-RAW path) | merged; compiles against SDK 9.0 headers; **Windows test with SDK 9.2.1 pending** (CI has no SDK: `R3DSDK_ZIP_URL` returns 404, D6) |
| M-0 | Mandatory fixes: output safety (complete-or-nothing outputs, collisions, overwrite of sources), archive integrity, cancellation, UTF-8 paths, neutral branding, developer vs release builds | done |
| Review 2026-10-15 | Independent review of the RAW-to-RAW path with LibRaw; ~60 findings, all fixed: LibRaw-safe tile layout, ActiveArea, LinearizationTable, black-level variants, strict LJ92 decoding, foreign DNG layouts, metadata and audio carried verbatim, crop/aspect/opcode rescaling, clip-aware overshoot rule, CI pixel and tag gate, app-local MSVC runtime, headless GUI runs | done |
| M-A | Lossy, quality-driven CinemaDNG (DNG 1.4 compression 34892 on the mosaic, per-frame quality search against the existing VBR budget, Q1–Q7); acceptance on a real CinemaDNG camera clip; maintainer check in Resolve | next |
| M-B | Nikon evidence: survey an R3D NE and an NEV clip, decode and re-encode one frame with the vendor's mosaic codec, map the container frame layout | needs D2, D3 |
| M-C | Nikon RAW-to-RAW, only if M-B passes | blocked on M-B |
| M-D | Production release: `R3DSS_RELEASE=ON` with installed-Windows acceptance, bundling only runtimes whose redistribution is confirmed | needs D6, D8 |

Not possible: RAW-to-RAW for REDCODE (KOMODO). RED exposes neither the mosaic nor an encoder (D1).

## Unresolved dependencies (owner: maintainer)

| # | Dependency | Needed for |
|---|---|---|
| D1 | Written answer from RED on any licensed mosaic output or REDCODE encoder | KOMODO RAW-to-RAW |
| D2 | One Nikon R3D NE and one NEV clip (1–2 s) in `samples-v2` | M-B |
| D3 | A JPEG XS-Bayer (TicoRAW) evaluation SDK with license and API docs | M-B, M-C |
| D4 | RED / Nikon position on third-party-written R3D NE / NEV files | M-C |
| D5 | A Windows machine with Resolve for grading checks | M-A and later |
| D6 | A working `R3DSDK_ZIP_URL` / `R3DSDK_TOKEN` secret | SDK tests in CI, production releases |
| D7 | Product name (Supersampler.app is a competitor; interim binaries `r3dss-gui` / `r3dss-cli`) | release |
| D8 | Confirmation of the third-party distribution terms (RED runtime, Qt LGPL, others), then the repository variable `R3DSS_THIRD_PARTY_TERMS_ACKNOWLEDGED=ON` | production releases |

## Developer builds vs releases

- Default (`R3DSS_RELEASE=OFF`): every optional module reports its status; `dev-*` tags or a manual CI
  run with `prerelease_tag` publish a labelled developer pre-release after Linux and Windows CI pass.
- `R3DSS_RELEASE=ON` (`v*` tags): configure fails unless every capability in `release/capabilities.json`
  is present (RED SDK, verified samples, GUI, terms acknowledgement); CI installs the package
  silently and runs the full acceptance, including the independent LibRaw gate, from the installed
  location. Any failure stops the release.

## M-A in detail (next)

1. Lossy mosaic encoder: DNG 1.4 lossy JPEG (34892), one grey component per tile, 8-bit codes
   through a LinearizationTable so the data stays undeveloped; per-frame quality search against the
   VBR controller's budget for Q1/Q3/Q5/Q7; lossless LJ92 stays available.
2. Acceptance: 6K24 CinemaDNG to 2K24 at Q5 is at most 40 % of the source size; developed PSNR against
   the same-filter reference ≥ 45 dB at Q1; frame rate, duration, timecode and bit-identical audio as
   today; the independent gate extended with a lossy tolerance.
3. Maintainer check in Resolve on a real camera CinemaDNG (Blackmagic, Sigma fp, Z CAM): Camera RAW
   controls, ISO / white balance / highlight recovery behave as on the source, no colour fringes.
