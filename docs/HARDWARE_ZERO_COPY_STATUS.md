# Hardware decode and zero-copy status

This document describes the performance path introduced by
`feature/hardware-zero-copy` on top of the continuous disk/Event replay engine.

## Scope

The optimization is deliberately split at the real CPU/GPU boundaries used by
OBS rather than treating "zero-copy" as a single switch.

## Disk/Event replay: hardware decode

On Windows while OBS is using its normal Direct3D 11 renderer, the disk replay
player asks FFmpeg's native H.264 decoder for D3D11VA output and supplies
**OBS's own `ID3D11Device`** as the FFmpeg hardware device.

```text
.srseg H.264 packet
    -> FFmpeg H.264 parser/decoder
    -> D3D11VA hardware decode
    -> AV_PIX_FMT_D3D11 decoder surface
    -> optional GPU->GPU copy into replay LRU texture
```

The compressed bitstream still passes through CPU memory, but the decoded image
is not downloaded to system RAM. If OBS is not using D3D11, the platform is not
Windows, or hardware-device setup fails, the existing software decoder remains
the fallback.

## Zero-CPU-copy replay presentation

`Pitel Instant Replay Event Output` was changed from an asynchronous source that
submitted CPU YUV planes with `obs_source_output_video()` to a synchronous video
source. It deliberately does **not** advertise `OBS_SOURCE_CUSTOM_DRAW`: the
renderer ultimately presents one OBS texture with `obs_source_draw()`, so libobs
must provide its normal single-texture source effect.

For a D3D11 hardware frame:

```text
D3D11VA NV12 decoder/cache surface
    -> D3D11 VideoProcessor (BT.709 limited -> BGRA full)
    -> OBS-owned D3D11 BGRA render texture
    -> OBS compositor
```

The VideoProcessor operation is a GPU-side color conversion/copy. There is no
GPU -> CPU -> GPU round trip. This is therefore **zero CPU copy/readback**, not a
claim that no GPU-side copy occurs.

The output texture is owned by OBS and drawn with the normal libobs graphics API,
so scenes, transitions, Stingers, Studio Mode and downstream OBS rendering still
see a normal source.

## Thread safety

Cue, angle validation and playlist fallback can decode before a replay is on air
and can run outside the render callback. FFmpeg's D3D11 device lock/unlock
callbacks therefore enter/leave the OBS graphics context around D3D11 decode
operations. OBS's graphics context is recursive for the thread that already owns
it, so the same mechanism is safe when decode happens from the normal video tick.

## Hardware-frame cache

The existing 192 MiB decoded-frame LRU remains in use, but it does **not** retain
large numbers of FFmpeg decoder-pool surfaces. Holding decoder output references
in a long-lived cache can exhaust the finite D3D11VA surface pool and eventually
stall decoding.

Before a D3D11 frame is inserted into the LRU, Pitel Instant Replay performs a
GPU-to-GPU `CopySubresourceRegion` into an independent one-slice NV12 texture on
the same OBS device. The cached `AVFrame` owns that texture, while the original
decoder surface can return to FFmpeg's pool. No image data is read back to CPU
memory. Hardware cache entries are budgeted using their logical NV12 footprint
(1.5 bytes/pixel), so the existing byte limit also bounds replay VRAM use.

## Recording: D3D11 capture-to-hardware-encoder path

A second GPU path is now implemented for Windows/D3D11 when `Auto`, `NVENC` or
`AMF` is selected. It is deliberately separate from the portable CPU encoder so
runtime capability failure can fall back without destabilizing replay storage.

The Capture Filter still observes `struct obs_source_frame` in `filter_video` to
learn the source dimensions and to preserve the asynchronous source timestamp
mapping. Actual encoding, however, can run from an OBS main-render callback:

```text
OBS camera/filter target
    -> OBS render into D3D11 BGRA texrender
    -> D3D11 VideoProcessor (RGB -> BT.709 limited NV12)
    -> FFmpeg AV_PIX_FMT_D3D11 NV12 hwframe
    -> h264_nvenc or h264_amf
    -> H.264 packet
    -> RAM replay ring + .srseg writer
```

