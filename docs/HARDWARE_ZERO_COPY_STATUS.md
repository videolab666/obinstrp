# Hardware decode and zero-copy status

This document describes the performance path introduced by
`feature/hardware-zero-copy` on top of the continuous disk/Event replay engine.

## Scope

The optimization is deliberately split at the real CPU/GPU boundaries used by
OBS rather than treating "zero-copy" as a single switch.

### Disk/Event replay: hardware decode

On Windows while OBS is using its normal Direct3D 11 renderer, the disk replay
player now asks FFmpeg's native H.264 decoder for D3D11VA output and supplies
**OBS's own `ID3D11Device`** as the FFmpeg hardware device.

That gives this path:

```text
.srseg H.264 packet
    -> FFmpeg H.264 parser/decoder
    -> D3D11VA hardware decode
    -> AV_PIX_FMT_D3D11 decoder surface
    -> replay frame cache (surface references only)
```

The compressed stream still passes through CPU memory, which is tiny compared
with decoded video. The decoded image itself is not downloaded to system RAM.

If OBS is not using D3D11, the platform is not Windows, or hardware device setup
fails, the existing software decoder remains the fallback.

## Zero-CPU-copy replay presentation

`Pitel Instant Replay Event Output` was changed from an asynchronous source that
submitted CPU YUV planes with `obs_source_output_video()` to a synchronous
`OBS_SOURCE_CUSTOM_DRAW` source.

For a D3D11 hardware frame:

```text
D3D11VA NV12 decoder surface
    -> D3D11 VideoProcessor (BT.709 limited -> BGRA full)
    -> OBS-owned D3D11 BGRA render texture
    -> OBS compositor
```

The VideoProcessor operation is a GPU-side color conversion/copy. There is no
GPU -> CPU -> GPU round trip. This is therefore **zero CPU copy/readback** rather
than a claim that no GPU-side copy exists at all.

The output texture is owned by OBS and drawn with the normal libobs graphics
API, so scenes, transitions, Stingers, Studio Mode and downstream OBS rendering
continue to see a normal source.

## Thread safety

Cue, angle validation and playlist fallback can decode before a replay is on air
and can run outside the render callback. FFmpeg's D3D11 device lock/unlock
callbacks therefore enter/leave the OBS graphics context around D3D11 decode
operations. OBS's graphics context is recursive for the thread that already owns
it, so the same mechanism is safe when decode happens from the normal video tick.

## Hardware-frame cache

The existing 192 MiB decoded-frame LRU remains in use. `AVFrame` clones of
D3D11VA output retain references to decoder-pool surfaces instead of copying the
image. Hardware entries are budgeted using the logical NV12 footprint
(1.5 bytes/pixel) so reverse, jog and random seeks cannot pin an unbounded number
of decode surfaces.

## Recording-side optimization

The Capture Filter receives `struct obs_source_frame` CPU planes from libobs.
That callback is already on the CPU side of an asynchronous camera source, so a
true camera-input GPU zero-copy path cannot be created merely by changing the
FFmpeg encoder call.

The avoidable work at this boundary has been removed: when the source is already
NV12 at the encoder resolution, Pitel Instant Replay now copies the two NV12
planes directly into the encoder input frame and completely bypasses swscale.
Other formats and scaling still use the existing swscale conversion.

For ordinary USB/DirectShow capture devices this is the appropriate low-risk
optimization. A future *GPU capture tap* would be a separate architecture for
sources that are already GPU textures; it would need source rendering into a
D3D11 surface plus backend-specific texture input for NVENC/QSV/AMF and is not a
transparent replacement for the current Capture Filter.

## Fallback behavior

- Windows + OBS D3D11 + supported GPU/driver: D3D11VA decode and GPU-only replay presentation.
- Hardware decode surface but native presentation setup fails: transfer that frame to CPU, swscale to BGRA, upload and keep replay visible; a warning is logged once.
- No compatible hardware decode device: software decode plus the same portable BGRA render fallback.
- Capture input already NV12 at native size: direct plane copy, no swscale.
- Other capture formats/sizes: existing conversion path.

Correctness takes priority over keeping a hardware path active after a capability
failure; a failed optimization must not turn an on-air replay black.

## Runtime validation checklist

Windows/D3D11 must be validated on real hardware before merging into the release
checkpoint:

- NVIDIA: confirm D3D11VA decode, actual GPU Video Decode utilization and no CPU readback spike.
- Intel: confirm D3D11VA/Quick Sync video decode on the OBS adapter.
- AMD: confirm D3D11VA/UVD-VCN video decode on the OBS adapter.
- 1080p50 and 1080p60 forward playback for at least 30 minutes.
- Reverse 25/50/100%, jog, shuttle and exact frame step across GOP and segment boundaries.
- Timeline scrub repeatedly across cached and uncached ranges.
- Same-Event angle changes while on air.
- Cue A while B is on air and A <-> B TAKE.
- Native Stinger IN/OUT and manual cut-away cleanup.
- Active `.part` near-live playback while recording continues.
- Force software fallback and verify identical Event/playlist behavior.
- OBS graphics reset/restart and source recreation.
- Compare CPU%, GPU Video Decode%, GPU 3D%, RAM and VRAM against the disk-replay-core baseline.

## Expected performance effect

The largest replay-side saving is removal of software H.264 decode and decoded
frame readback/upload. CPU savings vary by CPU, GPU and replay behavior, but are
most visible during reverse/jog/scrub and rapid multicamera switching where many
frames must be decoded from preceding keyframes.

The recording-side NV12 fast path removes format conversion but intentionally
retains one CPU plane copy into the encoder frame. That copy is much cheaper than
a swscale conversion and preserves all existing NVENC/AMF/QSV/libx264 fallback
behavior.
