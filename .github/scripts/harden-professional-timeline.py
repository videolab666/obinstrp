from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


root = Path('.')
dock_path = root / 'src/sr-event-dock.cpp'
workflow_path = root / '.github/workflows/validate-program-output.yml'
script_path = root / '.github/scripts/harden-professional-timeline.py'

dock = dock_path.read_text(encoding='utf-8')

# Short recordings can be smaller than the nominal minimum zoom span. Keep
# qBound's lower/upper arguments ordered in that case.
dock = replace_once(
    dock,
    '''\t\tconst uint64_t minimumSpan = std::max<uint64_t>(minimumDurationNs * 6ULL, 250000000ULL);\n\t\tnextSpan = qBound(minimumSpan, nextSpan, recordSpan);''',
    '''\t\tconst uint64_t minimumSpan = std::max<uint64_t>(minimumDurationNs * 6ULL, 250000000ULL);\n\t\tconst uint64_t boundedMinimumSpan = std::min(minimumSpan, recordSpan);\n\t\tnextSpan = qBound(boundedMinimumSpan, nextSpan, recordSpan);''',
    'short-recording zoom bounds',
)

# When a transient edit preview is loaded, the global transport Play/Pause
# button must behave like EDIT preview and stay inside Event IN/OUT.
old_toggle = '''\tvoid togglePlayPause()\n\t{\n\t\tconst enum sr_replay_bus bus = transportBus();\n\t\tsr_replay_channel_state state = {};\n\t\tif (!sr_replay_channel_get_state(bus, &state) || !state.cued) {\n\t\t\tsetStatus("EventDock.NoCue");\n\t\t\treturn;\n\t\t}\n\t\tconst bool ok = state.playing && !state.paused ? sr_replay_channel_pause(bus, true)\n\t\t\t\t: state.paused                 ? sr_replay_channel_pause(bus, false)\n\t\t\t\t\t\t\t       : sr_replay_channel_play(bus);\n\t\tif (!ok)\n\t\t\tsetStatus("EventDock.TransportFailed");\n\t\trefreshTransportStatus();\n\t}\n'''
new_toggle = '''\tvoid togglePlayPause()\n\t{\n\t\tconst enum sr_replay_bus bus = transportBus();\n\t\tsr_replay_channel_state state = {};\n\t\tif (!sr_replay_channel_get_state(bus, &state) || !state.cued) {\n\t\t\tsetStatus("EventDock.NoCue");\n\t\t\treturn;\n\t\t}\n\t\tif (!replayPlayoutActive() && state.preview_mode) {\n\t\t\ttoggleEditPreview();\n\t\t\trefreshTransportStatus();\n\t\t\treturn;\n\t\t}\n\t\tconst bool ok = state.playing && !state.paused ? sr_replay_channel_pause(bus, true)\n\t\t\t\t: state.paused                 ? sr_replay_channel_pause(bus, false)\n\t\t\t\t\t\t\t       : sr_replay_channel_play(bus);\n\t\tif (!ok)\n\t\t\tsetStatus("EventDock.TransportFailed");\n\t\trefreshTransportStatus();\n\t}\n'''
dock = replace_once(dock, old_toggle, new_toggle, 'edit-aware play pause')

# Restart in EDIT means Goto IN, not the beginning of the transient full
# recording preview interval.
old_restart = '''\tvoid restartTransport()\n\t{\n\t\tsr_replay_channel_restart(transportBus());\n\t\trefreshTransportStatus();\n\t}\n'''
new_restart = '''\tvoid restartTransport()\n\t{\n\t\tsr_replay_channel_state state = {};\n\t\tif (!replayPlayoutActive() && sr_replay_channel_get_state(transportBus(), &state) && state.cued &&\n\t\t    state.preview_mode) {\n\t\t\tgotoEditMarker(false);\n\t\t\treturn;\n\t\t}\n\t\tsr_replay_channel_restart(transportBus());\n\t\trefreshTransportStatus();\n\t}\n'''
dock = replace_once(dock, old_restart, new_restart, 'edit-aware restart')

