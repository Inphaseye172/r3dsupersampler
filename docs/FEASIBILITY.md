# Feasibility: REDCODE RAW, R3D NE and NEV sources

## 0. Requirement and capability matrix (2026-10-16, supersedes the sections below)

**Requirement (binding):** genuine RAW in, genuine RAW out at a lower resolution and smaller size,
keeping frame rate, duration, audio, timecode and the RAW grading workflow. The sensor mosaic is
resampled in the Bayer domain and never debayered. Developed RGB re-mosaiced into DNG, ProRes or
DNxHR does **not** satisfy it; such outputs could only ever be optional, clearly labelled
derivatives. An earlier version of this section said the "no debayer" requirement was withdrawn;
that was wrong and is retracted.

"Mosaic decode" is an API that returns the undeveloped sensor mosaic; "RAW encode" is one that
writes the camera's RAW format from a mosaic. RAW-to-RAW needs both.

| Source | Mosaic decode | RAW encode | Status |
|---|---|---|---|
| **REDCODE RAW (RED KOMODO)**, the maintainer's camera (`samples-v1`: KOMODO 6K, 6144×3240, 24 fps, `redcode` LQ) | **none**: the RED R3D SDK (9.2.1 verified, `docs/CODESTREAM.md` §6) outputs developed RGB only; `VideoPixelType` has no CFA type | **none**: the SDK only trims or snapshots existing compressed frames; nobody licenses a REDCODE encoder publicly | **Blocked.** No public or licensable component exposes the KOMODO mosaic or writes REDCODE. Only RED could change this (dependency D1). The SDK is used to identify and verify clips |
| **Nikon R3D NE / NEV** | RED SDK: RGB only. A JPEG XS-Bayer (TicoRAW) decoder might read the essence if it is standard, **unconfirmed** without a Nikon clip and the vendor SDK | the same vendor's encoder, plus our own container writer; **unproven** | **Possible, unproven** (milestone M-B: evidence first) |
| **CinemaDNG** (mosaic) | yes, our own reader (lossless JPEG and uncompressed, 8–16 bit, every CFA, IFD0 and SubIFD layouts, ActiveArea, LinearizationTable, opcodes) | yes, our own lossless writer; a lossy (DNG 1.4 compression 34892) quality-driven encoder is milestone M-A | **Working.** Each release is checked pixel for pixel against LibRaw on Linux and on the installed Windows package (`scripts/verify_cdng.py`) |
| Blackmagic RAW, ProRes RAW, Canon RAW Light, ARRIRAW | SDKs return developed images or nothing public | none public | not RAW-to-RAW candidates |

Consequences:

1. For the KOMODO the requirement cannot be met with any component that can be obtained today.
   That is a hard blocker, stated in the product and its release notes, not hidden behind an RGB
   substitute.
2. Nikon sources are the realistic RAW-to-RAW target once the evidence in milestone M-B exists.
3. CinemaDNG mosaic to smaller CinemaDNG mosaic is fully provable now and is what the product does.

Licensing: Apache-2.0 covers only this project's code. The RED SDK runtime, any Nikon or vendor
codec, Qt (LGPL-3.0) and other bundled libraries keep their own terms (`NOTICE`,
`third_party/licenses`); production releases require the maintainer's confirmation of those terms
(`R3DSS_THIRD_PARTY_TERMS_ACKNOWLEDGED`). Supersampler.app is an unrelated competitor.

Sections 1–5 below are the 2026-10-14/15 research, kept as history. Their recommendations (the
SDK-RGB "Path C" and the withdrawn-requirement plan) are not current.

---


