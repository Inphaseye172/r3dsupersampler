# DaVinci Resolve: half-resolution decode ringing

**Symptom.** Clipped highlights in R3D NE (and renamed NEV) clips show dark halos and overshoot
at hard edges when Resolve decodes at half resolution. The artefact is in the decode, not in
the file.

**Why.** Reduced-resolution RED decodes use a fast wavelet-level shortcut; near clipped
values the reconstruction filter rings. The RED SDK exposes the same modes
(`DECODE_HALF_RES_GOOD`, `DECODE_HALF_RES_PREMIUM`, `DECODE_FULL_RES_PREMIUM`).

**Fix.** Project Settings → Camera RAW → RED → *Decode quality: Full Res Premium*, or set it per
clip in the Camera RAW palette. Timeline playback resolution does not change the decode quality.

**Our output.** The supersampled mosaic is produced with a clip-aware filter
(`docs/RESAMPLING.md` §4), so the file itself carries no halos; Resolve's own decode setting
still applies on playback.
