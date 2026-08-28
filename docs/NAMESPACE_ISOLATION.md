# Pitel Instant Replay namespace isolation

Pitel Instant Replay is intentionally isolated from earlier `sports-replay` builds.
There is no runtime compatibility bridge or automatic migration.

## Runtime namespace

- module/package: `pitel-instant-replay`
- capture source ID: `pitel_instant_replay_capture`
- Event Output source ID: `pitel_instant_replay_event_output`
- no standalone playback-source ID is registered; A/B replay is rendered through Event Output sources
- frontend/source hotkeys: `PitelInstantReplay.*`
- dock ID: `pitel_instant_replay_dock`
- Qt dock object names: `PitelInstantReplayDock` / `PitelInstantReplayOperatorScroll`
- macOS bundle ID: `com.videolab666.pitel-instant-replay`
- module config namespace: `pitel-instant-replay/standalone-v1/` (older module-root config files are ignored)
- default Session root: `<OBS module config>/standalone-v1/Sessions`
- canonical replay scenes: `Pitel Instant Replay A` / `Pitel Instant Replay B`

The installer installs only Pitel Instant Replay files and never deletes, edits or
imports files/configuration belonging to another plugin. Old and new modules can
therefore exist on the same OBS installation without sharing source IDs, hotkeys,
dock IDs, module configuration or default recording/session storage.

This is a deliberate breaking boundary: scene collections containing older source
IDs are not migrated. Use Replay Setup to create fresh Pitel Instant Replay A/B
scenes and attach fresh Pitel capture filters.

Historical third-party provenance required by the GPL is documented separately in
`THIRD_PARTY_NOTICES.md`; that notice is not part of the OBS runtime namespace.
