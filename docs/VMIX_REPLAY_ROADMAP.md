# OBS Pitel Instant Replay — vMix-style Instant Replay roadmap

**Repository:** `videolab666/obinstrp`  
**Upstream:** `Voodoo25/obs-sports-replay`  
**Target:** Windows x64 first, OBS Studio 31/32+  
**Primary use case:** 4×1080p50/60 sports replay with long-running ISO recording, fast multi-angle events, slow motion, A/B replay outputs, audio, stingers and safe disk retention.

## 1. Goal

Evolve the existing plugin rather than rewrite it. The current code already provides useful building blocks: per-camera capture filters, H.264 encoding through NVENC/AMF/QSV/x264, OBS nanosecond timestamps, replay playback, slow/reverse controls, audio capture, MP4 remux/load, scene tracking, thumbnails and a native Qt dock.

The target architecture is:

```text
Camera 1 ─► encoder ─► Disk Segment Store ─┐
Camera 2 ─► encoder ─► Disk Segment Store ─┤
Camera 3 ─► encoder ─► Disk Segment Store ─┤
Camera 4 ─► encoder ─► Disk Segment Store ─┘
                                          │
                               Shared Session Timeline
                                          │
                               Timestamp/Keyframe Index
                                          │
                                     Event Database
                                          │
                         ┌────────────────┴────────────────┐
                         │                                 │
                  Replay Channel A                  Replay Channel B
                         │                                 │
                         └────────────────┬────────────────┘
                                          │
                                    Replay Output
                                          │
                              Native OBS Stinger IN/OUT
                                          │
                                       Program
```

A replay event is primarily metadata (`IN`, `OUT`, available angles, preferred angle, speed, audio mode, played/protected flags), not a copied media file.

## 2. Important implementation constraint discovered in the current code

The current playback engine assumes **every stored H.264 packet is independently decodable**. `sr-codec.c` therefore uses `GOP=1`, and `playback-source.c` flushes the decoder on a non-sequential jump and decodes only the selected packet.

Therefore we must **not simply change the current RAM encoder to GOP 15/25/30 while keeping the old playback path**. Doing so would break random seek, reverse and frame selection.

Safe migration order:

1. Keep legacy All-I replay working.
2. Add continuous disk recording and segment/index infrastructure first.
3. Add disk reader that can seek to the previous IDR and decode forward to the exact requested frame.
4. Only then make short-GOP the default for disk replay.
5. Keep All-I as an optional maximum-scrub-performance mode.

This is preferable to running two simultaneous encoders per camera during migration.

## 3. Target codec policy

MVP codec: **H.264**.

Target replay-optimized defaults after the new disk player is active:

```text
GOP: 0.5 s
B-frames: 0
Closed GOP
CQP/ICQ quality control
Segment: about 4 s, rotated on an IDR
```

Presets:

```text
Ultra Replay: 0.25 s GOP
Balanced:     0.50 s GOP
Economy:      1.00 s GOP
All-I:        1 frame GOP
```

At 60 fps: 0.5 s ≈ GOP 30; 0.25 s ≈ GOP 15.  
At 50 fps: 0.5 s ≈ GOP 25; 0.25 s ≈ GOP 12/13.

HEVC/AV1 are later milestones and must not delay the first stable H.264 implementation.

## 4. Shared session timeline

Use OBS timestamps as the canonical time source:

```cpp
using SrTime = uint64_t; // nanoseconds
```

For each camera:

```text
OBS frame timestamp + manual camera sync offset = corrected session timestamp
```

Example:

```text
CAM1   +0 ms
CAM2  -17 ms
CAM3  +33 ms
CAM4   +0 ms
```

All events are stored against this common timeline. Camera availability for an event is determined by whether that camera has media coverage for the event interval.

## 5. Session layout

Example:

