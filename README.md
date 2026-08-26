# Pitel Instant Replay for OBS Studio

**Low-memory instant replay for live sports broadcasts.**

Pitel Instant Replay captures the last seconds of any camera into a **compressed**
in-memory buffer (hardware H.264 all-intra via NVENC / AMF / QSV, with an
x264 software fallback) instead of holding raw frames in RAM. A 15-second
1080p60 buffer uses on the order of **tens of megabytes instead of several
gigabytes**, so multi-camera replay setups run comfortably on ordinary
streaming PCs.

![Pitel Instant Replay running in a live off-road race broadcast](docs/screenshot-obs.png)

## Why

The popular raw-buffer replay plugins keep every frame uncompressed in memory
(~8 MB per 1080p frame). With several cameras and a long buffer that easily
reaches many gigabytes and pushes a PC to its limit mid-broadcast. Pitel
Instant Replay solves that by encoding the buffer with your GPU, keeping quality high
while cutting memory use by roughly **50–100×**.

## Features

- **Compressed replay buffer** — per-camera capture filter, buffer duration
  and quality configurable.
- **Auto-play on scene switch** — cut to the replay scene and it plays the
  last N seconds automatically.
- **Slow motion & reverse** — playback speed from 10% to 400%, hotkeys for
  speed presets and direction.
- **Configurable end action** — freeze on the last frame, **return to the
  previous scene**, **cut to the replay camera's scene**, or loop.
- **Sponsor bumpers** — optional intro and outro video clips played around
  the replay (e.g. a "REPLAY" sting with a sponsor).
- **Automatic save to disk** — every replay is saved as an `.mp4` (muxed
  from the already-encoded frames, no re-encode).
- **Replay bin dock** — a dockable panel shows the last replays as
  thumbnails; **double-click sends a saved replay to program** and returns
  to the previous scene when it ends. Perfect for re-showing a play minutes
  later.
- **Multi-camera** — one capture filter per camera; replay any of them.

## Requirements

- OBS Studio 31+ (developed and tested on 32.0.2, Windows).
- A GPU with a hardware H.264 encoder recommended (NVIDIA/AMD/Intel); falls
  back to x264 software encoding otherwise.

## Installation

For the stable RAM-replay version, download and run the installer from the
[latest release](../../releases/latest).

The vMix-style disk/Event operator is currently developed on
`feature/disk-replay-core`. Open the latest successful workflow run for
[pull request #1](../../pull/1/checks), download the Windows x64 artifact,
unpack its inner `pitel-instant-replay-*-windows-x64.zip`, and copy the contained
`obs-plugins` and `data` directories into the OBS installation directory.
Close OBS before replacing plugin files, then restart it.

Open **Docks → Pitel Instant Replay — Instant Replay**. The unified dock starts on
the **Replay operator** tab; the legacy saved-MP4 bin remains available on the
second tab. Development artifacts are unsigned and require OBS Studio 31+.

## How to use

### Disk/Event operator (vMix-style)

1. Add **Pitel Instant Replay Capture** to every camera. Encoder, quality,
   GOP and sync offset remain per-camera settings; the dock controls whether
   all configured cameras are recording.
2. In **Replay operator**, click **START RECORD**. Wait for the green `REC`
   status and increasing packet/MB counters before marking an Event. The dock
   reports encoder/write failures and a reached disk-space reserve directly.
   The **Hardware / Performance** block shows the actual per-camera path
   (`D3D11 -> NVENC/AMF` versus CPU fallback), video/GOP settings, disk queue,
   dropped packets and A/B replay decode/cache status.
3. Create an Event with **IN** then **OUT**, or use **-5/-10/-20** after the
   action. Empty Events are rejected until recorded packets are available.
4. Add a **Pitel Instant Replay Event Output** source configured for bus A to a
   replay scene. Select an Event and click **PLAY EVENT** (or double-click its
   row) to cue it internally and take it to air in one action.
5. **Cue A/B** and **TAKE A/B** remain available for advanced two-bus operation:
   prepare B while A is on air, then switch without replaying the IN Stinger.
   Use **RETURN LIVE** to return to the scene that was live before replay.
6. The angle buttons show recorded previews for the selected Event. Double-click
   the Speed, Name or Tag cell to edit a highlight; use arrows and
   Move/Copy/Duplicate to assemble the 20 highlight lists.
7. Choose **Master**, **Selected camera** or **Off** in the bus audio control.
   In Event Output properties, set **Replay gain** and choose whether live audio
   is kept, ducked or muted during TAKE. Audio currently plays at normal forward
   speed; reverse and variable-speed replay remain muted.
8. Open the **Storage** tab to see every replay session and its size, inspect the
   last automatic-cleanup result, or permanently delete a closed session. The
   active session is protected from manual deletion.
9. Replay Setup can also record **PROGRAM**: the final composited OBS Program/PGM output becomes a manual replay angle alongside ISO cameras. On Windows/D3D11 it stays GPU-resident through NVENC/AMF; PROGRAM is intentionally skipped by automatic **Play Each Angle** tours.

Continuous recording keeps its camera sources showing internally even when
they are not in the current OBS scene. **STOP RECORD** releases those holds and
stops every Pitel capture writer. The default disk-space reserve is 20 GB; set
the recording folder, reserve, segment duration and optional native OBS
Stingers from the dock settings.

### Legacy RAM replay

1. **Add the capture filter to each camera.** Right-click a camera source →
   *Filters* → add **Pitel Instant Replay Capture**. Set the buffer duration and
   quality there.
2. **Add the playback source.** In your replay scene, add a **Pitel Instant Replay**
   source and pick which camera to replay from in its properties. Set the
   playback speed and, under *When the replay ends*, choose *Return to the
   previous scene*. Optionally set intro/outro sponsor clips.
3. **Live replay.** Cut to the replay scene → the last N seconds play
   automatically, then it returns to your main camera.
4. **Replay bin.** Open the **Replays (Pitel Instant Replay)** dock. It lists your
   saved replays with thumbnails; double-click any of them to send it to
   program. The save folder is set with the ⚙ button (defaults to
   `Videos/Pitel Instant Replay`).

### The capture filter (per camera)

![Pitel Instant Replay Capture filter](docs/screenshot-capture-filter.png)

### The playback source

![Pitel Instant Replay source properties](docs/screenshot-source-properties.png)

### The replay bin dock

![Replay bin dock with thumbnails](docs/screenshot-dock.png)

Assign hotkeys under *Settings → Hotkeys* (capture, play/pause, speed
presets, reverse, play last saved replay).

## Questions, ideas & feedback

Come say hi in [**Discussions**](../../discussions) — that is the best place to
reach me:

- ❓ **[Q&A](../../discussions/categories/q-a)** — something not working, or not
  sure how to set it up.
- 💡 **[Ideas](../../discussions/categories/ideas)** — a feature you need for
  your workflow. This is what drives the roadmap.
- 📺 **[Show and tell](../../discussions/categories/show-and-tell)** — using it
  on a real broadcast? I would love to see it.

For confirmed bugs, open an [issue](../../issues).
*Se habla español* — escribí en el idioma que prefieras.

## Building from source

Uses the standard [OBS plugin template](https://github.com/obsproject/obs-plugintemplate)
build system.

```sh
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Project

Pitel Instant Replay is maintained in [`videolab666/obinstrp`](https://github.com/videolab666/obinstrp).

## License

GPL-2.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.md` for licensing and historical third-party provenance.
