# Implementation plan (approved 2026-10-15)

REDCODE via the RED R3D SDK → 16-bit RGB → resample → lossy CinemaDNG / ProRes / DNxHR.
Supersedes the 2026-10-14 Bayer-domain plan; `docs/CODESTREAM.md` records the facts that forced the change.

## Context

The repository currently implements "Path C": a Bayer-domain pipeline with a CinemaDNG reference
codec and NEV / R3D NE stubs that wait for the intoPIX TicoRAW SDK. The container survey
(`docs/CODESTREAM.md`, commits `4758d5b`, `5e94841` on `origin/claude/loving-allen-t7sfhg`) changed
the facts: the primary source is RED KOMODO REDCODE RAW, which no public code can decode at the
Bayer level or encode, and the RED R3D SDK 9.2.1 decodes it (and Nikon R3D NE / NEV) to 16-bit RGB.

New direction, decided by the maintainer:

- Pipeline: **RED SDK decode → 16-bit linear RGB → resample → re-mosaiced lossy CinemaDNG, or
  ProRes 4444/XQ, or DNxHR 444/HQX**. The "no debayer" requirement is withdrawn.
- Drop intoPIX/TicoRAW entirely (option, stubs, messages). Keep the Bayer resampler, add RGB planes.
- Nikon R3D NE and NEV go through the same RED SDK reader (future camera). A native JPEG XS-Bayer
  codec for Nikon is out of scope.
- Windows x64 first (MSVC + CMake + vcpkg); prove Windows through `windows-latest` CI; nothing
  Windows- or GPU-related is reported as verified from this Linux container.
- Apache-2.0 + `PATENTS.md`; rename the product to drop RED's "R3D" mark; keep the `r3dss-cli` name;
  no footage in git; the RED SDK never enters git.

State of the branch today: local is at `118b7d4`, origin is 3 commits ahead (`5e94841`); CI run
#6 on that head is green on Linux and Windows (installer built, 0 MSVC warnings). First action of
milestone 1 is `git pull --ff-only`.

Each milestone below is one PR with tests and green CI. Tasks are numbered globally and each names
files, responsibilities, acceptance and dependencies.

---

## Cross-cutting decisions (apply from milestone 1)

