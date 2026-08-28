# Upstream cleanup ledger

This document records the removal of runtime implementation inherited from
`Voodoo25/obs-sports-replay` / Systec before Pitel Replay is split into a thin
OBS bridge and a separate replay engine.

Baseline for this cleanup: `feature/session-manager@ee3c4904c5836de4199b4e1021b1558aa6ea8c4f`.
Cleanup branch: `cleanup/upstream-removal`.

The OBS plugin remains GPL-2.0-or-later because it links to libobs. This cleanup
does **not** attempt to change the OBS bridge license. Its purpose is provenance:
remove the old replay implementation so future engine code does not depend on
Systec runtime source.

## Original upstream runtime map

| Upstream path | Cleanup action |
| --- | --- |
| `src/capture-filter.c` | deleted; replaced by new disk-only `src/pir-capture-filter.c` |
| `src/playback-source.c` | deleted; Session/Event Output is the only replay runtime |
| `src/plugin-main.c` | deleted; replaced by new `src/pir-plugin-main.c` |
| `src/sr-buffer.c/.h` | deleted; no continuous replay RAM ring remains |
| `src/sr-capture.h` | replaced with a new disk/session bridge interface |
| `src/sr-clip.c/.h` | deleted; legacy private clip player removed |
| `src/sr-codec.c/.h` | replaced by new FFmpeg adapter implementation; Intel isolated D3D11VA behavior retained as a requirement |
| `src/sr-config.c/.h` | replaced by new Session-oriented configuration implementation |
| `src/sr-credit.h` | deleted |
| `src/sr-dock.cpp/.h` | old dock deleted; replaced by new Operator/Session shell and interface |
| `src/sr-load.c/.h` | deleted; loose-MP4 replay loader removed |
| `src/sr-save.c/.h` | deleted; RAM snapshot saver removed |
| `src/sr-scene-tracker.c/.h` | replaced by new OBS scene/transition bridge implementation |
| `src/sr-thumb.c/.h` | replaced by new thumbnail service; zero Session timestamp is valid |
| `src/plugin-support.c.in/.h` | replaced with project-specific minimal build support |

## Architectural result

Capture is now disk/session-only:

```text
OBS source
  -> GPU/CPU encoder
  -> sr_segment_writer
  -> Session storage
```

There is no parallel:

```text
OBS source -> sr_buffer -> snapshot -> loose MP4 -> legacy playback source
```

Small bounded decode/frame caches remain. They are playback performance caches,
not replay capture storage.

## Critical behaviors that cleanup must preserve

- NVIDIA/AMD D3D11 zero-copy encode path.
- NVIDIA/AMD replay hardware decode/native rendering path.
- Intel replay decode on an isolated FFmpeg D3D11VA device, followed by
  hardware-to-system transfer before the OBS renderer. It must not share the
  OBS D3D11 immediate/video context.
- Intel PROGRAM QSV recording path.
- BT.709 limited-range normalization for CPU recording paths.
- Current SDR/sRGB replay renderer behavior.
- Session zero-based timeline, Recording Runs and discontinuity semantics.
- Master audio, selected-camera audio and PROGRAM synchronization.
- Session crash recovery and generation guards.
- Studio Mode preview protection and temporary transition restoration.

## Repository material outside runtime

OBS plugin build scaffolding remains based on the standard OBS plugin template
where appropriate. It is infrastructure, not Systec replay implementation.
Before a standalone proprietary engine repository is created, that repository
must start from a new project skeleton and must not copy this GPL OBS build
scaffolding.

Historical screenshots, README text and locale entries inherited from the old
project are audited separately from runtime code. They are not dependencies of
the replay engine and should not be used as provenance for future proprietary
assets.

## Release gate for this branch

Do not merge this cleanup until:

- Windows, Ubuntu and macOS builds pass;
- no legacy RAM/file replay source is compiled or referenced;
- Session/Event replay works after OBS restart;
- START/STOP/Resume recording works;
- NVIDIA and Intel runtime tests pass on Windows;
- Program recording and audio remain synchronized;
- Multiview and Event Output retain the validated color pipeline.