# Toggle TAKE must never expose a transient whole-recording preview interval.
old_toggle_take = '''\tvoid takeToggleBus()\n\t{\n\t\tif (!controller || !sr_replay_take_toggle(controller)) {\n\t\t\tsetStatus("EventDock.TakeFailed");\n\t\t\treturn;\n\t\t}\n\t\tsetStatus("EventDock.ToggleTaken");\n\t\trefresh();\n\t\trefreshTransportStatus();\n\t}\n'''
new_toggle_take = '''\tvoid takeToggleBus()\n\t{\n\t\tconst enum sr_replay_bus bus = transportBus();\n\t\tsr_replay_channel_state candidate = {};\n\t\tif (sr_replay_channel_get_state(bus, &candidate) && candidate.cued && candidate.preview_mode) {\n\t\t\tif (!cueSelected(bus))\n\t\t\t\treturn;\n\t\t}\n\t\tif (!controller || !sr_replay_take_toggle(controller)) {\n\t\t\tsetStatus("EventDock.TakeFailed");\n\t\t\treturn;\n\t\t}\n\t\tsetStatus("EventDock.ToggleTaken");\n\t\trefresh();\n\t\trefreshTransportStatus();\n\t}\n'''
dock = replace_once(dock, old_toggle_take, new_toggle_take, 'normalize toggle take')

# LIVE can land a few frames ahead of the latest finalized/indexed packet while
# recording. Retry slightly behind live edge so the button reliably displays a
# decodable near-live frame rather than appearing to do nothing.
old_seek_else = '''\t\t} else {\n\t\t\tok = cueEditPreviewAt(editPreviewCamera, eventId, timestampNs, editTimelineStartNs,\n\t\t\t\t\t      editTimelineEndNs);\n\t\t}\n\t\tif (!ok)\n\t\t\treturn false;\n'''
new_seek_else = '''\t\t} else {\n\t\t\tok = cueEditPreviewAt(editPreviewCamera, eventId, timestampNs, editTimelineStartNs,\n\t\t\t\t\t      editTimelineEndNs);\n\t\t\tif (!ok && timestampNs > editTimelineStartNs) {\n\t\t\t\tconst uint64_t retryBack = std::max<uint64_t>(editFrameDurationNs() * 3ULL, 100000000ULL);\n\t\t\t\tconst uint64_t retry = retryBack >= timestampNs - editTimelineStartNs\n\t\t\t\t\t\t       ? editTimelineStartNs\n\t\t\t\t\t\t       : timestampNs - retryBack;\n\t\t\t\tok = cueEditPreviewAt(editPreviewCamera, eventId, retry, editTimelineStartNs,\n\t\t\t\t\t\t      editTimelineEndNs);\n\t\t\t}\n\t\t}\n\t\tif (!ok)\n\t\t\treturn false;\n'''
dock = replace_once(dock, old_seek_else, new_seek_else, 'near-live seek retry')

dock_path.write_text(dock, encoding='utf-8')

# Product commit must restore the normal CI workflow and remove this helper.
workflow_path.write_text('''name: Replay Development CI\nrun-name: Replay Development CI — ${{ github.ref_name }} — ${{ github.event_name }}\n\non:\n  workflow_dispatch:\n  push:\n    branches:\n      - feature/hardware-zero-copy\n\npermissions:\n  contents: read\n\nconcurrency:\n  group: replay-development-ci-${{ github.ref }}\n  cancel-in-progress: true\n\n# Full validation for the active replay development branch.\n# Every push is built; workflow_dispatch keeps a manual Run workflow button\n# available from the default branch. PR validation is intentionally left to\n# the repository's normal PR workflow when this branch is eventually retargeted\n# to main, avoiding duplicate push + pull_request builds during development.\njobs:\n  check-format:\n    name: Check Formatting 🔍\n    uses: ./.github/workflows/check-format.yaml\n    permissions:\n      contents: read\n\n  build-project:\n    name: Build Project 🧱\n    uses: ./.github/workflows/build-project.yaml\n    secrets: inherit\n    permissions:\n      contents: read\n''', encoding='utf-8')
script_path.unlink()
