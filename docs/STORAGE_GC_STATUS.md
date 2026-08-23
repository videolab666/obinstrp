# Storage GC checkpoint

- Minimum free disk space is the hard reserve.
- GC purge target is the hysteresis target and is always >= the reserve.
- Default low-space action: delete oldest finalized replay segments that are not referenced by any saved Event.
- Every saved Event pins overlapping media, regardless of Protected state. Active `.part` files are never candidates.
- Automatic GC runs on a dedicated storage-management thread, never on capture/render/Qt callbacks.
- If GC cannot recover the reserve, the existing per-camera writer reserve stops new segment creation until space returns.
- Event create/range-update and GC final overlap-check + unlink share a process-wide media-reference guard.
- Automatic GC is intentionally limited to the active session in this checkpoint; cross-session retention comes later.

## Runtime validation

1. Record 4 x 1080p50/60 until near the configured reserve.
2. Save Events in old and recent ranges.
3. Force GC by raising the reserve above current free space.
4. Verify oldest unreferenced finalized segments disappear first.
5. Verify every Event-overlapping segment and every `.part` file remains.
6. Verify recording resumes after free space is restored.
7. Fill the session with only Event-pinned media and verify recording pauses instead of deleting it.
8. Test Stop mode (writer reserve pauses without GC) and Warn-only mode (writer reserve disabled) separately.
9. Create Events rapidly during GC to exercise the media-reference guard.
