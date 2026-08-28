# Short-GOP replay status

The active replay codec path supports short-GOP H.264 on the disk-backed Session/Event engine. There is no capture-time RAM replay store or loose-MP4 playback path.

## Implemented

- Session video is stored in indexed segment media with real keyframe flags.
- Non-sequential seek, reverse and jog locate the preceding keyframe and decode forward to the requested frame.
- The encoder exposes All-I, 0.25 s, 0.50 s and 1.00 s GOP presets.
- The default is 0.50 s (`Balanced`): approximately 30 frames at 60 fps or 25 frames at 50 fps.
- B-frames remain disabled so DTS/PTS order stays predictable for replay seeking and export.
- Closed GOP is requested through `AV_CODEC_FLAG_CLOSED_GOP`.
- The selected GOP preset is applied to NVENC, AMF, QSV and libx264 creation/fallback paths.
- MP4 Event export preserves real keyframe flags instead of marking every packet as independently decodable.

## Decoded frame cache

The persistent disk player uses a bounded LRU decoded-frame cache keyed by `(segment sequence, packet position)`. Cached pictures therefore remain addressable across segment switches and active `.part` to finalized-file transitions.

The default cache budget is **192 MiB per replay player**. The budget is byte-bounded, so 4K naturally retains fewer decoded frames rather than scaling memory use with replay duration. Decoded pictures use FFmpeg reference-counted `AVFrame` clones.

## Runtime validation

CI verifies compilation, packaging and formatting, but real hardware still has to prove restart-safe keyframe behavior for NVENC, AMF, QSV and libx264 fallback. For each backend, test at least 1080p50 and 1080p60 with `Balanced (0.50 s)` and verify:

1. observed keyframe interval is approximately 25 frames at 50 fps / 30 frames at 60 fps;
2. forward replay remains frame-correct;
3. 25% and 50% slow motion remain frame-correct;
4. reverse playback has no corruption across GOP boundaries;
5. repeated reverse/jog over a GOP benefits from cache hits;
6. switching reverse back to forward does not show a stale frame or force unnecessary decoder rewind;
7. random seek/jog lands on the requested frame after rebuilding from the preceding keyframe;
8. Session/Event replay seeks correctly across indexed segment media;
9. replay reads finalized segments and the active `.part` segment while recording continues;
10. reverse/jog remains timestamp- and frame-order-correct across segment changes.

## Current integration

Short-GOP playback is part of the same Session timeline used by Events, recording runs, camera/PROGRAM video and replay audio. It is not a compatibility layer over an older replay implementation.
