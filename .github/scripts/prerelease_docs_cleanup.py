from pathlib import Path

ROOT = Path('.')


def replace_once(path: str, old: str, new: str) -> None:
    file = ROOT / path
    text = file.read_text(encoding='utf-8')
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{path}: expected one match, found {count}: {old[:80]!r}')
    file.write_text(text.replace(old, new, 1), encoding='utf-8')


replace_once(
    'docs/DISK_ENGINE_TESTING.md',
    '''The current session root defaults to:\n\n```text\n<Videos>/Pitel Instant Replay/Sessions/\n```\n''',
    '''The current session root defaults to the plugin configuration namespace:\n\n```text\n<OBS module config>/standalone-v1/Sessions/\n```\n''',
)
replace_once(
    'docs/DISK_ENGINE_TESTING.md',
    'If the legacy replay folder was changed before the first continuous session is created, the default session root follows that folder and appends `/Sessions`.',
    'The session root is configured only through the current Pitel Instant Replay Session settings. Pre-release replay-folder settings are not migrated or consulted.',
)
replace_once(
    'docs/DISK_ENGINE_TESTING.md',
    '''## Known limitations at this checkpoint\n\n- Continuous disk recording is video-only. Long-session audio comes later.\n- Disk replay is not yet wired into the OBS playback source.\n- Minimum free-space reserve is enforced, but automatic free-space garbage collection is not implemented yet.\n- The reserve/session-root controls are in the config layer but do not yet have their final operator settings UI.\n- No SQLite event database yet.\n- No crash-recovery scanner yet, although `.part` and packet framing are designed for it.\n- No short-GOP mode yet; legacy playback still requires All-I.\n- GitHub Actions should be enabled for the fork so format/build CI can validate each synchronization of the draft PR.\n''',
    '''## Current status\n\nThis document originated as the first disk-engine validation checklist. The current plugin has moved beyond that checkpoint: Session/Event replay, recording runs, master/camera audio, automatic storage cleanup, crash recovery, short-GOP playback and the operator Session UI are all part of the active architecture. Validation should target the current Session/Event workflow rather than any pre-release playback-source or loose-MP4 path.\n''',
)

(ROOT / 'docs/SHORT_GOP_STATUS.md').write_text(
    '''# Short-GOP replay status\n\nThe active replay codec path supports short-GOP H.264 on the disk-backed Session/Event engine. There is no capture-time RAM replay store or loose-MP4 playback path.\n\n## Implemented\n\n- Session video is stored in indexed segment media with real keyframe flags.\n- Non-sequential seek, reverse and jog locate the preceding keyframe and decode forward to the requested frame.\n- The encoder exposes All-I, 0.25 s, 0.50 s and 1.00 s GOP presets.\n- The default is 0.50 s (`Balanced`): approximately 30 frames at 60 fps or 25 frames at 50 fps.\n- B-frames remain disabled so DTS/PTS order stays predictable for replay seeking and export.\n- Closed GOP is requested through `AV_CODEC_FLAG_CLOSED_GOP`.\n- The selected GOP preset is applied to NVENC, AMF, QSV and libx264 creation/fallback paths.\n- MP4 Event export preserves real keyframe flags instead of marking every packet as independently decodable.\n\n## Decoded frame cache\n\nThe persistent disk player uses a bounded LRU decoded-frame cache keyed by `(segment sequence, packet position)`. Cached pictures therefore remain addressable across segment switches and active `.part` to finalized-file transitions.\n\nThe default cache budget is **192 MiB per replay player**. The budget is byte-bounded, so 4K naturally retains fewer decoded frames rather than scaling memory use with replay duration. Decoded pictures use FFmpeg reference-counted `AVFrame` clones.\n\n## Runtime validation\n\nCI verifies compilation, packaging and formatting, but real hardware still has to prove restart-safe keyframe behavior for NVENC, AMF, QSV and libx264 fallback. For each backend, test at least 1080p50 and 1080p60 with `Balanced (0.50 s)` and verify:\n\n1. observed keyframe interval is approximately 25 frames at 50 fps / 30 frames at 60 fps;\n2. forward replay remains frame-correct;\n3. 25% and 50% slow motion remain frame-correct;\n4. reverse playback has no corruption across GOP boundaries;\n5. repeated reverse/jog over a GOP benefits from cache hits;\n6. switching reverse back to forward does not show a stale frame or force unnecessary decoder rewind;\n7. random seek/jog lands on the requested frame after rebuilding from the preceding keyframe;\n8. Session/Event replay seeks correctly across indexed segment media;\n9. replay reads finalized segments and the active `.part` segment while recording continues;\n10. reverse/jog remains timestamp- and frame-order-correct across segment changes.\n\n## Current integration\n\nShort-GOP playback is part of the same Session timeline used by Events, recording runs, camera/PROGRAM video and replay audio. It is not a compatibility layer over an older replay implementation.\n''',
    encoding='utf-8',
)

