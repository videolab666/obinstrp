# Continuous Disk Replay — smoke test

This document applies to the early disk-engine implementation on `feature/disk-replay-core`.

The existing RAM replay path remains the production path. Continuous disk recording is intentionally opt-in until the disk reader/player replaces it.

## Preconditions

- Build the plugin for Windows x64.
- Start OBS Studio with a normal 1080p50/60 project.
- Add a `Sports Replay Capture` filter to one camera source.
- Verify the ordinary RAM replay still works before enabling the disk option.

## Enable continuous recording

Open the capture-filter properties and enable:

```text
Continuous replay recording to disk
```

The current session root defaults to:

```text
<Videos>/Sports Replay/Sessions/
```

If the legacy replay folder was changed before the first continuous session is created, the default session root follows that folder and appends `/Sessions`.

## Expected session layout

After the encoder emits its first packet, the plugin lazily creates a session directory similar to:

```text
Sports Replay/
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

## OBS log expectations

At startup of the disk writer:

```text
Sports Replay: continuous replay session ...
Sports Replay: continuous recorder started for ...
```

Every ~60 seconds the capture filter reports RAM + disk statistics, including:

- disk packet count;
- bytes written;
- finalized segment count;
- queue depth / peak;
- dropped packet count;
- write-error state.

For a healthy local SSD/NVMe test the queue should normally remain close to zero and packet drops should remain zero.

## Smoke-test sequence

1. Run one camera for 10 minutes.
2. Confirm new `.srseg/.sridx` pairs finalize continuously.
3. Trigger several ordinary RAM instant replays while continuous recording remains enabled.
4. Confirm current replay playback behavior is unchanged.
5. Disable `Continuous replay recording to disk` and confirm the active queue drains and the final segment is finalized.
6. Re-enable the option and confirm sequence numbers continue instead of overwriting old files.
7. Change encoder quality and confirm the old writer closes and a new writer starts without affecting the RAM replay path.
8. Stop OBS normally and verify no actively written segment remains open.

## Multi-camera smoke test

Repeat with 2–4 camera sources. Each camera source name is hashed into a separate `cam-xxxxxxxx` directory, so normal OBS source names do not become unsafe file-system paths.

Expected properties:

- all cameras share one session directory;
- every camera writes independently;
- stopping one capture writer does not stop the others;
- the source's OBS nanosecond timestamp is stored in every packet/index record.

## Known limitations at this checkpoint

- Continuous disk recording is video-only. Long-session audio comes later.
- Disk replay is not yet wired into the OBS playback source.
- No automatic free-space garbage collector yet.
- No SQLite event database yet.
- No crash-recovery scanner yet, although `.part` and packet framing are designed for it.
- No short-GOP mode yet; legacy playback still requires All-I.
- GitHub Actions should be enabled for the fork so format/build CI can validate each synchronization of the draft PR.
