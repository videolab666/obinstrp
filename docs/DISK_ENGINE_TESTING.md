# Continuous Disk Replay — smoke test

This document applies to the early disk-engine implementation on `feature/disk-replay-core`.

The existing RAM replay path remains the production path. Continuous disk recording is intentionally opt-in until the disk reader/player replaces it.

## Preconditions

- Build the plugin for Windows x64.
- Start OBS Studio with a normal 1080p50/60 project.
- Add a `Pitel Instant Replay Capture` filter to one camera source.
- Verify the ordinary RAM replay still works before enabling the disk option.

## Enable continuous recording

Open the capture-filter properties and enable:

```text
Continuous replay recording to disk
```

The current session root defaults to the plugin configuration namespace:

```text
<OBS module config>/standalone-v1/Sessions/
```

The session root is configured only through the current Pitel Instant Replay Session settings. Pre-release replay-folder settings are not migrated or consulted.

The config schema already carries a minimum-free-space reserve (currently 100 GiB by default). The early writer does **not** delete media yet: if the free space falls below the reserve, it stops opening new segments and periodically waits for space to become available. Automatic safe GC to the configured purge target is a later milestone.

## Expected session layout

After the encoder emits its first packet, the plugin lazily creates a session directory similar to:

```text
Pitel Instant Replay/
  Sessions/
    20260823-170000-a1b2c3d4/
      session.json
      cam-1234abcd/
        00000001.srseg.part
        00000001.sridx.part
```

The active `.part` pair is flushed periodically. Roughly every configured segment interval (default 4 seconds), on the next keyframe, it is finalized to:

```text
00000001.srseg
00000001.sridx
```

and a new `.part` pair begins.

The current encoder is still All-I (`GOP=1`), so every frame should be marked as a keyframe. Short-GOP recording must not be enabled until the keyframe-aware disk playback path is integrated.

## Inspect a segment without OBS

The repository includes a stdlib-only integrity checker:

```powershell
python tools/srseg_inspect.py "D:\OBS-Replay\...\cam-1234abcd\00000001.srseg"
```

For an active pair:

```powershell
python tools/srseg_inspect.py "D:\OBS-Replay\...\cam-1234abcd\00000002.srseg.part"
```

The matching `.sridx` / `.sridx.part` is inferred automatically. Use `-v` to print each indexed video packet.

The checker validates:

- format/version magic;
- segment/index identity;
- every indexed file offset;
- packet framing and payload bounds;
- timestamp equality between packet and index;
- keyframe flag equality;
- monotonic timestamps;
- discontinuity flags.

A normal completed segment should end with:

```text
validation:    OK
```

## OBS log expectations

At startup of the disk writer:

```text
Pitel Instant Replay: continuous replay session ...
Pitel Instant Replay: continuous recorder started for ...
```

Every ~60 seconds the capture filter reports RAM + disk statistics, including:

- disk packet count;
- bytes written;
- finalized segment count;
- queue depth / peak;
- dropped packet count;
- disk-reserve state;
- write-error state.

For a healthy local SSD/NVMe test the queue should normally remain close to zero and packet drops should remain zero.

## Smoke-test sequence

1. Run one camera for 10 minutes.
2. Confirm new `.srseg/.sridx` pairs finalize continuously.
3. Run `tools/srseg_inspect.py` on several finalized pairs and require `validation: OK`.
4. Trigger several ordinary RAM instant replays while continuous recording remains enabled.
5. Confirm current replay playback behavior is unchanged.
6. Disable `Continuous replay recording to disk` and confirm the active queue drains and the final segment is finalized.
7. Re-enable the option and confirm sequence numbers continue instead of overwriting old files.
8. Change encoder quality and confirm the old writer closes and a new writer starts without affecting the RAM replay path.
9. Stop OBS normally and verify no actively written segment remains open.

## Multi-camera smoke test

Repeat with 2–4 camera sources. Each camera source name is hashed into a separate `cam-xxxxxxxx` directory, so normal OBS source names do not become unsafe file-system paths.

Expected properties:

- all cameras share one session directory;
- every camera writes independently;
- stopping one capture writer does not stop the others;
- the source's OBS nanosecond timestamp is stored in every packet/index record;
- the segment catalog can resolve a timestamp to the correct camera segment;
- the keyframe-aware disk decode helper can locate the preceding keyframe and decode forward to a requested timestamp.

## Current status

This document originated as the first disk-engine validation checklist. The current plugin has moved beyond that checkpoint: Session/Event replay, recording runs, master/camera audio, automatic storage cleanup, crash recovery, short-GOP playback and the operator Session UI are all part of the active architecture. Validation should target the current Session/Event workflow rather than any pre-release playback-source or loose-MP4 path.