replace_once(
    'docs/EVENT_EXPORT_STATUS.md',
    '- Event rows remain metadata-only; export does not mutate the Event database or the legacy MP4 replay importer.',
    '- Event rows remain metadata-only; export does not mutate the Event database or Session media.',
)

roadmap = ROOT / 'docs/VMIX_REPLAY_ROADMAP.md'
text = roadmap.read_text(encoding='utf-8')
start = text.index('## 2. Important implementation constraint discovered in the current code')
end = text.index('## 3. Target codec policy', start)
text = text[:start] + '''## 2. Current playback constraint\n\nThe active replay engine is keyframe-aware: non-sequential seek, reverse and jog resolve the previous IDR/keyframe and decode forward to the requested frame. Short-GOP media is therefore a first-class Session/Event format rather than a migration mode layered on top of an All-I RAM player.\n\nDo not reintroduce a second capture-time replay store or a playback path that assumes every H.264 packet is independently decodable. Disk Session media is the single source of replay truth.\n\n''' + text[end:]
for old, new in (
    ('Legacy saved MP4 files should remain loadable.', 'Export operates on current Session/Event media only.'),
    ('### M2 — Disk segment writer while legacy replay remains All-I', '### M2 — Disk segment writer foundation'),
    ('- [ ] Write continuous ISO media in parallel with current RAM replay.', '- [ ] Write continuous ISO media directly into Session segments.'),
    ('2. Session manager + disk segment format/writer while legacy All-I stays intact.', '2. Session manager + disk segment format/writer.'),
):
    if old not in text:
        raise RuntimeError(f'roadmap: missing {old!r}')
    text = text.replace(old, new, 1)
roadmap.write_text(text, encoding='utf-8')

hardware = ROOT / 'docs/HARDWARE_ZERO_COPY_STATUS.md'
text = hardware.read_text(encoding='utf-8')
old = '''The GPU and CPU encoders are never allowed to write into the same open segment.\nIf GPU creation or runtime encoding fails, the current writer is closed, the RAM\nvideo ring is cleared, and the next CPU packet begins a clean stream boundary\nwith that encoder's own SPS/PPS.\n'''
new = '''The GPU and CPU encoders are never allowed to write into the same open segment.\nIf GPU creation or runtime encoding fails, the current writer is closed and the\nnext CPU packet begins a clean stream/discontinuity boundary with that encoder's\nown SPS/PPS.\n'''
if old not in text:
    raise RuntimeError('hardware status: missing fallback block')
text = text.replace(old, new, 1)
old = '''## Timestamp domains\n\nDisk/Event media uses `obs_get_video_frame_time()` because Event IN/OUT markers\nlive on the OBS global video clock. Legacy RAM replay historically stores camera\nvideo against the asynchronous source/device timestamp used alongside its audio\nring.\n\nThe GPU encode callback therefore keeps the most recent mapping between source\ntime and OBS video time. GPU-generated packets are stamped on disk in OBS time,\nwhile their RAM-ring timestamps are mapped back into the source clock. This\npreserves compatibility with both continuous Event replay and legacy RAM replay.\n'''
new = '''## Timestamp domains\n\nDisk/Event media is normalized onto the OBS global video clock and then mapped\ninto the active Session recording-run timeline. Event IN/OUT markers, camera\nvideo, PROGRAM video and replay audio therefore share one Session time domain.\nThere is no parallel capture-time RAM replay timestamp domain.\n'''
if old not in text:
    raise RuntimeError('hardware status: missing timestamp block')
hardware.write_text(text.replace(old, new, 1), encoding='utf-8')

namespace = ROOT / 'docs/NAMESPACE_ISOLATION.md'
text = namespace.read_text(encoding='utf-8')
text = text.replace('- playback source ID: `pitel_instant_replay`\n', '')
text = text.replace(
    '- Event Output source ID: `pitel_instant_replay_event_output`\n',
    '- Event Output source ID: `pitel_instant_replay_event_output`\n- no standalone playback-source ID is registered; A/B replay is rendered through Event Output sources\n',
)
text = text.replace(
    '- default recording root: `Videos/Pitel Instant Replay/Recorder`',
    '- default Session root: `<OBS module config>/standalone-v1/Sessions`',
)
namespace.write_text(text, encoding='utf-8')

# Guard against documentation accidentally restoring removed runtime concepts.
for path in (
    'docs/DISK_ENGINE_TESTING.md',
    'docs/SHORT_GOP_STATUS.md',
    'docs/EVENT_EXPORT_STATUS.md',
    'docs/VMIX_REPLAY_ROADMAP.md',
    'docs/HARDWARE_ZERO_COPY_STATUS.md',
    'docs/NAMESPACE_ISOLATION.md',
):
    text = (ROOT / path).read_text(encoding='utf-8')
    for token in ('Legacy saved MP4', 'legacy replay folder', 'current RAM replay', 'legacy All-I', 'RAM-ring timestamps', 'legacy RAM replay'):
        if token in text:
            raise RuntimeError(f'{path}: stale token remains: {token}')
