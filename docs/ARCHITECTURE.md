# Architecture

Updated 2026-10-16. The product is a RAW-to-RAW supersampler: CinemaDNG mosaic in, smaller
CinemaDNG mosaic out, resampled per CFA plane and never debayered (`docs/IMPLEMENTATION_PLAN.md`).
The RED SDK module identifies and decodes REDCODE to developed RGB for checking only. This file
describes the modules as they exist now.

## 1. Modules

```
src/core          r3dss_core        Expected/Error, BayerFrame, CFA, Rational, Timecode, AudioBlock,
                                    ClipMetadata, ThreadPool, BoundedQueue, CancellationToken, PauseGate,
                                    Logger, SHA-256, hardware probe + thread plan
src/codec         r3dss_codec       ICodecReader / ICodecWriter / ICodecFactory, CodecRegistry, memory codec
src/codec_cdng    r3dss_codec_cdng  TIFF/DNG IFD reader+writer, lossless JPEG (LJ92) codec, WAV, CinemaDNG codec
src/codec_red     r3dss_codec_red   RED chunk container walker (3-char tag + version, 4 KiB aligned), header strings,
                                    source detection, container-only reader
src/codec_r3dsdk  r3dss_codec_r3dsdk RED R3D SDK runtime + decoder (compiled against the SDK when R3DSDK_ROOT is set,
                                    otherwise an "unavailable" shim) and the RED-family codec factory
src/resample      r3dss_resample    tap tables, CPU per-plane resampler, output size planner
src/resample_cuda r3dss_resample_cuda CUDA resampler (same math), built when a CUDA toolkit is found
src/ratecontrol   r3dss_ratecontrol profiles, complexity estimator, VBR controller
src/pipeline      r3dss_pipeline    job model (JSON), folder scanner, archive copier, verifier, clip converter, scheduler
src/synth         r3dss_synth       synthetic mosaics and clips (tests, CLI `synth`, benchmark)
src/cli           r3dss-cli         codecs | probe | convert | scan | synth
src/gui           r3dss-gui         Qt 6 Widgets batch GUI (also `--run-job JOB.json`, headless)
bench             r3dss-bench       per-stage and end-to-end throughput
tests             r3dss-tests       Catch2
```

Dependency direction is strictly downward: core ← codec ← {codec_cdng, codec_red ← codec_r3dsdk, resample,
ratecontrol} ← pipeline ← {cli, gui, bench, tests}. Codec modules never leak SDK types.

## 2. Data flow per clip

```
FolderScanner → BatchJob → Scheduler ─┬─ clip worker ×P ─ convertClip():
                                      │     reader thread ──Q1(depth 4)──► resample workers ×W ──OrderedBuffer(2×depth)──► writer thread
                                      │     (demux+decode)                 (CPU or CUDA, + complexity)   (VBR budget → encode → mux → progress)
                                      │                                                                  → audio passthrough (1 s blocks) → finish → Verifier
                                      └─ copy thread: ArchiveCopier (copy + SHA-256 + re-hash) → manifest
```

- `BayerFrame`: `width, height, bits, cfa, black[4], white, std::vector<uint16_t> data` (row-major
  mosaic), per-frame metadata (timecode, ISO, exposure), `index`.
- Q1 is a `BoundedQueue` (depth 4). `push()` blocks when full, which is the backpressure that
  keeps a fast reader from outrunning the GPU. `pop()` blocks when empty. Both wake on
  cancellation.
- `OrderedBuffer` restores frame order after the parallel resample stage (the writer always
  sees index 0, 1, 2, …) and bounds the number of out-of-order frames to 2 × depth.
- The VBR controller runs on the writer thread because its EMA depends on frame order; the
  complexity estimate (the expensive part) is computed on the resample workers.
- Audio is copied after video in one-second blocks, so a clip never holds its whole audio in RAM.
- Every stage returns `Expected<T>`; the first error cancels the token, all threads exit, the
  writer's `abort()` removes partial output, and the clip is reported as `Failed` with the error.