| Decision | Choice | Why |
|---|---|---|
| Product name | **Supersampler** (binary `Supersampler.exe`, installer "Supersampler", Qt org "Anchorpoint", app "Supersampler"); CLI stays `r3dss-cli`; C++ namespace `r3dss` and CMake target prefixes stay | Matches the maintainer's product site; no RED mark; renaming internals buys nothing |
| License | `LICENSE` = Apache-2.0; `PATENTS.md` lists third-party codec/patent exposure: REDCODE RAW (RED; decode only via RED's SDK under RED's license), Apple ProRes (Apple licensing programme; ffmpeg's `prores_ks` is an independent implementation), Avid DNxHR (Avid), JPEG/JPEG lossless (expired), DNG (Adobe royalty-free patent license), plus the Apache-2.0 §3 patent grant for our own code | Maintainer requirement |
| RED SDK handling | `cmake/FindR3DSDK.cmake` (already exists, fix lib names: Linux `Lib/linux64/libR3DSDKPIC-cpp11.a` + `-ldl -lpthread`; Windows `Lib/win64/R3DSDK-2017MD.lib`, `/MD`). `R3DSS_WITH_R3DSDK` becomes AUTO/ON/OFF (AUTO = on when `R3DSDK_ROOT` resolves). Runtime DLL/.so dir = `<root>/Redistributable/<win|linux>`, passed in full to `InitializeSdk()`; installed next to the exe. `.gitignore` gets `.deps/r3dsdk/` | Never commit SDK; builds stay green without it |
| SDK in CI | Job-level `env: R3DSDK_ZIP_URL: ${{ secrets.R3DSDK_ZIP_URL }}` / `R3DSDK_TOKEN`; a step `if: env.R3DSDK_ZIP_URL != ''` downloads with `Authorization: Bearer`, unzips to `.deps/r3dsdk`, exports `R3DSDK_ROOT`. Absent → module compiles as "unavailable", SDK tests `SKIP` | Maintainer requirement |
| Samples | `scripts/fetch_samples.py` (stdlib only): target dir `$R3DSS_SAMPLES` (default `.deps/samples`), skips download when the `.R3D` exists with SHA-256 `b03dfab1…9a6e`, otherwise downloads `samples-v1` zip to a cache, unzips, verifies, exits non-zero on mismatch. CI caches `.deps/samples` keyed `samples-v1`. Tests `SKIP` when the file is missing | Maintainer requirement |
| Source detection | By content, never by extension: with the SDK, from `camera_model` / `sensor_name` / `redcode` metadata; without the SDK, from the RED header TLV strings. Label format: `REDCODE RAW (KOMODO 6K)`, `R3D NE (NIKON ZR)`, `N-RAW NEV (…)` | Maintainer requirement |
| Frame types | Keep `BayerFrame`; add `RgbFrame { w, h, bits, std::array<std::vector<uint16_t>,3> planes, meta, index, ColorSpaceTag }` in core. Readers advertise `FrameLayout` (Mosaic / Rgb); the converter runs the matching path | Minimal churn to the proven Bayer code |
| Linear RGB definition | SDK decode with `ImagePipelineMode = Primary_Development_Only` (REDWideGamutRGB / Log3G10, IPP2), 16-bit interleaved → de-interleave → linearise Log3G10 → linear RWG, headroom clip at 16.0 (= +4 stops over 1.0), stored as `code = L / 16 × 65535` → `BaselineExposure = +4` so 18 % grey displays correctly | Scene-linear, documented, invertible; colour-managed by `ColorMatrix1 = inv(RWG→XYZ(D65))` |
| Re-mosaic ("pre-WB") | Neutral from the clip's Kelvin/tint via the DNG method: XYZ of the illuminant → camera (RWG) via ColorMatrix1 → normalise G = 1 → `AsShotNeutral`; pixels are multiplied by AsShotNeutral per channel (un-balancing the SDK's balanced output) before sampling R, G, G, B into an RGGB mosaic (nearest-site sample of the full-res RGB; no extra filtering because the resampler already band-limited) | A DNG reader dividing by AsShotNeutral recovers exactly the balanced image and reports the right Kelvin |
| Lossy DNG | DNG 1.4 `Compression = 34892` (lossy baseline JPEG, 8-bit, one grey component per tile) with a `LinearizationTable` (256 → 16-bit, sRGB-like curve so 8-bit codes spend bits in the shadows); encoder = libjpeg-turbo from vcpkg (BSD/IJG); the Q profile's bpp budget drives a per-frame search over JPEG quality (bisection, 3 encodes max) with the existing `VbrController` budget | Only lossy DNG the spec allows; measured ≤ 40 % target is reachable (Q5 1–2 bpp vs LQ source ≈ 4.5 MB/frame) |
| ProRes / DNxHR | vcpkg `ffmpeg[avcodec,avformat,swscale]` (LGPL, dynamic), `prores_ks` profiles 4 (4444) / 5 (4444 XQ) with `yuva444p12le`/`yuv444p12le`, `dnxhd` with `-profile dnxhr_444` / `dnxhr_hqx` (ffmpeg encodes DNxHR at 10-bit; 12-bit **unconfirmed** in ffmpeg, documented), MOV/MXF containers, PCM audio passthrough (incl. float32 → `pcm_f32le`), `timecode` metadata + `tmcd` track | Standard, redistributable, proven in Resolve |
| Frame rate | `ClipInfo` gains `timebase` (SDK `VideoAudioFramerate`) and `recordRate` (`record_framerate`); job option `frameRateMode = Timebase | Recording` written to DNG `FrameRate` tag / sidecar `.json` and to the ProRes/DNxHR stream rate; sources never modified | Maintainer requirement |

---

## Milestone 1 — Container, detection and build hygiene (PR 1)

Dependencies: none. Everything builds and tests without the SDK and without samples.

1. **Sync and licensing files.** `git pull --ff-only`; add `LICENSE` (Apache-2.0 text, copyright
   "Anchor Point Studio"), `PATENTS.md` (table above), `NOTICE`. Update `README.md` title and
   status table; `docs/FEASIBILITY.md` gets a new §0 "Decision 2026-10-15" pointing at
   `CODESTREAM.md` and marking the TicoRAW sections historical.
   Accept: files present; README reflects the new pipeline.
