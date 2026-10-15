# Patents and third-party codec rights

Supersampler's own code is licensed under Apache-2.0, whose section 3 grants every recipient a
patent license covering the contributors' own patent claims. The formats it reads and writes are,
however, controlled by third parties. This file records what we know so that users and
distributors can judge their own exposure. It is not legal advice.

| Technology | Rights holder | How Supersampler uses it | Status |
|---|---|---|---|
| REDCODE RAW (`.R3D`), incl. the Nikon R3D NE and NEV variants | RED Digital Cinema (patents on compressed raw video recording, e.g. US 8,174,560 and related family) | **Decode only**, exclusively through RED's own R3D SDK under the RED R3D SDK License Agreement. We never parse, decode or encode the REDCODE essence ourselves; the repository only walks the public chunk container. No encoder exists and none will be written. | SDK terms must be accepted by whoever downloads it; the installer's redistribution of RED's runtime libraries is documented in `docs/REDISTRIBUTION.md` (milestone 6). |
| DNG / CinemaDNG | Adobe Inc. | Read and write. | Adobe grants a royalty-free patent license for DNG-compliant implementations (DNG specification, "Patent license"). |
| Lossless JPEG (ITU-T T.81 process 14) | expired | Read and write (DNG compression 7). | Patents expired. |
| Baseline JPEG (ITU-T T.81) | expired | Lossy DNG (compression 34892) via libjpeg-turbo (milestone 3). | Patents expired; libjpeg-turbo is BSD/IJG licensed. |
| Apple ProRes | Apple Inc. | Encode via FFmpeg's independently developed `prores_ks` encoder (milestone 4). | Apple licenses ProRes to implementers; FFmpeg's encoder is not an Apple-licensed implementation. Commercial distributors should assess this themselves. |
| Avid DNxHR | Avid Technology, Inc. | Encode via FFmpeg's `dnxhd` encoder (milestone 4). | Avid publishes DNxHR as a SMPTE-registered format (VC-3, SMPTE ST 2019); patent licensing is between the distributor and Avid. |
| intoPIX TicoRAW / JPEG XS | intoPIX | **Not used.** The earlier plan to decode or encode Nikon N-RAW natively was dropped on 2026-10-15. | Historical note in `docs/FEASIBILITY.md`. |

If you distribute binaries, review the RED R3D SDK License Agreement and the ProRes and DNxHR
positions above for your jurisdiction and business.
