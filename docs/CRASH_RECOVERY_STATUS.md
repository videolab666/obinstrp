# Crash Recovery checkpoint

The storage manager now performs a one-shot recovery pass in its background
thread when the plugin starts. The scan explicitly skips the process's active
session, so it never touches current writers and never blocks an OBS video,
render, Qt, or audio callback.

## Recovered media

- interrupted camera `.srseg.part` files;
- interrupted master-audio `.sraud.part` files;
- the atomic-finalize gap where the media file was renamed but the matching
  index remained `.part`;
- missing/trailing index records: indexes are rebuilt from media packet
  records and are not trusted as the recovery source;
- incomplete packet tails: only complete, bounded, monotonic records are
  retained;
- recovered output is published under the normal `.srseg/.sridx` and
  `.sraud/.sraidx` names.

The original `.part` media is not removed until the complete recovered media
and rebuilt index have been written and published. An invalid header or a file
with no complete packet is left in place for manual inspection.

## Failure-safety model

Recovery writes `.recovering` temporary files. The rebuilt index is finalized
first and media second, because catalogs discover finalized media rather than
indexes. If OBS or the machine stops during recovery, the original `.part`
media remains and the next plugin start retries the operation.

## Runtime validation

1. Record one camera and master audio until active `.part` files contain data.
2. Terminate OBS without allowing plugin unload.
3. Preserve copies of the interrupted files for comparison.
4. Restart OBS and wait for the `Pitel Instant Replay: crash recovery finalized ...`
   log line.
5. Verify the recovered `.srseg/.sridx` and `.sraud/.sraidx` pairs open through
   the disk players and cover the expected tail of the previous session.
6. Run `tools/srseg_inspect.py` against every recovered video pair and require
   `validation: OK`.
7. Repeat with truncation at every byte position inside a packet header,
   payload, and index record.
8. Repeat after manually simulating the finalize gap: rename only `.srseg.part`
   to `.srseg` while leaving `.sridx.part` behind.
9. Repeat with a corrupt magic/version and confirm recovery logs an error but
   leaves the original file untouched.
10. Test plugin unload while recovery is scanning; shutdown must wait cleanly
    for the storage thread without deleting source `.part` files.

## Still outside this checkpoint

- SQLite segment-table reconciliation (the current catalogs are file-backed);
- resuming the previous session as the active recording session;
- salvage after corruption in the middle of a file (recovery keeps the valid
  prefix and treats the rest as an invalid tail);
- automated fault-injection tests on Windows filesystems and removable disks.