Status: research completed 2026-10-15 from public sources. Items marked **unconfirmed** could not
be verified from primary documentation (the RED, Nikon, intoPIX and product web pages were not
reachable from the research environment; search-engine excerpts and the publicly mirrored RED SDK
headers plus FFmpeg's `r3d.c` were). Nothing in this document is a legal opinion.

## 1. R3D NE

### 1.1 Decode

| Fact | Source | Confidence |
|---|---|---|
| The RED R3D SDK decodes R3D NE since SDK 9.1.0; 9.1.2 added a custom I/O backend "for N-RAW & R3D NE clips"; 9.2.0 fixed a crash loading them. | RED download page release notes (via search excerpt) | high, exact latest version **unconfirmed** |
| Platforms: Windows, macOS, Linux x64. GPU decode via CUDA and OpenCL (Metal on Apple). | RED SDK page | high |
| The SDK outputs **debayered RGB only**. `VideoPixelType` = 16-bit RGB planar / interleaved, 8-bit BGRA / BGR, 10-bit DPX, 12-bit BGR, half-float RGB, half-float ACES. There is no Bayer / CFA / mosaic output and no "no debayer" option in `ImageProcessingSettings`. | mirrored `R3DSDKDefinitions.h` (SDK 7 header) | high for v7; **unconfirmed** for 9.x, but RED has never advertised raw-mosaic output |
| Free download after registration; SDK DLLs may be redistributed with an application (per the READMEs of third-party bindings); headers may not be redistributed. | RED legal page excerpt, `r3d-rs` README, header notice | medium, full license text **unconfirmed** |

Consequence: the RED SDK can open and verify R3D NE and NEV clips and give us RGB thumbnails and
metadata, but it cannot feed a Bayer-domain pipeline.

### 1.2 Encode / write

| Fact | Source | Confidence |
|---|---|---|
| The SDK's only "write" APIs are `CreateTrimFrom` (frame-range trim of an existing clip, no re-encode), `CreateSnapshotFrom` (one-frame clip) and RMD sidecar / LUT export. | mirrored `R3DSDK.h` | high |
| No REDCODE encoder has ever been offered in a public SDK; no third-party software encodes R3D. | RED SDK scope statement, community consensus | high |
| The RED SDK license bans reverse engineering and "discovering file formats". | license excerpt | medium (**unconfirmed** wording) |

**Verdict: there is no supported way to write R3D NE.** The plan does not pretend otherwise. R3D NE
is an input format only; the "R3D NE" output choice in the UI is disabled with that reason.

### 1.3 Container relationship to NEV

- RED's support article on R3D NE says a NEV renamed to `.R3D` "is still N-RAW internally" and
  renders with different colour, exposure and latitude than a native R3D NE clip.
- A third-party macOS utility converts NEV to R3D by renaming plus patching one FourCC
  (`iNRAW` → `iNR3D`). Resolve then treats the file as R3D NE.
- Older R3D files use a chunked container: 4-byte big-endian size + 4-character tag, file starts
  with `RED1` (FFmpeg `libavformat/r3d.c`); data-recovery vendors describe `RED2` headers with
  `RDI`/`RDA`/`RII` chunks aligned on 4096 bytes.

Inference (**unconfirmed** until sample files are inspected): NEV and R3D NE share one RED-style
chunked container and one TicoRAW essence, differing in codec FourCC and metadata. This matters
because one demuxer could serve both inputs, and because a NEV *muxer* is the only writer we can
ever build.

## 2. NEV / N-RAW

| Fact | Source | Confidence |
|---|---|---|
| N-RAW is intoPIX TicoRAW, "customised" for Nikon. 12-bit. | Nikon and intoPIX press releases | high |
| TicoRAW codes Bayer data directly (RGGB and other CFA, 8/10/12/16-bit), wavelet based, CBR or capped VBR from about 2:1 to 20:1 (≈0.5 bpp floor), plus a mathematically lossless profile since FastTicoRAW v2.8. | intoPIX product pages | high (vendor claims) |
| Decoders: RED R3D SDK (RGB output; Nikon says Premiere's N-RAW support is "via the addition of Nikon N-RAW support to RED's R3D SDK"); intoPIX FastTicoRAW SDK (CPU x64/ARM64 and CUDA, Bayer output, evaluation on request, commercial license). | Nikon press release, intoPIX pages | high |
| Whether the generic intoPIX SDK decodes Nikon's customised streams. | — | **unconfirmed** |
| The NEV container specification. | — | **unconfirmed**, no public spec, no FFmpeg support, no open-source decoder found |
| A Nikon video SDK. | Nikon developer site lists camera-control and NEF still-image SDKs only | none found |
| Encoder: intoPIX FastTicoRAW SDK only. | intoPIX | high |

## 3. Licensing table

| Component | Role in this product | License / terms | Ship in a commercial Windows app? |
|---|---|---|---|
| RED R3D SDK | Decode R3D NE and NEV to RGB for probing, QC thumbnails, verification | RED R3D SDK License Agreement; free; DLL redistribution with your app (per third-party READMEs); header redistribution prohibited; no reverse engineering / file-format discovery. Full text **unconfirmed**. | Yes for decode, after reading the full agreement. Never commit the headers. |
| intoPIX FastTicoRAW SDK | Bayer decode and encode of TicoRAW essence, CPU and CUDA | Commercial per-product license, evaluation first; pricing / royalties **unconfirmed**; patented. | Yes with a signed license. Must be obtained. |
| NEV / R3D NE container knowledge | Demux and mux | No public spec. Either Nikon under NDA (**unconfirmed** that Nikon offers one) or clean-room analysis of camera files, done without the RED SDK and after legal review. | Depends on legal review. |
| Qt 6 Widgets | GUI | LGPLv3 with dynamic linking (ship the relink notice) or a commercial Qt license | Yes (LGPL, dynamic). |
| CUDA Toolkit runtime (`cudart`) | GPU resampler | NVIDIA EULA allows redistributing the runtime | Yes. |
| Catch2 | tests only | BSL-1.0 | Not shipped. |
| nlohmann-json, CLI11 | engine / CLI | MIT, BSD-3 | Yes. |
| CinemaDNG / DNG | reference codec | Adobe DNG specification, royalty-free patent license | Yes. |
| Inno Setup | installer | Inno Setup license (free, incl. commercial) | Yes. |

## 4. Verdict and paths

1. **Path A, fully native (target).** intoPIX SDK decodes and encodes the TicoRAW essence on the
   Bayer mosaic; our own demuxer reads NEV and R3D NE; our own muxer writes NEV; RED SDK optional
   for QC. Needs: intoPIX license, sample files, legal review, confirmation that intoPIX decodes
   Nikon streams, and a container spec or analysis. R3D NE *output* remains unsupported.
2. **Path B, RED SDK RGB decode + re-mosaic.** Works today for any input, but debayers and then
   re-mosaics, which violates the core requirement and discards raw fidelity. **Rejected.**
3. **Path C, stub codec layer with a clean interface + an open reference codec.** Everything but
   the proprietary codec modules is built and proven now; the modules drop in later.

**Recommendation: build Path C now, converging on Path A.** The repository implements Path C:
the codec interface (`docs/CODEC_INTERFACE.md`), a complete CinemaDNG reader/writer, and NEV /
R3D NE modules that recognise the files, walk the chunk container, and report exactly why they
cannot decode yet.

## 5. What you need to check or purchase

1. Register with RED, download the current R3D SDK, read the full license, and confirm in
   `R3DSDKDefinitions.h` that no Bayer output exists in 9.x.
2. Request the intoPIX FastTicoRAW SDK evaluation (CPU + CUDA). Ask explicitly: does it decode
   Nikon N-RAW / R3D NE codestreams, and can a per-frame bit budget be set (capped-rate mode)?
3. Ask Nikon developer relations for an NEV container specification or NDA.
4. Download Nikon's ZR R3D NE and NEV sample clips and Z9 / Z8 N-RAW clips as fixtures; keep them
   outside the repository and point `R3DSS_SAMPLES` at them.
5. Trademark review of the product name: "R3D" is a RED mark; "N-RAW" and "NEV" are Nikon's.