```text
D:\OBS-Replay\2026-08-23_Padel_Final\
  session.json
  session.sqlite

  cam01\
    00000001.srseg
    00000001.sridx
    00000002.srseg
    00000002.sridx

  cam02\
    ...

  audio\
    master\
      ...

  cache\
    thumbs\
      ...
```

`session.json` stores format/version/session metadata. SQLite stores cameras, segment ranges, events, lists, settings and export state. Encoded packets stay in segment files.

## 6. Internal segment format

Do not use one giant MP4 as the live replay store. Use a simple versioned crash-recoverable container, e.g. `.srseg`, plus `.sridx`.

Reasons:

- concurrent write/read;
- fast random access;
- safe rolling deletion;
- easy crash recovery;
- no dependency on MP4 finalization;
- per-segment lifecycle and health state.

Example header:

```cpp
struct SrSegmentHeader {
    char magic[8];
    uint32_t version;
    uint32_t camera_id;
    uint32_t codec_id;
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint64_t segment_start_ns;
    uint32_t extradata_size;
    uint32_t flags;
};
```

Packet framing:

```cpp
struct SrPacketHeader {
    uint32_t magic;
    uint32_t payload_size;
    uint8_t type;
    uint8_t flags;
    uint16_t reserved;
    uint64_t timestamp_ns;
    int64_t pts;
    int64_t dts;
    int64_t duration;
};
```

Flags include at least `KEYFRAME` and `DISCONTINUITY`.

## 7. Index format

Each segment has a compact binary index:

```cpp
struct SrIndexEntry {
    uint64_t timestamp_ns;
    uint64_t file_offset;
    uint32_t packet_size;
    uint32_t frame_number;
    uint8_t keyframe;
};
```

Seek algorithm:

```text
target timestamp
  ↓
find segment by time range
  ↓
binary-search .sridx
  ↓
find nearest previous keyframe
  ↓
decode forward
  ↓
suppress preroll frames
  ↓
show exact target frame
```

## 8. Crash-safe writing

Active files:

```text
00000125.srseg.part
00000125.sridx.part
```

After clean close:

```text
00000125.srseg
00000125.sridx
```

Recovery scans complete packet records, truncates an incomplete tail, rebuilds the index, reconciles the database and finalizes recoverable segments.

Disk I/O must run outside the Qt UI thread and outside long blocking sections of the OBS video callback.

## 9. Writer threading

Initial pipeline:

```text
OBS frame
  ↓
existing encoder
  ↓
AVPacket
  ↓
bounded packet queue
  ↓
SegmentWriter worker
  ↓
.srseg + .sridx
```

The queue exposes metrics: depth, high-water mark, drops, bytes/s and disk latency. If a disk stalls, the plugin must report the problem and protect OBS responsiveness.

## 10. Event model

```cpp
struct ReplayEvent {
    uint64_t id;
    uint64_t in_ns;
    uint64_t out_ns;
    uint32_t preferred_angle;
    double speed_percent;
    int audio_mode;
    bool protected_event;
    bool played;
    std::string name;
    std::string tag;
};
```

Event creation is metadata-only.

Operator actions:

```text
IN
OUT
-5
-10
-20
```

Support configurable post-roll; a post-roll event remains `Pending` until the writer reaches the requested `OUT` timestamp.

## 11. 20 Event Lists

Match the vMix workflow conceptually:

```text
[1] [2] [3] ... [20]
```

An event may appear in multiple lists without duplicating media. Required operations:

- reorder;
- Move To;
- Copy To;
- Duplicate event metadata;
- Cut/Copy/Paste Before/Paste After;
- Protect/Unprotect;
- Played/Unplayed;
- Delete Event;
- Delete Event + safely reclaimable media.

## 12. A/B replay channels

Two independent logical channels:

```cpp
class ReplayChannel {
public:
    EventId event;
    CameraId angle;
    SrTime playhead;
    double speed;
    bool reverse;
    bool loop;
    bool playing;
    bool paused;
    AudioMode audio_mode;
};
```

