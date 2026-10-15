# Codestream and container survey

Status: **in progress**. First survey done 2026-10-14 on one clip with `tools/r3d_survey.py`.
This file is the source of truth for what we have actually observed in files, as opposed to
what vendor pages claim (`docs/FEASIBILITY.md`).

## 1. RED container (`.R3D`) — observed

Clip: `A060_A027_0912VZ_001.R3D`, 379,416,576 bytes, **RED KOMODO 6K** (serial KMDBK008429,
firmware 1.8.6, "6K 17:9", shot 2026-09-12, 84 frames). Note: this is native REDCODE RAW, not
Nikon R3D NE. See §3.

Container rules (these contradict the current `src/codec_nev/src/red_container.cpp`):

| Rule | Observed |
|---|---|
| Chunk header | 4-byte big-endian size, then **3 ASCII tag bytes + 1 version byte** (e.g. `RED` v50, `RDI` v1, `REO` v66). The repo treats byte 8 as ASCII and stops at chunk 1. |
| Alignment | Every chunk starts on a **4096-byte boundary**; gaps are zero-filled. The repo walks `offset += size` with no alignment and stops at the first zero header. |
| Order | `RED` header (914 B) → interleaved `RDI` video frames (~4.5 MB each) and `RDA` audio blocks (192,128 B, 7 blocks for 84 frames → one per 12 frames) → `RDX` v2 (119 KB) → `RII`, `RIA`, `RIX` v1, `RIX` v2 (small indexes) → `REO` v66 trailer (962 B, a copy of the header with end timecode). |
| Metadata | Header and trailer carry a TLV list (2-byte length, 2-byte key, value) with camera model, serial, firmware, reel/clip, date, start/end timecode, lens, format name, colour/ISO floats. Each `RDI` frame repeats a ~4.6 KB metadata block before the payload. |
| Frame payload | Starts ≈ 4,736 bytes into the `RDI` chunk, entropy 7.77 bits/byte (compressed), trailing zero padding to 4 KiB. |

Fix required in `red_container.cpp`: parse `tag[3] + ver`, honour 4 KiB alignment, skip zero
gaps. Then `listChunks` returns all 98 chunks instead of one.

## 2. JPEG XS marker check — Komodo clip

No `FF10 FF50` (SOC+CAP) sequence anywhere in the frame; no PIH/CDT/NLT at the payload start.
**REDCODE RAW is not a JPEG XS codestream** (expected — it is RED's own wavelet codec and there
is no public encoder or Bayer-level decoder for it). Nothing in this clip answers the Nikon
question.

## 3. What is still unknown — Nikon R3D NE / NEV

We have **no Nikon clip surveyed yet**. The whole Path-A plan (own JPEG XS-Bayer codec) rests on
the Nikon essence being JPEG XS Bayer or a close TicoRAW dialect. To settle it we need, from a
Nikon ZR / Z8 / Z9 / Z6 III:

1. One R3D NE clip (ZR) and one NEV clip, 1–2 s each.
2. Run `python tools/r3d_survey.py CLIP` and record here: container tag set (expect the RED
   wrapper for R3D NE; NEV may be a different wrapper), payload start, and whether `FF10 FF50`
   appears at the payload start. If it does, parse the PIH (`FF12`) and record profile
   (`Ppih`), level, `Wf/Hf`, `Nc`, bit depth `Bw`, `Cpih` (colour transform; Star-Tetrix for
   Bayer) and the NLT segment.
3. Only then decide: conformant JPEG XS Bayer → start from SVT-JPEG-XS; dialect → spec-diff.

## 4. Consequence for the product

* **Nikon sources** (the original target): still viable via own JPEG XS-Bayer codec, pending §3.
* **RED KOMODO / REDCODE sources**: Bayer-domain resample and native re-encode are **not
  possible** with any public code or SDK. The only workable route is RED R3D SDK decode → RGB
  resample → DNG (re-mosaiced) or ProRes/DNxHR — i.e. the "Path B" in FEASIBILITY.md. If
  REDCODE sources matter to users, Path B must ship alongside Path A, not instead of it.

## 5. Sample corpus

Release `samples-v1` on GitHub holds the Komodo clip above (shot by the maintainers, CC0, test
use). Add Nikon clips there as they become available; tests read `R3DSS_SAMPLES`.