No CPU readback or CPU YUV plane copy exists between the OBS render texture and
the NVENC/AMF input frame. The hardware frame pool uses D3D11 textures created
for VideoProcessor output and submitted directly through FFmpeg's D3D11 hardware
frames API.

### What this does and does not mean

This is **plugin-side GPU-resident capture-to-encoder processing**. It is not a
claim that every physical camera is sensor-to-encoder zero-copy. For a normal
USB/DirectShow asynchronous source, OBS may already have received the camera
frame in CPU memory and uploaded it to its render texture before this plugin
renders the source. That upstream transfer is outside the Capture Filter's
control.

The optimization still removes the plugin's former path of CPU format conversion
and encoder-input upload for NVENC/AMF. It is especially useful when a source is
already GPU-origin, is reused by the compositor, or when multiple replay cameras
would otherwise each perform CPU conversion/upload work.

`QSV` is intentionally **not** labelled zero-copy yet. It continues through the
portable CPU-frame encoder path until a correct oneVPL/D3D11 interop path is
implemented and validated. `libx264` remains CPU by definition.

## CPU capture fallback and NV12 fast path

The portable Capture Filter path remains intact on all platforms and is also the
fallback after a Windows hardware capability/runtime failure. When its incoming
frame is already NV12 at encoder resolution, the two NV12 planes are copied
directly into the encoder input frame and `swscale` is bypassed. Other formats or
sizes continue through the existing conversion path.

The GPU and CPU encoders are never allowed to write into the same open segment.
If GPU creation or runtime encoding fails, the current writer is closed, the RAM
video ring is cleared, and the next CPU packet begins a clean stream boundary
with that encoder's own SPS/PPS.

## Timestamp domains

Disk/Event media uses `obs_get_video_frame_time()` because Event IN/OUT markers
live on the OBS global video clock. Legacy RAM replay historically stores camera
video against the asynchronous source/device timestamp used alongside its audio
ring.

The GPU encode callback therefore keeps the most recent mapping between source
time and OBS video time. GPU-generated packets are stamped on disk in OBS time,
while their RAM-ring timestamps are mapped back into the source clock. This
preserves compatibility with both continuous Event replay and legacy RAM replay.

## Fallback behavior

- Windows + OBS D3D11 + supported GPU/driver: D3D11VA replay decode and GPU-only replay presentation.
- Hardware replay presentation failure: transfer that frame to CPU, convert/upload it, and keep replay visible; a warning is logged once.
- No compatible replay hardware device: software decode plus the portable BGRA render fallback.
- Windows/D3D11 + `Auto`/`NVENC`/`AMF`: try D3D11 render -> NV12 hwframe -> direct hardware encode.
- GPU encoder creation/runtime failure: close the writer, clear the video ring, and restart on the portable CPU encoder path.
- `QSV`/`x264`, non-Windows or non-D3D11: portable CPU-frame encoder path.
- CPU input already NV12 at native size: direct plane copy, no `swscale`.

Correctness takes priority over keeping a hardware path active after a capability
failure; an optimization failure must not turn an on-air replay black or mix
incompatible codec headers in one storage segment.

## Operator Hardware / Performance status

The Replay operator dock now exposes the runtime path instead of only showing
the configured encoder preference. A compact per-camera table is refreshed from
callback-published snapshots and therefore does not take the encoder/render
mutex from the Qt frontend thread.

For each capture camera the panel reports:

- actual active path (`D3D11 -> NVENC/AMF`, `CPU -> NVENC/AMF/QSV/x264`, waiting or error);
- video size/FPS, GOP and QP;
- disk writer queue depth/high-watermark, dropped packets and recording state;
- RAM replay-buffer footprint, finalized segment count and encoder submission
  timing in the row tooltip;
- explicit D3D11 creation/runtime fallback reason when a GPU path failed.

The summary also reports replay bus A/B decoder state (`D3D11VA` or software),
current decoded size, disk-player LRU cache hit ratio and decoded-frame count.
The encode timing is intentionally labelled as **submission/callback time**: it
does not pretend to measure asynchronous GPU completion latency.

## Replay Setup and REC preflight