The operator can cue B while A is on air, then take B without searching for the next replay.

Required controls:

```text
[A|B] [A] [B]
A: [P][1][2][3][4]...
B: [P][1][2][3][4]...
```

## 13. Replay decoding, jog and reverse

Forward seek starts at the previous keyframe. A decoded RAM cache around the playhead supplies frame-step and jog responsiveness.

Typical cache window:

```text
[-2 sec] [PLAYHEAD] [+3 sec]
```

Reverse with P-frames:

```text
I → P1 → P2 → ... → P29
       decode forward
            ↓
         cache GOP
            ↓
P29 ← P28 ← ... ← I
```

Prefetch the previous GOP while the current GOP is being displayed in reverse.

Required speed presets: 25%, 33%, 50%, 75%, 100%; core range remains about 10–400%.

## 14. Audio architecture

Current raw planar audio buffering is not appropriate for multi-hour disk sessions.

MVP disk audio:

```text
AAC-LC
48 kHz
Stereo
160–192 kbit/s
```

Modes:

```text
Off
Master Replay Audio
Camera Audio
```

Recommended multi-angle architecture:

```text
CAM1 ─ video
CAM2 ─ video
CAM3 ─ video
CAM4 ─ video
MASTER AUDIO ─────────────────
```

Changing replay angle should not cause a master audio discontinuity.

Live/replay mix policy:

```text
Keep live audio
Duck live audio
Mute live audio
Replay gain
Fade in/out
```

Advanced later: varispeed audio and optional pitch-preserving time stretch.

## 15. Native OBS stingers

Do not expand the current simple intro/outro clip implementation into a second transition engine. Use OBS's native `obs_stinger_transition` capabilities.

Replay IN and Replay OUT must have separate settings:

- file;
- transition point by frame/time;
- hardware decode;
- preload;
- track matte where supported;
- stinger audio;
- audio fade behavior.

First implementation strategy:

1. save the user's current OBS transition;
2. select dedicated Replay IN stinger;
3. switch Live → Replay Scene;
4. play replay;
5. select Replay OUT stinger;
6. switch Replay → previous/live scene;
7. restore the user's prior transition.

Studio Mode behavior must be explicitly tested.

## 16. Storage manager and disk reserve

Settings:

```text
Replay folder
Minimum free disk space
Purge-until free space
Segment duration
Retention policy
```

Use hysteresis, e.g. start purging below 100 GB and stop at 110 GB.

GC priority:

1. corrupt/orphan temporary files;
2. old media with no event coverage;
3. old media referenced only by unprotected events;
4. never automatically delete a segment overlapping protected events;
5. if nothing can be deleted, stop recording and show a critical warning while keeping existing replay playback usable.

## 17. Delete semantics

`Delete Event` deletes metadata/list membership only.

`Delete Event + Media` may reclaim only segments that are safe under retention/protection rules. A segment can overlap several events, therefore a simple file refcount is insufficient; use interval-overlap checks.

Manual actions:

```text
Delete Session
Delete Unprotected Media
Delete Media Before...
Delete Media After...
Open Folder
```

## 18. Operator Dock

Replace the current thumbnail bin as the primary interface with a vMix-like operator dock based on `QTableView + QAbstractTableModel`.

Layout concept:

```text
LIVE  REC ●  Session  Disk free  [Settings]

[1][2][3]...[20]

# | IN | OUT | Duration | Speed | Angle | Audio | Played | Protect

MARK [IN] [OUT] [-5] [-10] [-20]

[A|B] [A] [B]   A:[P][1][2][3][4]  B:[P][1][2][3][4]

[Play][Stop][Prev frame][Next frame][Loop]
[25][33][50][75][100]  Audio[Master]

-------------------- timeline/playhead --------------------
```

Routine live operation should not require opening OBS source property dialogs.

## 19. Controller architecture

Keep Qt out of the core engine:

