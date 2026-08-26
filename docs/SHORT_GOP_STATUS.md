# Short-GOP replay checkpoint

The replay codec path is no longer restricted to All-I H.264.

## Implemented

- RAM replay snapshots trim their leading packets to the first retained real keyframe.
- RAM playback is keyframe-aware: sequential forward playback retains decoder state, skipped forward packets are decoded to preserve references, and reverse/random seeks flush and rebuild from the preceding keyframe.
- Disk playback uses the same correctness model: locate segment, find the preceding keyframe, then decode forward to the requested timestamp.
- MP4 replay export preserves the encoder's real keyframe flags instead of marking every packet as a keyframe.
- The encoder exposes four GOP presets: All-I, 0.25 s, 0.50 s, and 1.00 s.
- The default is 0.50 s (`Balanced`). At 60 fps this resolves to 30 frames; at 50 fps it resolves to 25 frames.
- B-frames remain disabled so DTS/PTS order stays simple for replay seeking and export.
- Closed GOP is requested through `AV_CODEC_FLAG_CLOSED_GOP`.
- The selected preset is applied consistently to NVENC, AMF, QSV, and libx264 creation/fallback paths.
- The encoder log reports the actual integer GOP length and its duration in milliseconds.

## Decoded frame cache

A bounded decoded-frame cache is now implemented for both the current RAM replay playout and the persistent continuous-disk player.

The cache is an LRU keyed by replay frame index for RAM snapshots. The disk player keys cached pictures by `(segment sequence, packet position)`, so cached frames remain addressable across segment switches and active `.part` to finalized-file transitions.

Important state separation was added to RAM playout:

- `cur_idx` describes the newest packet represented in decoder reference state;
- `display_idx` describes the picture most recently sent to OBS.

This distinction is required for short-GOP reverse playback. A backward cache hit must not rewind the decoder's newer reference chain, otherwise a subsequent direction change back to forward playback would throw away useful codec state.

The default cache budget is **192 MiB per replay player**. At 1080p NV12/YUV420 this is enough for approximately one full 60-frame GOP plus a small margin. The budget is byte-bounded rather than frame-count-bounded, so 4K naturally retains a smaller window instead of multiplying memory use by resolution.

Decoded pictures are cached using FFmpeg reference-counted `AVFrame` clones. Replay duration therefore does not determine decoded-cache memory consumption.

## CI status

The frame-cache checkpoint passes the repository's full CI matrix:

- clang-format: pass;
- gersemi: pass;
- Windows build/package: pass;
- Ubuntu build/package: pass;
- macOS build/package: pass.

The latest cache-enabled continuous-disk player source checkpoint is commit `1adddd0a98842e1d90018bbd246e1bb5a13700fa`.

## Still to validate on real hardware

CI verifies compilation and packaging, but it cannot prove that each hardware backend emits restart-safe IDR/key packets exactly at the requested interval. Runtime validation is still required for NVENC, AMF, QSV, and the libx264 fallback.

For each backend, test at least 1080p50 and 1080p60 with `Balanced (0.50 s)` and verify:

1. observed keyframe interval is approximately 25 frames at 50 fps / 30 frames at 60 fps;
2. forward replay remains frame-correct;
3. 25% and 50% slow motion remain frame-correct;
4. reverse playback has no corruption after GOP boundaries;
5. repeated reverse/jog over a GOP is visibly cheaper after the first decode because subsequent frames hit the cache;
6. switching direction from reverse back to forward does not show a stale frame or force unnecessary decoder rewind;
7. random seek/jog lands on the requested frame after rebuilding from the previous keyframe;
8. saved MP4 replay opens cleanly and seeks normally;
9. continuous disk replay can read both finalized segments and the active `.part` segment while recording continues;
10. disk-player reverse/jog reuses cached pictures across segment changes without timestamp or frame-order errors.

## Next checkpoint

The short-GOP correctness path and bounded decoded-cache foundation are now in place. The next architectural milestone is the **SQLite Event DB + metadata-only replay events**: shared session timeline events (`IN`, `OUT`, `-5`, `-10`, `-20`), 20 lists, played/protected state, and angle availability without copying media files.
