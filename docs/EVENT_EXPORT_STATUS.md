# Event MP4 export status

The Event dock now exposes the first production export path from the disk replay store: **Fast MP4**. It remuxes the already encoded H.264 camera packets and optional master AAC packets instead of decoding and encoding them again.

## Operator workflow

1. Select a completed Event.
2. Choose **Preferred angle** or **All full angles**.
3. Press **Fast MP4** and choose the destination file or folder.
4. Follow the progress bar. **Cancel** stops the background job and removes its incomplete `.part` file.

Preferred-angle selection uses the Event's preferred camera when that camera has full coverage, then the camera selected in the dock, then the first fully covered camera. All-angle export creates one safely named MP4 per camera with full continuous Event coverage. Existing files are never overwritten.

When the Event audio mode is Master, available master AAC is interleaved into each exported angle. A missing master-audio range does not prevent video export.

## Fast-mode boundaries

Fast mode starts reading video at the preceding IDR keyframe. Packets before Event IN receive negative timestamps and the MP4 edit list hides that decode preroll, preserving a clean start without re-encoding. Event OUT is limited at the stored packet boundary. With the recommended 0.5-second short GOP and B-frames disabled, seeking and boundary behavior remain predictable.

The exporter refuses a camera angle if codec parameters, dimensions, frame rate, or codec extradata change inside the Event. Those cases require the future **Frame Accurate** export mode, which will decode and re-encode boundary regions.

## Reliability and threading

- Catalog scanning, packet reads, muxing, trailer writing, and fast-start relocation run on a worker thread.
- Output is first written to `<destination>.part` and renamed only after a successful MP4 trailer.
- Cancellation and failures remove the temporary file.
- Camera sync calibration is applied while interleaving against the global/master timeline.
- Event rows remain metadata-only; export does not mutate the Event database or Session media.
