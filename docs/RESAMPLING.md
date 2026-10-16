# Bayer-domain resampling

Implementation: `src/resample/src/taps.cpp` (tap tables), `cpu_resampler.cpp`,
`src/resample_cuda/src/cuda_resampler.cu`. No debayer exists anywhere in the pipeline.

## 1. Planes

The mosaic `M[y][x]` (W×H, both even) has a 2×2 CFA period. Plane `p ∈ {0,1,2,3}` has offset
`(ox, oy) = (p & 1, p >> 1)` and samples `P_p[j][i] = M[2j + oy][2i + ox]`, size W/2 × H/2.
For RGGB the planes are R, Gr, Gb, B; the code never needs to know which is which because every
plane is resampled with the same geometry rule, and the output mosaic keeps the input CFA tag.

## 2. Geometry (phase-correct mapping)

Output W'×H' (even), scale `sx = W/W'`, `sy = H/H'` (equal in practice). The output site of
plane p at plane index `i'` sits at mosaic coordinate `u = (2i' + ox + 0.5)·sx`. On the source
plane grid, whose samples sit at mosaic coordinates `2n + ox + 0.5`, that is the continuous
position

```
t = (u − ox − 0.5) / 2
```

Each plane is resampled where its sites really are. For `sx = 2` this gives `t = 2i' + 0.25`
for offset 0 and `t = 2i' + 0.75` for offset 1, so the two planes of a row are sampled at
different phases, exactly as they are in the sensor. Treating the four planes as
origin-aligned images would shift them against each other by up to half an output plane
pixel and produce colour fringes on every edge after debayering.

Identity (`sx = 1`) gives `t = i'`: a single unit tap, so "same resolution" is bit-exact.

## 3. Filters

Separable, two passes per plane (horizontal on every source plane row into a float buffer, then
vertical into the output). `sEff = max(1, s)` widens the kernel when downscaling (anti-aliasing).

- **Lanczos-3**: taps `n ∈ [ceil(t − 3·sEff), floor(t + 3·sEff)]`, `w_n = lanczos3((n − t)/sEff)`.
- **Area**: window `[t − sEff/2, t + sEff/2]`, `w_n` = overlap of `[n − ½, n + ½]` with the window.
  For `s = 2` and offset 0 this is the 3-tap `(0.125, 0.5, 0.375)` box, not the plain 2×2 bin:
  the plain bin is a phase shift. Area is the "lowest noise, no ringing" option.

Weights are folded onto the edge samples (clamp-to-edge), normalised to sum 1, precomputed once
per (geometry, kernel) as `TapTable{start[], count[], weights[dst × maxTaps]}` per axis and
plane offset, and shared by CPU and CUDA.

## 4. Clip-aware fallback

An output sample is recomputed with the 2-D Area taps directly on the source plane when either
any Lanczos tap input on either pass is ≥ `white − 1`, or the Lanczos result itself is ≥
`white − 1`. The second condition stops overshoot next to bright but unclipped detail from creating
new saturated sites that raw developers would treat as clipped: an area average of inputs that are
all below `white − 1` cannot reach it. Lanczos' negative lobes otherwise produce dark
halos and over-white overshoot around clipped highlights, the same artefact as a half-resolution
R3D decode in Resolve (`docs/RESOLVE_NOTE.md`). The flag is carried through the horizontal pass
as a byte per intermediate sample. The fallback is skipped for the Area kernel.

## 5. Levels and bit depth

Weights sum to 1, so a constant plane maps to itself: black level and white level pass through
unchanged (unit tests check this exactly). Arithmetic is float32, output is
`clamp(round(v), 0, min(2^bits − 1, white))` written as `uint16_t`, so no output sample exceeds
the white level; the frame keeps `bits`, `black[4]`,
`white`, `cfa` and metadata. No dithering (raw data, the camera noise already dithers).

## 6. Pseudocode

```
for p in 0..3:
  (ox, oy) = (p & 1, p >> 1)
  TX = taps(W, W', ox, kernel);  TY = taps(H, H', oy, kernel);  AX, AY = taps(..., Area)
  for j in 0..H/2:                                  # horizontal pass
    for i' in 0..W'/2:
      tmp[j][i'] = Σ_k TX.w[i'][k] · P_p[j][TX.start[i'] + k]
      clip[j][i'] = any input ≥ white − 1
  for j' in 0..H'/2:                                # vertical pass
    for i' in 0..W'/2:
      v = Σ_k TY.w[j'][k] · tmp[TY.start[j'] + k][i']
      if any clip[TY.start[j'] + k][i']: v = Σ_ky Σ_kx AY.w · AX.w · P_p[..][..]
      M'[2j' + oy][2i' + ox] = clamp(round(v), 0, maxCode)
```

The CUDA version launches one thread per (output column, source row) for the horizontal pass and
one per output sample for the vertical pass; the fallback is evaluated in the vertical kernel.

## 7. Output sizes

`planOutputSize()` keeps the source aspect ratio, rounds to the codec's alignment (CinemaDNG 2,
TicoRAW 8 **unconfirmed**) and refuses to upscale (the clip is skipped with a message).

Heights follow the source aspect; two common ones:

| Preset | Width | 16:9 source | 17:9 source (e.g. 6144×3240) |
|---|---|---|---|
| 6K | 6048 | 3402 | 3190 |
| UHD | 3840 | 2160 | 2026 |
| 2K (DCI) | 2048 | 1152 | 1080 |
| FHD | 1920 | 1080 | 1012 |

A preset wider than the source is skipped (no upscaling): 6K from a 4096-wide clip writes nothing
and the CLI exits with code 3. A custom `WxH` is used exactly; when its aspect differs from the
source by more than 0.5 % the CLI warns, and the CinemaDNG writer sets `DefaultScale` so the
displayed aspect stays the source's (non-square pixels instead of a squeezed picture).

## 8. Resolution of a per-plane resample

Each CFA plane is filtered to its own Nyquist frequency at the output size, which is half the
output's pixel Nyquist. Detail finer than that (for example a stripe pattern whose period is
under about 4 output pixels) is removed instead of aliasing into moiré and false colour. A
developed image of the output is therefore softer on very fine detail than the source debayered at
full resolution and then scaled down; smooth areas, colour and exposure are the same. This is
inherent to keeping the result a genuine, undeveloped mosaic: a demosaic of the source would be
needed to keep that detail, and the output would no longer be RAW.

## 9. Expected quality versus a naive 2×2 bin

- Bin (plain 4×4 super-pixel): positive weights, no ringing, best noise reduction, but a
  `sinc` response that passes aliases above the output Nyquist, softens detail near it, and
  (as a plain bin) shifts each plane's phase.
- Area here: same noise and ringing behaviour as the bin, phase-correct.
- Lanczos-3: preserves detail to roughly 0.9 × Nyquist with far less aliasing; small ringing on
  hard edges, suppressed next to clipping by the fallback; slightly less noise reduction.

The benchmark harness (`r3dss-bench`) resamples the zone-plate fixture with both kernels so
MTF50 and alias energy can be measured; no numbers are claimed in advance.
