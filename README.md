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

There is no legacy capture-time replay RAM ring and no loose-MP4 replay bin in
the current runtime. Small bounded decoder/frame caches are still used where
they improve seek, reverse and preview performance.

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

The project also has CPU-frame fallbacks for compatibility. CPU video paths are
normalized to BT.709 limited range.

## Building

The OBS bridge uses the standard OBS plugin CMake infrastructure and currently
builds for Windows, Ubuntu and macOS in CI.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Exact presets and packaging commands are defined by the repository CMake and
GitHub Actions files.

## Development branches

The current persistent Session implementation is developed from
`feature/session-manager`.

`cleanup/upstream-removal` removes the obsolete runtime inherited from the
original sports-replay fork and is intentionally kept separate until CI and
Windows field validation pass. See `docs/UPSTREAM_CLEANUP.md` for the provenance
ledger and validation requirements.

## License

The OBS plugin/bridge is distributed under GPL-2.0-or-later. See `LICENSE`.

A future standalone replay engine is planned as a separate process and separate
repository; code for that engine must not be copied from the GPL OBS bridge.
