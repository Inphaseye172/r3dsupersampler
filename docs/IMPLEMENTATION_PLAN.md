# Implementation plan

Ordered tasks grouped by milestone. Each task lists files, responsibilities, acceptance criteria
and dependencies. **Done** marks what the repository already contains (v0.1); the rest is
specified so a smaller model can implement it without design decisions.

## M0 — skeleton, build, CI, stub codec layer, CPU pipeline end to end

1. **Repo skeleton** — Done. `CMakeLists.txt`, `CMakePresets.json` (linux-ci, linux-debug,
   windows-release, windows-debug), `vcpkg.json`, `cmake/*.cmake`, `scripts/bootstrap.ps1`,
   `scripts/build.ps1`, `scripts/bootstrap.sh`, `.clang-format`, `.gitignore`.
   Accept: `cmake --preset linux-ci && cmake --build --preset linux-ci` from a clean clone.
2. **Core** — Done. `src/core`: Expected/Error, types, timecode, concurrency, logger, SHA-256,
   hardware plan. Accept: `test_core.cpp` green.
3. **Codec interface + registry** — Done. `src/codec`. Accept: memory codec round trip in
   `test_pipeline.cpp`.
4. **NEV / R3D NE stubs** — Done. `src/codec_nev`. Accept: stub test in `test_pipeline.cpp`.
5. **Synthetic fixtures** — Done. `src/synth`.
6. **Pipeline v1** — Done. `src/pipeline` (job, converter, scheduler). Accept: memory clip test.
7. **CLI** — Done. `src/cli/main.cpp`: `codecs`, `probe`, `convert`, `scan`, `synth`.
8. **CinemaDNG codec** — Done. `src/codec_cdng` (tiff, ljpeg, wav, cdng_codec). Accept:
   `test_cdng.cpp`, `test_ljpeg_tiff_wav.cpp`.
9. **CI** — Done. `.github/workflows/ci.yml`: Linux build + tests + CLI e2e + GUI smoke +
   bench; Windows build + tests + zip + Inno Setup installer artifacts; optional CUDA input.

## M1 — real decode of NEV / R3D NE (needs samples and the intoPIX SDK)

10. **Container parser** — `src/codec_nev/src/red_container.cpp` (+ new `red_header.hpp`).
    Replace the opaque chunk walker with: header chunk fields (version, timescale, width,
    height, frame rate, audio channels, codec FourCC `iNRAW` / `iNR3D`), video frame chunks
    (frame number, metadata blob, codestream range), audio chunks (PCM layout incl. float32),
    end-of-file index chunks (frame offset table). Learn the layout from Nikon's sample files;
    use FFmpeg's `r3d.c` only as a study reference for the RED1-era layout. Prerequisite: legal
    review (`docs/FEASIBILITY.md` §3). Accept: `probe` prints frame count, size, fps, audio,
    timecode for every sample; a unit test parses a synthetic container written by task 22's
    muxer.
11. **TicoRAW adapter** — `src/codec_nev/src/ticoraw_adapter.cpp` behind `R3DSS_WITH_TICORAW`,
    `cmake/FindTicoRAW.cmake` fixed to the real SDK layout. Implement `ITicoRawCodec::decode`
    (CPU and CUDA) into `BayerFrame` (12-bit, CFA from the header). Accept: decodes every sample
    frame; sample-level parity between CPU and CUDA decode; `caps().read == true`.
12. **Metadata mapping** — `src/codec_nev/src/nev_metadata.cpp`: ISO, white balance, colour
    space name, lens model and correction blob (opaque), per-frame timecode, camera serial /
    firmware; unknown blobs into `ClipMetadata::opaque`. Accept: `test_nev_samples.cpp`
    (gated on `R3DSS_SAMPLES`) checks each field against values read in Resolve.
