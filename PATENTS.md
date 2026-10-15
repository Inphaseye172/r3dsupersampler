# Patents and third-party codec rights

This project's own code is licensed under Apache-2.0, whose section 3 grants every recipient a
patent license covering only the contributors' own patent claims. **Apache-2.0 does not license,
and cannot sublicense, any third-party SDK, codec, format or patent.** The formats this project
reads and may write are controlled by third parties; each binary release needs its own review of
those rights (tracked as dependency D8 in `docs/IMPLEMENTATION_PLAN.md`). This file records what
we know so that users and distributors can judge their own exposure. It is not legal advice.

| Technology | Rights holder | How this project uses it | Status |
|---|---|---|---|
| REDCODE RAW (`.R3D`), incl. the Nikon R3D NE and NEV variants | RED Digital Cinema (patents on compressed raw video recording, e.g. US 8,174,560 and related family) | **Decode only**, exclusively through RED's own R3D SDK under the RED R3D SDK License Agreement. We never parse, decode or encode the REDCODE essence ourselves; the repository only walks the chunk container. No public encoder exists. | SDK terms must be accepted by whoever downloads it; whether an installer may bundle RED's runtime libraries is governed by RED's agreement and is unresolved (D8). |
| DNG / CinemaDNG | Adobe Inc. | Read and write. | Adobe grants a royalty-free patent license for DNG-compliant implementations (DNG specification, "Patent license"). |
| Lossless JPEG (ITU-T T.81 process 14) | expired | Read and write (DNG compression 7). | Patents expired. |
| Baseline JPEG (ITU-T T.81) | expired | Candidate for lossy CinemaDNG output (whether DNG permits it for mosaics is being verified; see `docs/FEASIBILITY.md`). | Patents expired; libjpeg-turbo is BSD/IJG licensed. |
| Apple ProRes | Apple Inc. | Not used. ProRes/DNxHR would only ever be optional *developed-image* exports, never the product's RAW output, and are not scheduled. | Apple licenses ProRes to implementers. |
| Avid DNxHR | Avid Technology, Inc. | Not used (see ProRes). | Avid licensing applies if ever added. |
| intoPIX TicoRAW / JPEG XS (Nikon N-RAW, R3D NE essence) | intoPIX (patents on TicoRAW / JPEG XS; JPEG XS also has a patent pool) | **Not used yet.** The only known route to native Nikon RAW-to-RAW; requires intoPIX's commercial SDK and license (dependency D3). | No rights held. |

If you distribute binaries, review the RED R3D SDK License Agreement, any intoPIX agreement and the
positions above for your jurisdiction and business. The release build refuses to package unless
`R3DSS_THIRD_PARTY_TERMS_ACKNOWLEDGED=ON` records that this review was done.