2. **Rename product.** `src/gui/CMakeLists.txt` OUTPUT_NAME `Supersampler`; `main.cpp` org/app names;
   window title; `installer/r3dss.iss` AppName/filenames/AppId (new GUID); CI artifact names;
   `scripts/build.ps1` zip name; docs. Keep `r3dss-cli`, `r3dss-bench`, `r3dss-tests`.
   Accept: `grep -ri "R3D Supersampler"` returns nothing outside `docs/CODESTREAM.md` history.
3. **Fix `red_container.cpp` per CODESTREAM §1.** Move `src/codec_nev` → `src/codec_red`
   (`r3dss::codec_red`), file `red_container.{hpp,cpp}`: header = 4-byte BE size + 3 ASCII tag
   bytes + 1 version byte (`Chunk{offset,size,tag(3),version}`); after each chunk round `off` up
   to the next 4096 multiple; an all-zero 8-byte header means "skip to next 4 KiB boundary"; stop
   on an unparseable header or EOF; `looksLikeRedContainer` checks tag `RED` at offset 4. Add
   `readHeaderStrings(path)` returning the printable strings (≥ 4 chars) of the `RED` chunk and,
   as a first TLV reading attempt, `readHeaderTlv()` (2-byte length, 2-byte key, value) exposed as
   `std::vector<Tlv{key, bytes}>`; it is informational until validated on the sample in task 5.
   Tests: a synthetic container written by a test helper (`tests/fixtures/red_container.cpp`:
   `RED` v50 at 0, three `RDI` v1 of odd sizes, one `RDA`, `REO`, zero gaps) must list every chunk
   with correct offsets, sizes, tags and versions; the old single-chunk file from
   `test_pipeline.cpp` must now fail to probe (tag `RED2` → tag `RED`, version 0x32 is fine; the
   test is rewritten to the new layout).
   Accept: unit test green; on the real sample (task 5) `listChunks` returns 98 chunks:
   RED, RDI×84, RDA×7, RDX, RII, RIA, RIX×2, REO.
4. **Remove TicoRAW.** Delete `ITicoRawCodec`, `makeTicoRawCodec`, `cmake/FindTicoRAW.cmake`,
   `R3DSS_WITH_TICORAW`, all "requires the intoPIX" strings, the NEV/R3D NE stub reader/writer.
   `codec_red` provides one `RedFamilyFactory` (`Format::RedFamily` replaces `Format::NEV` and
   `Format::R3DNE`; `formatFromName` keeps "nev"/"r3dne"/"r3d" as aliases) whose `probe()` =
   extension `.r3d`/`.nev` **and** `looksLikeRedContainer`; `makeReader()` returns the SDK reader
   when `codec_r3dsdk` is available, else a reader whose `open()` fills container facts and
   whose frame calls fail with `CodecUnavailable("RED R3D SDK not found: set R3DSDK_ROOT")`;
   `makeWriter()` → `Unsupported("REDCODE cannot be written; choose CinemaDNG, ProRes or DNxHR")`.
   Update `pipeline/builtin_codecs.cpp`, `clip_converter.cpp` messages, GUI container combo
   (remove NEV and R3D NE entries; keep CinemaDNG; ProRes/DNxHR arrive in M4), tests, docs
   (`CODEC_INTERFACE.md` stub section rewritten).
   Accept: `grep -ri ticoraw\|intopix src tests` empty; all tests green.
5. **Create `src/codec_r3dsdk` skeleton + samples script + CI wiring.** Target
   `r3dss_codec_r3dsdk` always exists: with the SDK it compiles `r3dsdk_reader.cpp` (M2) plus
   `r3dsdk_runtime.cpp` (init/finalize once, library-dir discovery from `R3DSS_R3DSDK_LIBDIR`
   compile definition, then `<exe dir>/redsdk`, then env `R3DSDK_LIBDIR`); without it,
   `r3dsdk_unavailable.cpp` (status `MissingSdk`). `cmake/FindR3DSDK.cmake` fixed to the real
   library names; `R3DSS_WITH_R3DSDK` AUTO. `scripts/fetch_samples.py` as specified. CI: SDK
   download step on both jobs (secrets-gated), samples cache + fetch step, `R3DSDK_ROOT` /
   `R3DSS_SAMPLES` exported; Windows job moved to `windows-latest`; Linux job runs
   `r3dss-cli probe --chunks $R3DSS_SAMPLES/*.R3D` when samples exist. New test
   `test_samples.cpp` (SKIP without samples): SHA-256 of the `.R3D` matches, `listChunks` → 98
   with the tag histogram above.
   Accept: CI green with and without the secrets (the maintainer sets the secrets; I verify the
   no-secret path here and the secret path through the Actions log).