## 3. Threading model

`planThreads()` in `core/hardware.hpp` (C = logical cores, G = CUDA present):

| Role | Count | Note |
|---|---|---|
| reader | 2 per clip if C ≥ 4, else 1 | NVMe-bound; the CinemaDNG reader is single-threaded today |
| decode workers | max(1, C/2) | used by CPU codecs; the TicoRAW adapter decides CPU vs CUDA |
| resample workers | G ? 2 : max(1, C/2) | one `IResampler` instance per worker, divided by parallel clips |
| encode workers | max(1, C − 2) | shared; CinemaDNG tiles are encoded on the shared pool |
| writer | 1 per clip | ordered |
| parallel clips | clamp(C/8, 1, 4), capped by RAM (3 stages × depth × frame bytes per clip ≤ ½ RAM) | user override in Settings |
| queue depth | 4 | |

Measured on the 4-core development container (synthetic 4096×2160 → 1920×1012, CPU only):
one-thread Lanczos resample 10.4 fps, four-thread 31.9 fps, lossless-JPEG encode 16 fps
single-threaded, end-to-end disk→disk with verification 5.8 fps. Treat these as a sanity check,
not a spec; see `docs/TEST_PLAN.md` for the benchmark protocol.

## 4. GPU / CPU split

- Resampling runs on CUDA when the toolkit was found at build time, the job allows it, and a
  device exists; otherwise on the CPU. Both use the same tap tables (`resample/taps.hpp`), the
  same separable two-pass structure, float accumulation and the same clip-aware fallback; the
  parity test allows ≤ 1 LSB.
- Codec work runs on the GPU only when the codec module reports `Capabilities::cuda`
  (TicoRAW on CUDA, later). CinemaDNG is CPU-only.
- Pinned host buffers and multi-stream overlap are task 19 (after the first real benchmark).

## 5. VBR and quality profiles

Unit: bits per mosaic pixel (bpp). User-reported ZR 6K/25p camera rates: N-RAW High ≈ 3.1 bpp,
Normal ≈ 1.6 bpp.

| Profile | floor bpp | cap bpp |
|---|---|---|
| Q1 Maximum | 2.4 | 4.0 |
| Q3 High | 1.6 | 3.0 |
| Q5 Normal (default) | 1.0 | 2.0 |
| Q7 Proxy | 0.5 | 1.0 |

Per frame: `c` = mean |3×3 Laplacian| over the four planes, normalised to the (white − black)
range, minus a noise floor (10th percentile of 16×16-block means, i.e. flat areas), divided by
`curveScale` (0.05) and clamped to [0, 1]. Budget = `floor + (cap − floor)·c`, smoothed with an
EMA (α = 0.3), hard-capped, multiplied by the output pixel count. The writer receives
`FrameBudget{bits, lossless}`. TicoRAW's capped-rate parameter names are **unconfirmed** until the
SDK is in hand. CinemaDNG output is lossless and ignores the budget: the CLI and GUI report such
outputs as "lossless" and disable the quality control; a lossy CinemaDNG encoder that uses the
budget is milestone M-A.

## 6. Error handling and logging

- No exceptions cross module boundaries. `Expected<T>` + `Error{code, message}`; codes:
  InvalidArgument, Io, Format, Unsupported, CodecUnavailable, Cancelled, Verification, Internal.
- A clip failure never aborts the batch; the batch summary counts done / failed / skipped /
  cancelled and the GUI row shows the message.
- `Logger` (core) writes `%LOCALAPPDATA%\r3dss\logs\r3dss-gui.log` (Windows) or
  `~/.local/share/r3dss/logs/` (Linux), rotating at 8 MiB × 3, plus stderr for the CLI and a
  callback sink for the GUI log pane. Levels trace … error.

## 7. Jobs

`pipeline/job.hpp` is the single job model. The GUI builds a `BatchJob` from its widgets; the
CLI builds one from flags or loads `--job file.json`; `r3dss-cli scan --write-job` emits one.