The operator dock now has an idempotent `SETUP` control next to REC/settings.
Replay Setup can create or repair separate `Pitel Replay A` / `Pitel Replay B`
scenes with Event Output A/B, fit the output scene items to the OBS base canvas,
and attach/remove only the Pitel Capture filter on operator-selected compatible
asynchronous video sources. Existing valid user A/B topology remains usable and
unrelated scenes, scene items and filters are not deleted or renamed. Setup
mutations are initiated from the Qt frontend thread and persisted through the OBS
frontend save API rather than touching scene/source topology from render or
encoder callbacks.

REC start performs a lightweight preflight. With no enabled Capture filters it
offers Replay Setup first. If an Event Transition is configured but separate A/B
replay scenes are missing, the operator can create/repair A/B or deliberately
continue in Cut-only mode. REC stop is immediate and does not run preflight.
Disabled Capture filters are excluded from centralized REC counts so an
intentionally disabled camera cannot leave the dock stuck in STARTING.

## CI/build validation

The GPU encoder implementation is guarded so non-Windows targets compile only
portable stubs. The Windows implementation is compiled against OBS 31.1.1 deps
with MSVC and uses the same D3D11 device as OBS. CI compilation validates API and
ABI compatibility, but it cannot prove actual NVENC/AMF runtime availability on
the hosted runners.

Real-GPU testing remains mandatory before treating this branch as release-ready.

## Runtime validation checklist

Windows/D3D11 must be validated on real hardware before merging into the release
checkpoint:

- Open Replay Setup in an existing scene collection and verify repeated Create/Repair A/B does not duplicate valid A/B scenes/outputs.
- Verify selecting/deselecting cameras only adds/removes Pitel Capture filters and preserves every unrelated filter.
- Verify REC preflight offers Setup when no Capture filters are enabled, and offers A/B repair vs Cut when Event Transition needs two replay scenes.
- Verify a disabled Capture filter is excluded from centralized REC counts and does not leave STARTING stuck.
- NVIDIA replay: confirm D3D11VA decode, GPU Video Decode utilization and no replay CPU-readback spike.
- NVIDIA capture: confirm `h264_nvenc`, GPU Video Encode utilization and no plugin GPU->CPU readback during recording.
- AMD replay: confirm D3D11VA/VCN decode on the OBS adapter.
- AMD capture: confirm `h264_amf` accepts the D3D11 NV12 hwframe path continuously.
- Intel: confirm replay D3D11VA decode; QSV recording should explicitly report/use the CPU-frame fallback until D3D11 QSV interop is implemented.
- Compare one, two, four and maximum intended camera counts at 1080p50/60.
- Compare CPU%, GPU 3D%, GPU Video Decode%, GPU Video Encode%, RAM and VRAM against `feature/disk-replay-core`.
- Verify GPU capture source scale/aspect and color against the CPU path with a known BT.709 test chart.
- Record at least 30 minutes and check packet continuity, GOP cadence, segment rotation and disk cleanup.
- Toggle global recording off/on repeatedly and verify writer/session state.
- Change encoder, QP and GOP while running and verify a clean stream/segment boundary.
- Force NVENC/AMF creation failure and confirm automatic CPU fallback continues recording.
- Force a GPU conversion/runtime failure if practical and confirm the same clean fallback boundary.
- Reverse 25/50/100%, jog, shuttle and exact frame step across GOP and segment boundaries.
- Timeline scrub repeatedly across cached and uncached ranges.
- Same-Event angle changes while on air.
- Cue A while B is on air and A <-> B TAKE.
- Native Stinger IN/OUT and manual cut-away cleanup.
- Active `.part` near-live playback while recording continues.
- OBS graphics reset/restart and source recreation.

## Expected performance effect

Replay-side savings come primarily from removing software H.264 decode and
eliminating decoded-frame readback/upload. The benefit should be largest during
reverse, jog, scrub and rapid multicamera switching where many frames are decoded
from preceding keyframes.

On Windows NVENC/AMF recording, the new GPU path removes the plugin's CPU
conversion and encoder upload stage. Its actual whole-system benefit depends on
the camera source: an already-GPU source can remain GPU resident through encode,
while a USB/DirectShow source may still incur OBS's upstream CPU-to-GPU upload.
The retained CPU NV12 fast path remains useful for QSV, non-D3D11 systems and
hardware-failure fallback.