6. **Source detection + `probe --chunks`.** `codec_red/red_detect.{hpp,cpp}`:
   `SourceKind { RedcodeRaw, NikonR3dNe, NikonNev, Unknown }`, `SourceInfo { kind, cameraModel,
   sensorName, redcode, label }`; `detectFromMetadata(map)` (SDK path: `sensor_name == "NIKON"` or
   `redcode == "R3D NE"` → NikonR3dNe; `sensor_name` starts with "KOMODO" or any other RED sensor →
   RedcodeRaw) and `detectFromHeader(strings)` (looks for "KOMODO", "NIKON", "R3D NE", camera-model
   tokens). `ClipInfo` gains `std::string sourceLabel`; the GUI Format column shows it; `probe`
   prints it and, with `--chunks`, the chunk table (offset, size, tag, version) and tag histogram.
   Tests: detection from synthetic metadata maps and header string lists.
   Accept: on the sample, `probe` prints `REDCODE RAW (KOMODO 6K)` (header-string path without
   the SDK; SDK path verified in M2).
7. **Build hygiene.** `scripts/bootstrap.ps1`: `function Invoke-Native { & $args; if ($LASTEXITCODE)
   { throw } }` around git / vcpkg / python / aqt; Python detection order `py -3`, `python3`,
   `python`; `winget install --id Git.Git`, `Kitware.CMake`, `Ninja-build.Ninja`,
   `Python.Python.3.12`, `Microsoft.VisualStudio.2022.BuildTools --override "--add
   Microsoft.VisualStudio.Workload.VCTools --includeRecommended"` for whatever is missing
   (`Get-Command` checks, VS via `vswhere`). `scripts/build.ps1`: after install, `if (-not
   (Test-Path dist\bin\Supersampler.exe)) { throw "GUI was not built: Qt6 not found (QT_ROOT=…)" }`
   and the same check for `r3dss-cli.exe`. `bench/bench.cpp`: rename the inner `std::ofstream out`
   (C4456 shadowing the `BayerFrame out`). `src/pipeline/CMakeLists.txt`: link
   `r3dss::codec_r3dsdk` unconditionally (task 5 makes it always exist), drop `codec_nev`.
   Accept: Windows CI log has zero `warning C4` lines; `build.ps1` reviewed (cannot run here).

---

## Milestone 2 — Decoder (PR 2)

Dependencies: M1. Verified locally with the SDK? Only if `R3DSDK_ROOT` is present in this
container (it is not today); otherwise verified through the Linux CI job with the secrets.