```text
Qt Dock ───────────┐
Hotkeys ───────────┤
WebSocket later ───┤
                   ▼
             ReplayController
                   │
                   ▼
               Core engine
```

This also makes Stream Deck / remote integration straightforward.

## 20. SQLite

Core tables:

```text
sessions
cameras
segments
audio_tracks
events
event_lists
event_list_items
settings
exports
```

Use WAL mode, foreign keys and schema migrations.

## 21. Export

Fast export: remux encoded packets, no H.264 re-encode where possible.

Later frame-accurate smart render can re-encode only partial edge GOPs while copying middle GOPs.

Export options:

```text
Preferred angle
All angles
Fast / no re-encode
Frame accurate
```

Legacy saved MP4 files should remain loadable.

## 22. Diagnostics

Per-camera:

- frames received/encoded;
- encoder backend;
- actual bitrate;
- keyframe interval;
- packet queue depth/drops;
- timestamp discontinuities.

Global:

- disk throughput/latency;
- free space;
- segment count;
- seek/decode latency;
- cache hit ratio;
- audio underruns.

## 23. Failure policy

Camera disappears: close current segment, mark discontinuity, keep other cameras recording, start a new segment on recovery.

Encoder fails: isolate failure per camera; Auto may fall back; no whole-plugin crash.

Disk reserve exhausted: run GC; if impossible, stop new recording and warn prominently; existing replay remains available.

Metadata DB error: avoid destructive cleanup and preserve media for recovery.

## 24. MVP milestones

### M0 — Fork and baseline
- [x] Fork upstream.
- [x] Create development branch `feature/disk-replay-core`.
- [ ] Preserve upstream license/notices.
- [ ] Add upstream tracking document.
- [ ] Confirm clean Windows build.
- [ ] Record baseline behavior/performance.

### M1 — Session/config foundation
- [ ] Version config schema.
- [ ] Add replay storage path, reserve, purge target, segment duration.
- [ ] Add `SrSession` start/stop and session folder metadata.

### M2 — Disk segment writer while legacy replay remains All-I
- [ ] Define `.srseg`/`.sridx` v1.
- [ ] Worker-thread SegmentWriter.
- [ ] `.part` files and atomic finalize.
- [ ] Keyframe/timestamp index.
- [ ] Write continuous ISO media in parallel with current RAM replay.
- [ ] 2-hour one-camera test.

### M3 — Multi-camera continuous recording
- [ ] Stable camera IDs.
- [ ] Shared ns timeline.
- [ ] Camera sync offsets.
- [ ] Offline/discontinuity handling.
- [ ] 4×1080p50/60 stress test.

### M4 — Segment reader / keyframe-aware player
- [ ] Segment lookup by timestamp.
- [ ] Previous-IDR seek.
- [ ] Decode-forward exact frame.
- [ ] Cross-segment read.
- [ ] Decoded frame cache.

### M5 — Switch disk path to short GOP
- [ ] Expose 0.25/0.5/1.0 s GOP presets.
- [ ] Keep B=0.
- [ ] Validate actual keyframes per backend.
- [ ] Preserve All-I as optional mode.
- [ ] Verify reverse/jog/frame-step on short GOP.

### M6 — SQLite Event DB and 20 lists
- [ ] DB/schema/migrations.
- [ ] IN/OUT/-5/-10/-20.
- [ ] Event CRUD.
- [ ] 20 lists and Move/Copy/Duplicate/Reorder.
- [ ] Played/protected state.

### M7 — A/B replay channels
- [ ] Independent cue/event/angle/speed/playhead.
- [ ] Take A/B and A↔B.

### M8 — Audio v1
- [x] Master and optional camera audio to disk.
- [x] AAC timestamps/indexing.
- [x] Off/Master/Camera playback.
- [x] Replay gain and Keep/Duck/Mute live policy.

