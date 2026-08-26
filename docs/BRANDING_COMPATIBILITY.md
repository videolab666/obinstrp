# Pitel Instant Replay branding and compatibility

The user-facing product name is **Pitel Instant Replay**. OBS source names,
filter names, dock titles, settings, documentation, logs, release installers,
and GitHub Actions artifact names use this branding.

The internal module target and persistent identifiers deliberately retain
their original `sports-replay`, `sports_replay_*`, and `SportsReplay.*`
values. OBS stores those identifiers inside scene collections, hotkey
bindings, module configuration paths, and existing installations. Renaming
them would make an ordinary branding update appear to OBS as a different
plugin and could orphan existing sources or load two conflicting modules.

This compatibility layer is not displayed in the OBS interface. Packaged ZIP
contents can therefore still contain `sports-replay.dll` and the matching data
directory while the downloadable artifact and every operator-facing label are
named Pitel Instant Replay.