8. **`R3dSdkReader : ICodecReader`** (`src/codec_r3dsdk/src/r3dsdk_reader.cpp`), modelled on
   `tools/sdkprobe/probe_decode.cpp`. `open()`: `Clip::LoadFrom`, status check; `ClipInfo`:
   width/height, `frameCount = VideoFrameCount()`, `timebase = VideoAudioFramerate()` as a
   `Rational` (24000/1001 detection by nearest standard rate), `recordRate` from `RMD_RECORD_FRAMERATE`,
   `startTimecode` from `AbsoluteTimecode(0)` (+ `edgeTimecode` string in `meta.vendor`),
   timecode fps from `TimecodeFramerate()` (100/120 fps possible on R3D NE: `Timecode` already
   handles any integer nominal rate), audio: `AudioChannelCount()`, sample rate from
   `RMD_AUDIO_SAMPLE_RATE` (**unconfirmed key name** → fall back to 48000 and log), format from
   `RMD_AUDIO_FORMAT` (0 = 32-bit int → `PCM32`, float → `Float32`); metadata: ISO, Kelvin, tint,
   exposure/shutter, lens name, camera model/serial/firmware, `sensor_name`, `redcode`, colour
   version (min/default) and the full `MetadataItemKey/As*` dump into `meta.vendor`. Source label
   via `detectFromMetadata`.
   `readFrame(index, RgbFrame&)`: `VideoDecodeJob{DECODE_FULL_RES_PREMIUM, PixelType_16Bit_RGB_Interleaved,
   ImageProcessing = defaults with ImagePipelineMode = Primary_Development_Only}` into a 16-byte
   aligned pool buffer, de-interleave into planes, Log3G10 → linear (formula from RED's white paper:
   `lin = (10^((v − b)/a) − 1)/c` with RED's published constants; constants cited in the source
   header), clip to [0, 16], scale to 16-bit, tag `ColorSpaceTag::REDWideGamutLinear`.
   `readAudio`: `DecodeAudioBlock`/`DecodeAudio` into interleaved bytes (**API shape unconfirmed**;
   adapt to the header: the SDK gives 32-bit big-endian samples per RED docs → convert to
   little-endian PCM32 or Float32).
   GPU: `R3DSS_HAVE_CUDA` && device → `R3DSDKCuda.h` path (`REDCuda`, `AsyncDecoder`,
   `DebayerCudaJob`) with host output; R3D NE forces CPU (no GPU decompression). Marked
   **unverified** in docs until the maintainer runs it on a GPU; CPU path is the default.
   Accept: `test_r3dsdk.cpp` (SKIP without SDK or samples): info matches CODESTREAM §6 (6144×3240,
   84 frames, 24 fps, TC 09:02:10:20, ISO 12800, 4400 K, 2 ch, label `REDCODE RAW (KOMODO 6K)`),
   frame 0 decodes with plausible statistics (mean in (0.01, 0.5) of range, no NaN), audio block 0
   reads.
9. **Frame-layout plumbing.** `core/types.hpp`: `RgbFrame`, `FrameLayout`, `ColorSpaceTag`;
   `ICodecReader::layout()`, `readFrame(int64_t, RgbFrame&)` default-returns Unsupported;
   `ICodecWriter::writeRgbFrame` likewise; `Capabilities::readsRgb/writesRgb`. `clip_converter.cpp`
   gets an RGB path (reader → Q1 of `RgbFrame` → resample (M3) → writer); until M3 the RGB path
   passes frames through unchanged at "same" size only. Memory codec stores either frame type.
   Accept: existing Bayer tests unchanged and green; a new memory-codec RGB round trip test.

---

## Milestone 3 — RGB resample and lossy DNG output (PR 3)

Dependencies: M2 (for the real end-to-end), M1 for everything else; the resampler and DNG work
are testable with synthetic RGB frames without the SDK.

10. **Generalise tap tables and the CPU resampler.** `buildTaps(srcLen, dstLen, offset, kernel,
    period)` with `period = 2` (mosaic plane, current behaviour) or `1` (RGB plane: `t = (i' + 0.5)·s
    − 0.5`). `IResampler::resample(const RgbFrame&, RgbFrame&, const ResampleParams&)` runs the
    same separable two-pass code over the three full-res planes (clip-aware fallback kept; white =
    65535). Tests: constant/ramp/phase for RGB, Bayer tests untouched, multi-thread bit-identical.
11. **CUDA parity.** Same `period` parameter in `cuda_resampler.cu`; kernels already take `ox, oy`
    and stride 2 — add a stride argument. `test_cuda.cpp` gains the RGB parity case (SKIP without a
    device; the maintainer runs it; documented as unverified until then).
12. **Re-mosaic stage.** `src/resample/src/remosaic.{hpp,cpp}`: `remosaic(const RgbFrame&,
    CfaPattern::RGGB, const std::array<double,3>& asShotNeutral, BayerFrame&)` as decided above;
    `neutralFromKelvin(kelvin, tint, colorMatrix1)` in `src/resample/src/color.{hpp,cpp}` together
    with the RWG→XYZ matrix constants (REDWideGamutRGB primaries and D65 white, cited) and
    `log3g10ToLinear`. Tests: a grey RGB frame re-mosaiced then divided by AsShotNeutral per
    colour is flat; a synthetic colour chart round-trips through `remosaic` + a trivial bilinear
    debayer within 1 %.
