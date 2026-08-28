# Pitel Instant Replay for OBS Studio

Pitel Instant Replay is a multi-camera instant-replay system for OBS Studio built
around continuous disk recording, persistent Sessions and frame-accurate Events.
The current architecture is intended for sports production workflows where an
operator needs ISO camera replay, PROGRAM replay, slow motion, alternate angles,
highlight lists and recoverable recordings without keeping a long raw-video
replay ring in RAM.

## Current architecture

Each configured camera can be recorded continuously into short indexed segments.
PROGRAM can be recorded as an additional angle. Session metadata, Events,
Recording Runs and angle identity survive OBS restarts.

```text
OBS camera / PROGRAM
        |
        v
hardware encoder where available
        |
        v
indexed Session segments on disk
        |
        +--> Event / highlight database
        +--> Replay A / Replay B
        +--> Multiview
        +--> Event export / ISO Run export
```

There is no capture-time replay RAM ring and no loose-MP4 replay bin in the
current runtime. Small bounded decoder/frame caches are still used where they
improve seek, reverse and preview performance.

## Main features

- Continuous multi-camera Session recording.
- Separate PROGRAM recording angle.
- NVIDIA/AMD GPU recording path on Windows where supported.
- Intel QSV fallback path for PROGRAM and isolated Intel D3D11VA replay decode.
- Short closed H.264 GOPs with no B-frames for responsive replay seeking.
- Persistent Session Manager with New, Open, Resume, Rename and export actions.
- Recording Runs: every START/STOP interval is tracked independently.
- Event IN/OUT and quick `-5`, `-10`, `-20` marking.
- Replay A/B buses, TAKE and RETURN LIVE workflow.
- Per-Event angle selection and camera availability checks.
- Highlight/Event Lists.
- Session-aware Multiview and timeline.
- Master, selected-camera or muted replay audio modes.
- Storage reserve, cleanup and crash recovery.
- Batch Event export and per-Run ISO export without H.264 re-encoding.

## Basic operator workflow

1. Add **Pitel Instant Replay Capture** to every camera that should be recorded.
2. Open **Docks → Pitel Instant Replay**.
3. Create or select the Recording Target Session.
4. Press **START RECORD** and verify that REC/packet counters advance.
5. Mark an Event using **IN / OUT** or one of the quick-mark buttons.
6. Cue or play the Event on Replay A/B and choose the required angle.
7. Use **TAKE** to put replay on Program and **RETURN LIVE** when finished.
8. Use Session Manager to reopen old Sessions or export Events/ISO Runs.

Recording does not automatically restart when OBS starts. Reopening an archived
Session is separate from choosing a Recording Target.

## Session model

Pitel Replay distinguishes three states:

- **OPEN** — Session currently being browsed/edited.
- **REC TARGET** — Session that the next START RECORD will use.
- **REC** — Session actively receiving media.

This allows an operator to browse yesterday's Session while today's match keeps
recording, without mixing the two timelines.

A new Session uses a zero-based timeline. Resuming a closed Session starts a new
Recording Run immediately after the last committed media timestamp and records
a discontinuity boundary. Camera, PROGRAM, master audio, camera audio and live
Event timestamps use the same Session time mapping.

## Replay media paths

On Windows, NVIDIA/AMD capture can remain GPU-resident through the hardware
encode path. Intel replay decode deliberately uses an isolated FFmpeg D3D11VA
device and transfers the decoded frame before OBS rendering; this avoids sharing
OBS's D3D11 immediate/video context on Intel drivers.

The project also has CPU-frame fallbacks for hardware/platform compatibility.
CPU video paths are normalized to BT.709 limited range.

## Building

The OBS bridge uses the standard OBS plugin CMake infrastructure and currently
builds for Windows, Ubuntu and macOS in CI.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Exact presets and packaging commands are defined by the repository CMake and
GitHub Actions files.

## Pre-release compatibility policy

Pitel Instant Replay is still pre-release. The current Session/Event architecture
is a deliberate breaking boundary: obsolete replay-source IDs, loose-MP4 replay
bins, old replay-folder configuration and older scene-collection layouts are not
migrated or supported. Use Replay Setup to create fresh Replay A/B scenes and
attach current Pitel capture filters.

This policy keeps the runtime focused on one disk-backed replay architecture and
avoids carrying migration code for builds that were never released as a stable
public interface.

## License and provenance

The OBS plugin/bridge is distributed under GPL-2.0-or-later. See `LICENSE`.
Historical project provenance and third-party licensing notes are preserved in
`THIRD_PARTY_NOTICES.md` and `docs/UPSTREAM_CLEANUP.md`.

A future out-of-process replay engine, if developed separately, should be treated
as a separate component with its own provenance and licensing analysis rather
than assuming that moving code out of the OBS plugin changes existing license
obligations.