### M9 — Native stingers
- [ ] Replay IN.
- [ ] Replay OUT.
- [ ] frame/time transition point.
- [ ] HW decode/preload/audio.
- [ ] save/restore user's transition.
- [ ] Studio Mode tests.

### M10 — Storage GC/delete
- [x] Free-space monitor.
- [x] hysteresis.
- [x] protected interval checks.
- [x] safe automatic purge.
- [x] manual session/media delete and session/GC diagnostics.

### M11 — Operator Dock
- [ ] QTableView/QAbstractTableModel.
- [x] vMix-like editable event grid with recorded angle previews.
- [x] mark controls.
- [x] A/B controls with explicit Cue/Program state.
- [x] transport/jog/speed/audio.
- [x] storage/session diagnostics.

### M12 — Export/recovery/reliability
- [ ] Fast MP4 remux.
- [ ] Multi-angle export.
- [ ] `.part` recovery.
- [ ] index rebuild.
- [ ] DB/media reconciliation.
- [ ] forced-kill and disk-removal tests.

### M13 — Performance
- [ ] NV12 fast path.
- [ ] hardware replay decode.
- [ ] async thumbnail generation.
- [ ] mmap/index/cache tuning.
- [ ] GPU zero-copy research.

### M14 — Advanced features
- [ ] preserve-pitch slow audio.
- [ ] obs-websocket vendor API.
- [ ] direct compressed SRT/RTSP ingest research.
- [ ] optional second physical replay output.

## 25. MVP Definition of Done

- [ ] 4 cameras continuous to disk for a 3-hour sports session.
- [ ] H.264 hardware encoding.
- [ ] Short GOP default with B=0 after keyframe-aware player exists.
- [ ] Common session timeline and camera sync offsets.
- [ ] IN/OUT/-5/-10/-20 events.
- [ ] Same event selectable from all recorded angles.
- [ ] 20 event lists.
- [ ] 25/33/50/75/100%, reverse and frame step.
- [ ] A/B replay channels.
- [ ] Master/camera replay audio on/off.
- [ ] Native OBS Stinger IN/OUT.
- [ ] Played/protected flags.
- [ ] Delete event/session/media.
- [ ] Configurable minimum free disk space and automatic GC.
- [ ] MP4 export.
- [ ] Crash recovery.
- [ ] Single operator dock for normal live operation.

## 26. Recommended PR sequence

1. Docs/upstream tracking/config skeleton.
2. Session manager + disk segment format/writer while legacy All-I stays intact.
3. Multi-camera continuous disk record.
4. Segment reader and keyframe-aware playback.
5. Short-GOP encoder presets/default.
6. Event DB and 20 lists.
7. New operator dock.
8. A/B channels.
9. Audio.
10. Native stingers.
11. Storage GC/delete.
12. Export/recovery/performance.

Each PR should leave the plugin buildable and testable. Avoid a single large rewrite.

## 27. First development checkpoint

The first major checkpoint is reached when OBS can run four Pitel Instant Replay Capture filters and produce continuous per-camera `.srseg/.sridx` files while the existing RAM replay remains functional:

```text
Pitel Instant Replay
Session: RUNNING
Storage: D:\OBS-Replay\...

cam01/*.srseg
cam02/*.srseg
cam03/*.srseg
cam04/*.srseg
```

Only after this is stable should the old replay acquisition path be replaced by EventDB + disk playback.

## 28. Upstream maintenance

Keep the fork easy to rebase/merge with upstream. Avoid gratuitous renames and preserve GPL-2.0-or-later notices.

Local developer setup:

```bash
git remote add upstream https://github.com/Voodoo25/obs-sports-replay.git
git fetch upstream
```

## 29. References

- Upstream: https://github.com/Voodoo25/obs-sports-replay
- OBS native stinger: https://github.com/obsproject/obs-studio/blob/master/plugins/obs-transitions/transition-stinger.c
- OBS platform file/disk API: https://github.com/obsproject/obs-studio/blob/master/libobs/util/platform.h
