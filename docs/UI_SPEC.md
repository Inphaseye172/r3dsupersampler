# UI specification

Implementation: `src/gui/`. Qt 6 Widgets, single `QMainWindow`.

## Main window

- **Toolbar**: *New batch*, *Settings…*, *Resolve note*.
- **Tab bar** (`QTabWidget`, closable, movable): one tab per batch. The title is the first
  clip's file name plus "+N"; suffix "(queued)" / "(running)". Closing a running or paused tab is
  refused with a message; closing the last tab creates a fresh one.
- **Log dock** (bottom): the engine log (level from Settings), 5000 lines.
- **Status bar**: left = live text ("<batch>: frame 120/480 at 23.4 fps" or "Idle"); right =
  "Queued batches: N" and "GPU: CUDA" or "CPU fallback".

## Batch tab

Left: table (`BatchModel`, `QTableView`, multi-row selection, progress-bar delegate).

| Column | Content |
|---|---|
| File | file / directory name (tooltip: full path + message) |
| Format | source label detected from content: `REDCODE RAW (KOMODO 6K)`, `R3D NE (NIKON ZR)`, `N-RAW NEV (…)`, `CinemaDNG` |
| Resolution | WxH from the probe |
| FPS | frame rate |
| Frames | frame count |
| Output | output file / folder name once known |
| Status | Queued, Probing, Running (x fps), Encoding, Verifying, Done, Failed, Skipped, Copied, Cancelled |
| Progress | 0–100 % bar |
| Message | plain sentence: result summary, skip reason or error |

Buttons under the table: *Add files…* (R3D, NEV, DNG; a .dng selects its folder), *Add folder…*
(scans with the folder-mode options), *Remove*, *Start* / *Run again*, *Pause* / *Resume*,
*Cancel*. Add/Remove and all options are disabled while the batch is queued, running or paused.

Right: options.

| Group | Control | Values |
|---|---|---|
| Output | Size | 6K (6048 × 3402), UHD (3840 × 2160), Full HD (1920 × 1080), Same resolution (VBR only) |
| | Container | CinemaDNG (default); Keep source container (CinemaDNG sources only; REDCODE cannot be written). ProRes 4444 / XQ and DNxHR 444 / HQX arrive in milestone 4 |
| | Quality | Q1 Maximum quality, Q3 High, Q5 Normal (default), Q7 Proxy |
| | Filter | Lanczos-3 (sharp, clip-aware), Area average (soft, lowest noise) |
| | Output folder | path + Browse… (defaults to `<source>/supersampled` or `<folder>_supersampled`) |
| | Verify every output | checkbox, default on |
| | Use CUDA GPU when available | checkbox, default from Settings |
| | Overwrite existing outputs | checkbox, default off (existing outputs are skipped) |
| Folder mode | Search sub-folders | default on |
| | Preserve folder structure in the output | default on |
| | Archive mode: copy non-raw files unchanged and verify by hash | default off |

Summary label under the options: "N clips, M non-raw files to copy", "Queued: starts when the
running batch finishes.", "Running…", "Paused after the frames in flight.", "Cancelling…",
"Copied a of b files (c failed)", "Finished: d done, f failed, s skipped, c cancelled, n copied
in t s" plus the manifest path in archive mode.

## Batch states and the queue

`Idle → Queued → Running ⇄ Paused → Finished`, and `Cancel` from Queued (back to Idle) or
Running/Paused (→ Finished with cancelled rows). One batch runs at a time; *Start* on another
tab queues it, and queued batches start in order when the running one finishes.

## Per-file states

Queued → Probing → Running (x %) → Encoding (audio) → Verifying → Done | Failed (reason) |
Skipped (reason) | Cancelled. Archive copies report in the summary label.

## Settings dialog

- **Engine**: hardware line (cores, RAM, resampler), *Use CUDA GPU when available*, *Parallel
  clips* (0 = automatic: cores/8 clamped to 1..4), *Thread limit* (0 = all cores), *Output name
  template* (`{stem} {size} {profile} {format} {kernel}`), *Log level*.
- **Codec modules**: table Module | Read | Write | Status with the exact text each factory
  reports (e.g. why NEV cannot be decoded yet).
- **About**: version, what this build ships, the Resolve note.

Settings persist via `QSettings` (organisation "Anchorpoint", application "R3DSupersampler").

## Messages (verbatim)

- "REDCODE cannot be written (no public encoder exists); choose CinemaDNG, ProRes or DNxHR output"
- "cannot decode REDCODE frames: RED R3D SDK not built in: …" (Failed; install the SDK runtime)
- "output already exists: <path>" (Skipped; enable Overwrite to replace)
- "output WxH would be larger than source WxH" (Skipped; custom sizes only: a preset larger than the source steps down to the largest preset that fits, or the source size; upscaling is never done)
- "verification failed: <problem>; …" (Failed; the output is kept for inspection)
- "These files are not recognised raw clips: …" (dialog after Add files)

## Threading

All engine work runs on an `EngineBridge` worker thread; callbacks arrive as queued signals and
update the model at most every 100 ms per clip. Pause and cancel are thread-safe calls into the
`Scheduler`.