13. **RED SDK QC module (optional)** — `src/codec_r3dsdk` behind `R3DSS_WITH_R3DSDK`,
    `cmake/FindR3DSDK.cmake`: open clip, read width/height/frame count/timecode/metadata, decode
    a quarter-res RGB thumbnail for the verifier. Never on the mosaic path. Accept: verifier
    uses it to confirm NEV outputs open in RED's own decoder.

## M2 — CPU resampler

14. **Tap tables** — Done. `src/resample/src/taps.cpp`.
15. **Separable per-plane resampler** — Done. `cpu_resampler.cpp`.
16. **Size planner** — Done. `size_planner.cpp`.

## M3 — CUDA resampler

17. **CMake CUDA detection, device query** — Done (`R3DSS_WITH_CUDA` AUTO/ON/OFF).
18. **Kernels + parity test** — Done (`cuda_resampler.cu`, `test_cuda.cpp`). Parity must be
    run on a machine with a GPU; CI runners have none.
19. **Performance** — pinned host buffers (`cudaHostAlloc`) and double-buffered streams so
    H2D/compute/D2H overlap; shared-memory tiling for the horizontal pass. Accept: bench shows
    ≥ 2× the current CUDA fps on the same GPU; parity test still ≤ 1 LSB.

## M4 — encoder, VBR, metadata/audio passthrough

20. **Rate control** — Done. `src/ratecontrol`.
21. **Passthrough** — Done for CinemaDNG (metadata and audio proven by round-trip tests).
22. **TicoRAW encoder + NEV muxer** — `ticoraw_adapter.cpp::encode` (capped-rate mode with the
    per-frame budget from `FrameBudget`, lossless when requested) and
    `src/codec_nev/src/nev_writer.cpp` writing the container learned in task 10 (header, frame
    chunks with the original per-frame metadata blobs, PCM audio, index). Calibration: a table
    bpp ↔ encoder parameter measured on the samples, stored in `ratecontrol/profiles.cpp`.
    Accept: "keep container" NEV→NEV and CinemaDNG→NEV outputs open in Resolve and RED's
    decoder; average bpp within 10 % of the budget per profile.
23. **R3D NE output** — stays unsupported; the UI/CLI message is final. No task.

## M5 — UI, batch/tabs, folder + archive mode, verification

24. **Scanner, archive copier, verifier** — Done. `src/pipeline`.
25. **Qt app shell** — Done. `src/gui`.
26. **Batch queue** — Done (one running, others queued in order).
27. **GUI polish** — drag-and-drop of files/folders onto the table (`dragEnterEvent`/
    `dropEvent` in `BatchTab`), a naming-template editor with live preview in Settings,
    high-DPI application icon (`installer/r3dss.ico`, `.rc` file), a "Open output folder"
    context-menu action. Accept: manual checklist in `docs/UI_SPEC.md`.

## M6 — benchmark, docs, installer

28. **Benchmark harness** — Done (`bench/bench.cpp`).
29. **Installer / packaging** — Done (`installer/r3dss.iss`, CPack zip, CI artifacts, release
    assets on `v*` tags).
30. **Docs** — Done (this directory). Keep `docs/FEASIBILITY.md` updated as the unconfirmed
    items are resolved.

## Open questions and recommended defaults

1. Reference codec for the deployable build: **CinemaDNG** (chosen, implemented).
2. GUI licensing: **Qt 6 LGPL, dynamically linked** (windeployqt copies the DLLs).
3. R3D NE output: **unsupported, say so** (alternative: a clearly labelled "renamed N-RAW").
4. Product name: **trademark check before release** ("R3D" is RED's mark).
5. SDK access: **you obtain intoPIX and RED SDKs; the build detects `TICORAW_ROOT` /
   `R3DSDK_ROOT`**.
6. Parallel-clip default: **clamp(cores/8, 1, 4)**, RAM-capped, overridable.
7. Output naming: **`{stem}_{size}_{profile}`** under the chosen output root, mirroring the
   source folder structure.