13. **Lossy DNG writer.** vcpkg `libjpeg-turbo`; `codec_cdng`: `CdngWriterOptions::compression =
    LosslessJpeg | LossyJpeg`; lossy path per tile: 16-bit → 8-bit through the inverse of a 256-entry
    `LinearizationTable` (sRGB-style curve), baseline JPEG grey via libjpeg-turbo, tags
    `Compression 34892`, `LinearizationTable`, `BitsPerSample 8`, `WhiteLevel 255`; also writes
    `ColorMatrix1` (from the colour module), `CalibrationIlluminant1 = 21`, `AsShotNeutral`,
    `BaselineExposure` (SRATIONAL), `BlackLevel 0`. Rate control: `writeFrame` uses
    `FrameBudget.bits`: encode at the profile's starting quality, bisect quality (3 steps max) until
    bytes ≤ budget; log the chosen quality. Reader: decode `34892` via libjpeg-turbo and apply the
    linearization table (so our own verifier and tests can read it back).
    Tests: lossy round trip PSNR ≥ 45 dB at Q1 on the Scene fixture; size decreases Q1 → Q7;
    `convert` of a synthetic RGB memory clip to lossy DNG verifies.
14. **Converter wiring + acceptance.** RGB path = reader → resample → remosaic → CinemaDNG lossy;
    Bayer path unchanged. Expectation for the verifier adds `sourceLabel`/`compression` to the
    report. CI (Linux job with secrets): `r3dss-cli convert $R3DSS_SAMPLES/A060_A027_0912VZ_001.R3D
    -o out --size UHD --profile Q5 --container dng` then a script asserts output bytes ≤ 0.40 ×
    source bytes and `probe out/...` reports 3840×2025 (6144×3240 is 17:9 → height 2025, rounded
    to 2026 by the even-alignment rule; the plan uses the planner's result), 84 frames, TC
    09:02:10:20, 2 audio channels. The maintainer confirms Resolve's Camera Raw tab opens it.
    Risk: Resolve's support for lossy (34892) DNG is **unconfirmed**; fallback if it fails =
    lossless LJ92 with the mosaic quantised to 12 bits (`BitsPerSample 16`, `WhiteLevel 4095`),
    which already measured ≈ 9 bits/sample on noisy content.

---

## Milestone 4 — ProRes / DNxHR output (PR 4)

Dependencies: M2 (RGB frames), M3 (converter RGB path). vcpkg ffmpeg build is slow (30–60 min on a
cold runner): CI uses the vcpkg binary cache (`actions/cache` on `.deps/vcpkg-cache`) and the
Linux job uses apt `libavcodec-dev libavformat-dev libswscale-dev libavutil-dev`.

15. **`src/codec_ffmpeg`** (`r3dss_codec_ffmpeg`, optional AUTO on `find_package(FFMPEG)` / pkg-config):
    `FfmpegWriter : ICodecWriter` with `writesRgb`. Config: codec `ProRes4444 | ProRes4444XQ |
    DnxhrHqx | Dnxhr444`, container `.mov` (ProRes, DNxHR) or `.mxf` (DNxHR). Pixel conversion:
    RGB 16-bit linear → the codec's YUV 4:4:4 12-bit (ProRes) / 10-bit (DNxHR) via `swscale` after
    applying a documented transfer: default **Log3G10 / REDWideGamutRGB** (re-encode the SDK's log
    output 1:1, metadata tags `color_primaries/trc` unspecified + a sidecar note) — i.e. for
    ProRes/DNxHR the reader keeps the Log3G10 values and the linearisation is skipped (reader
    option `wantLinear`). Audio: `pcm_s32le` / `pcm_f32le` stream, interleaved by PTS; timecode:
    `timecode` metadata on the stream + `tmcd` track (MOV) via `AVFMT` muxer options; stream rate =
    timebase (M5 adds the option). `finish()` flushes and writes the trailer; `abort()` removes.
    `Format::ProRes`, `Format::Dnxhr` + factories; `formatFromName("prores", "prores-xq",
    "dnxhr", "dnxhr-444")`; CLI `--container dng|prores|prores-xq|dnxhr|dnxhr-444`.
16. **GUI container dropdown**: "CinemaDNG (lossy, Camera Raw)", "ProRes 4444", "ProRes 4444 XQ",
    "DNxHR HQX", "DNxHR 444"; entries disabled with a tooltip when the ffmpeg module is absent.
    Verifier: reopens MOV/MXF with `avformat` (reader side of the module: stream count,
    frames, rate, audio channels, timecode tag).
    Accept: synthetic RGB memory clip → ProRes 4444 and DNxHR HQX files that `ffprobe` (CI) reports
    with the right codec/profile, pix_fmt (`yuv444p12le` / `yuv422p10le` or `yuv444p10le`), frame
    count, audio and timecode; the sample converts in CI when the SDK secrets are present.

---

## Milestone 5 — Frame-rate fix (PR 5)

17. `ClipInfo::timebase`, `ClipInfo::recordRate` (M2 already reads both). `JobOptions::frameRateMode
    = Timebase (default) | Recording`; CLI `--frame-rate timebase|recording`; GUI checkbox "Write the
    recording frame rate (off-speed clips play at their shot speed)". DNG: `FrameRate` tag and
    `<clip>.json` sidecar `{timebase, recordRate, written}`; ProRes/DNxHR: stream `time_base`/
    `avg_frame_rate` and the `tmcd` rate. Timecode counting uses the written rate. Sources are
    opened read-only everywhere (`std::ifstream`, SDK `LoadFrom`); a test asserts the sample's
    mtime and hash are unchanged after a convert.
    Accept: a synthetic clip with `recordRate = 48/1`, `timebase = 24/1` produces 24 fps outputs by
    default and 48 fps with the option, in DNG and ProRes.

---

## Milestone 6 — Ship (PR 6)

18. **Installer.** `installer/supersampler.iss`: bundles `dist/bin` (exe, Qt DLLs via windeployqt,
    `cudart64_*.dll` when built with CUDA, ffmpeg DLLs, `redsdk/` = `Redistributable/win` DLLs from
    the CI-fetched SDK) and the VC++ 2022 redistributable (`vc_redist.x64.exe` downloaded in CI and
    run `/install /quiet /norestart` from `[Run]`). `docs/REDISTRIBUTION.md` quotes/cites RED's SDK
    redistribution terms as read by the maintainer (unconfirmed wording until then) and lists every
    bundled third-party file with its license.
19. **Release workflow.** `windows-latest` job builds the installer on every push (artifact) and
    attaches `Supersampler-<tag>-setup.exe` + portable zip to `v*` tag releases
    (`softprops/action-gh-release`, already wired). Installer is built even without the SDK
    (then without the `redsdk/` folder; the app reports `MissingSdk`).
20. **CI end-to-end on samples.** Both jobs (when samples + SDK present): decode → resample →
    encode (DNG, ProRes, DNxHR) → verify, size assertion, `ffprobe` assertions; results uploaded
    as artifacts (`probe.json`, `verify.json`, sizes). Without the secrets the steps are skipped
    and the job still passes.

---

## Verification (what I can and cannot prove here)

- Linux container: `cmake --preset linux-ci`, build, `ctest`, `r3dss-cli probe --chunks` on the
  downloaded sample (98 chunks), synthetic RGB → resample → remosaic → lossy DNG round trips, GUI
  offscreen smoke. If the maintainer places the SDK at `R3DSDK_ROOT` in this container, the M2/M3
  acceptance runs here too; otherwise it runs in the Linux CI job with the secrets.
- Windows: only through the `windows-latest` CI job (build, tests, installer artifact). Reported as
  "CI green", never as locally verified.
- GPU (CUDA resampler RGB parity, SDK CUDA decode): the maintainer runs `r3dss-tests "[cuda]"` and a
  GPU convert; documented as unverified until then.
- Resolve: the maintainer opens the M3 DNG and M4 MOV/MXF outputs; results recorded in
  `docs/TEST_PLAN.md`.

## Open questions (defaults applied unless you object)

1. Product name **Supersampler** (binary `Supersampler.exe`).
2. Linear encoding for DNG: **RWG-linear with +4 stops headroom, BaselineExposure 4**; ProRes/DNxHR
   carry **Log3G10/RWG as decoded** (no baked look). Alternative: Full_Graded IPP2 output to Rec.709.
3. Lossy DNG = **DNG 1.4 lossy JPEG (34892) with LinearizationTable**; fallback to 12-bit lossless
   if Resolve rejects it.
4. ffmpeg on Windows via **vcpkg `ffmpeg[avcodec,avformat,swscale]`** (LGPL build, dynamic).
   Alternative: prebuilt gyan.dev shared build downloaded by `bootstrap.ps1` (faster CI, same license).
5. GUI "GPU" setting also governs SDK CUDA decode (one switch).
