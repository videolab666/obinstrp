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

## Still to validate on real hardware

CI verifies compilation and packaging, but it cannot prove that each hardware backend emits restart-safe IDR/key packets exactly at the requested interval. Runtime validation is still required for NVENC, AMF, QSV, and the libx264 fallback.

For each backend, test at least 1080p50 and 1080p60 with `Balanced (0.50 s)` and verify:

1. observed keyframe interval is approximately 25 frames at 50 fps / 30 frames at 60 fps;
2. forward replay remains frame-correct;
3. 25% and 50% slow motion remain frame-correct;
4. reverse playback has no corruption after GOP boundaries;
5. random seek/jog lands on the requested frame after rebuilding from the previous keyframe;
6. saved MP4 replay opens cleanly and seeks normally;
7. continuous disk replay can read both finalized segments and the active `.part` segment while recording continues.

## Next performance checkpoint

Correctness is implemented first. Reverse/random playback may decode up to one GOP from its preceding keyframe for each non-sequential request. The next optimization is a small decoded-frame/GOP cache around the playhead so reverse and jog do not repeatedly decode the same reference chain.
