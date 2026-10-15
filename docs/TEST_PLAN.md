# Test plan

Run: `cmake --preset linux-ci && cmake --build --preset linux-ci && ctest --preset linux-ci`
(Windows: the `windows-release` presets). Framework: Catch2 v3; 31 test cases today.

## 1. Fixtures (`src/synth`)

`synth::makeFrame()` patterns: Constant, PlaneConstants (a distinct constant per plane), RampX,
RampY, ZonePlate, Impulse, Noise, Scene (gradient + moving clipped highlight disc with soft
shoulder + fine-detail band + mild noise). `writeSyntheticClip()` writes a CinemaDNG clip with
float32 WAV audio, timecode and full metadata; `makeMemoryClip()` builds the same in the
in-memory codec; `r3dss-cli synth` exposes the generator to users.

## 2. Unit tests

| Area | File | Checks |
|---|---|---|
| core | `test_core.cpp` | Expected/TRY macros; CFA helpers; timecode ↔ frames incl. 29.97 drop-frame; SHA-256 known answers; BoundedQueue blocking, draining, cancel; ThreadPool; thread plan policy |
| taps | `test_taps_resampler.cpp` | weights sum to 1, in-range starts; identity = single tap; phase mapping gives the (0.125, 0.5, 0.375) Area taps at 2× |
| resampler | same | constant and per-plane constants exact; ramp within 1 LSB; clip-aware never exceeds white and differs from plain Lanczos; multi-thread bit-identical to single-thread; Area halves noise variance; upscale refused; size planner presets |
| rate control | `test_ratecontrol.cpp` | flat frame → floor; zone plate > flat; budget inside [floor, cap]; EMA monotone to the cap |
| LJ92 / TIFF / WAV | `test_ljpeg_tiff_wav.cpp` | lossless JPEG round trip for 1/2/4 components, predictors 1–7, 12-bit, 16-bit extremes incl. the +32768 category; compression ratio sanity; TIFF writer → reader; WAV float32 / pcm24 / pcm16 round trips and partial reads |
| CinemaDNG | `test_cdng.cpp` | SMPTE timecode tag; writer → reader round trip (lossless JPEG and uncompressed) of samples, CFA, levels, fps, ISO, exposure, white balance, colour matrix, lens, DNGPrivateData blob, reel name, per-frame timecode, float32 audio |
| pipeline | `test_pipeline.cpp` | job JSON round trip and naming; memory clip end to end (ordering, audio, timecode, equality with a direct resample); CinemaDNG disk end to end with verification, skip-if-exists, overwrite, upscale skip, R3D NE refusal; folder scan + archive copy + hash verify + scheduler + manifest; cancellation; NEV/R3D NE stub probe and status |
| CUDA | `test_cuda.cpp` | CPU/CUDA parity ≤ 1 LSB for both kernels; skipped without a device |
| RED container | `test_red_container.cpp` | synthetic container (3-char tags, versions, 4 KiB alignment, zero gaps, truncation); source detection from SDK metadata and header strings; RED-family factory honesty (reads only with the SDK, never writes) |
| samples | `test_samples.cpp` | `[samples]`: SHA-256 of the Komodo clip, 98 chunks, tag histogram, `REDCODE RAW (KOMODO 6K)` label; skipped without `R3DSS_SAMPLES` |

## 3. Integration tests (sample clips)

`scripts/fetch_samples.py` downloads the `samples-v1` release (RED KOMODO 6K clip, CC0) into
`$R3DSS_SAMPLES` and verifies its SHA-256; CI caches it. Without the RED SDK the tests cover the
container and detection only; with the SDK (CI secrets `R3DSDK_ZIP_URL` / `R3DSDK_TOKEN`) milestones
2–6 add decode → resample → encode → verify runs on the sample.

Manual Resolve checklist for every release:
1. Import an output CinemaDNG folder (and NEV once available) into Resolve 20.
2. Media page: resolution, frame rate, duration and audio channels match the source.
3. Timecode of the first frame matches the source clip.
4. Colour page, Camera RAW: white balance and ISO read as expected; no colour fringing on
   edges (phase correctness); no halos around clipped highlights at *Full Res Premium*.
5. Compare against the source at matched size.

## 4. Benchmark harness

`r3dss-bench --width W --height H --frames N --size UHD|FHD|6K [--json out.json] [--no-gpu]`
reports frames per second for: synthetic generation, CPU resample (1 thread and all threads),
CUDA resample (when built), complexity estimate, lossless-JPEG encode, the full pipeline on the
memory codec, writing the synthetic source, and the full disk→disk pipeline with verification,
plus MiB written. Report throughput as fps at the stated source and output size, the hardware
(CPU model, cores, GPU, drive), and attach the JSON. CI runs a small benchmark on every push
and uploads `bench-linux.json`; numbers from CI runners are indicative only.
