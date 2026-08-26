# Pitel Instant Replay branding and compatibility

New packages use `pitel-instant-replay` consistently, including
`pitel-instant-replay.dll`, the plugin data directory, package/bundle names and
logs.

The following hidden OBS persistence identifiers deliberately keep their legacy
values so existing scene collections and hotkeys continue to work:

- `sports_replay`, `sports_replay_capture`, `sports_replay_event_output`;
- `SportsReplay.*` hotkey IDs;
- existing dock IDs/object names used by saved OBS layouts.

These are compatibility keys, not user-facing product names. The Windows
installer removes the old `sports-replay.dll`/PDB/data directory. Manual ZIP
upgrades must remove the old DLL once before copying the new plugin so OBS cannot
load both modules. Runtime config and played-history loading migrate the old
`sports-replay` module config directory into the new one on first launch.

References to `Voodoo25/obs-sports-replay` remain because that is the actual
upstream repository name.
