from pathlib import Path
import json
import re

root = Path('.')


def rep(path, old, new, count=-1):
    p = root / path
    text = p.read_text(encoding='utf-8')
    if old not in text:
        raise SystemExit(f'missing anchor in {path}: {old[:80]!r}')
    p.write_text(text.replace(old, new, count), encoding='utf-8')


# Hard runtime namespace break from all earlier sports-replay builds.
rep('src/sr-capture.h', '#define SR_CAPTURE_ID "sports_replay_capture"',
    '#define SR_CAPTURE_ID "pitel_instant_replay_capture"')
rep('src/sr-capture.h', '#define SR_PLAYBACK_ID "sports_replay"',
    '#define SR_PLAYBACK_ID "pitel_instant_replay"')
rep('src/sr-event-output.h', '#define SR_EVENT_OUTPUT_ID "sports_replay_event_output"',
    '#define SR_EVENT_OUTPUT_ID "pitel_instant_replay_event_output"')

for name in ('src/plugin-main.c', 'src/playback-source.c'):
    p = root / name
    text = p.read_text(encoding='utf-8')
    if 'SportsReplay.' not in text:
        raise SystemExit(f'expected legacy hotkey IDs in {name}')
    p.write_text(text.replace('SportsReplay.', 'PitelInstantReplay.'), encoding='utf-8')

rep('src/sr-dock.cpp', 'SportsReplayDock', 'PitelInstantReplayDock')
rep('src/sr-dock.cpp', 'SportsReplayOperatorScroll', 'PitelInstantReplayOperatorScroll')
rep('src/sr-dock.cpp', 'sports_replay_dock', 'pitel_instant_replay_dock')
rep('src/sr-scene-tracker.c', 'holds a Sports\n * Replay', 'holds a Pitel Instant Replay\n * replay')

# No config/history import from the old module. Pitel owns only its own module config.
p = root / 'src/sr-config.c'
text = p.read_text(encoding='utf-8')
text, n = re.subn(
    r'\n\tbool migrated_legacy_config = false;\n\tif \(!data\) \{.*?\n\t\}\n\n',
    '\n', text, flags=re.S)
if n != 1:
    raise SystemExit(f'expected one legacy config migration block, got {n}')
text = text.replace('\n\tif (migrated_legacy_config)\n\t\tsave_locked();\n', '')
if 'sports-replay' in text:
    raise SystemExit('legacy sports-replay config token remains in sr-config.c')
p.write_text(text, encoding='utf-8')

p = root / 'src/sr-dock.cpp'
text = p.read_text(encoding='utf-8')
legacy_played = '''\n\t\tbool migratedLegacy = false;\n\t\tif (!data) {\n\t\t\tchar *legacyPath = obs_module_config_path("../sports-replay/played.json");\n\t\t\tif (legacyPath) {\n\t\t\t\tdata = obs_data_create_from_json_file(legacyPath);\n\t\t\t\tmigratedLegacy = data != nullptr;\n\t\t\t}\n\t\t\tbfree(legacyPath);\n\t\t}\n'''
if legacy_played not in text:
    raise SystemExit('missing legacy played-history migration block')
text = text.replace(legacy_played, '\n')
text = text.replace('\n\t\tif (migratedLegacy)\n\t\t\tsavePlayedPaths();', '')
for token in ('sports-replay', 'sports_replay', 'SportsReplay'):
    if token in text:
        raise SystemExit(f'legacy dock token remains: {token}')
p.write_text(text, encoding='utf-8')

# New canonical topology names do not reuse scenes/source names created by previous builds.
rep('src/sr-replay-setup.h', '#define SR_REPLAY_SETUP_SCENE_A "Pitel Replay A"',
    '#define SR_REPLAY_SETUP_SCENE_A "Pitel Instant Replay A"')
rep('src/sr-replay-setup.h', '#define SR_REPLAY_SETUP_SCENE_B "Pitel Replay B"',
    '#define SR_REPLAY_SETUP_SCENE_B "Pitel Instant Replay B"')
rep('src/sr-replay-setup.h', '#define SR_REPLAY_SETUP_OUTPUT_A "Pitel Instant Replay Event Output A"',
    '#define SR_REPLAY_SETUP_OUTPUT_A "Pitel Replay Output A"')
rep('src/sr-replay-setup.h', '#define SR_REPLAY_SETUP_OUTPUT_B "Pitel Instant Replay Event Output B"',
    '#define SR_REPLAY_SETUP_OUTPUT_B "Pitel Replay Output B"')
rep('src/sr-replay-setup.h', 'canonical Pitel Replay scene', 'canonical Pitel Instant Replay scene')

# Separate physical disk namespace too: previous Pitel-branded sports-replay builds used the parent folder.
rep('src/sr-config.c', 'dstr_cat(&d, "/Videos/Pitel Instant Replay");',
    'dstr_cat(&d, "/Videos/Pitel Instant Replay/Recorder");')

# Installer is standalone: it never deletes or mutates another plugin.
p = root / 'installer/pitel-instant-replay.iss'
text = p.read_text(encoding='utf-8')
text = text.replace('; Author: Systec - https://www.systecinformatica.com.ar',
                    '; Pitel Instant Replay standalone installer')
text = text.replace('#define MyPublisher "Systec"', '#define MyPublisher "videolab666"')
text = text.replace('#define MyURL "https://www.systecinformatica.com.ar"',
                    '#define MyURL "https://github.com/videolab666/obinstrp"')
text, n = re.subn(
    r'\n\[InstallDelete\]\n; Remove the legacy module before installing the renamed module\.\n'
    r'Type: files; Name: "\{app\}\\obs-plugins\\64bit\\sports-replay\.dll"\n'
    r'Type: files; Name: "\{app\}\\obs-plugins\\64bit\\sports-replay\.pdb"\n'
    r'Type: filesandordirs; Name: "\{app\}\\data\\obs-plugins\\sports-replay"\n',
    '\n', text)
if n != 1:
    raise SystemExit(f'expected one installer legacy-delete block, got {n}')
p.write_text(text, encoding='utf-8')

# Public project metadata and credits are owned by the standalone repo.
p = root / 'buildspec.json'
data = json.loads(p.read_text(encoding='utf-8'))
data['platformConfig']['macos']['bundleId'] = 'com.videolab666.pitel-instant-replay'
data['author'] = 'videolab666'
data['website'] = 'https://github.com/videolab666/obinstrp'
data['email'] = ''
p.write_text(json.dumps(data, indent=4) + '\n', encoding='utf-8')

rep('src/plugin-support.h', '#define PLUGIN_WEBSITE "https://www.systecinformatica.com.ar"',
    '#define PLUGIN_WEBSITE "https://github.com/videolab666/obinstrp"')
p = root / 'src/sr-credit.h'
text = p.read_text(encoding='utf-8')
text = text.replace(
    '/* Builds the "<a href="...">Pitel Instant Replay (version) by Systec</a>" credit\n'
    ' * line shown at the bottom of the plugin\'s windows/dialogs, into buf.',
    '/* Builds the Pitel Instant Replay project link shown at the bottom of the\n'
    ' * plugin\'s windows/dialogs, into buf.')
old_credit = ('snprintf(buf, size, "<a href=\\"%s\\">%s (%s) %s Systec</a>", PLUGIN_WEBSITE,\n'
              '\t\t obs_module_text("PitelInstantReplay"), PLUGIN_VERSION, obs_module_text("Credit.By"));')
new_credit = ('snprintf(buf, size, "<a href=\\"%s\\">%s (%s)</a>", PLUGIN_WEBSITE,\n'
              '\t\t obs_module_text("PitelInstantReplay"), PLUGIN_VERSION);')
if old_credit not in text:
    raise SystemExit('missing old credit template')
p.write_text(text.replace(old_credit, new_credit), encoding='utf-8')

rep('tools/srseg_inspect.py', 'Inspect Sports Replay .srseg/.sridx files',
    'Inspect Pitel Instant Replay .srseg/.sridx files')

# Replace compatibility documentation with an explicit hard-isolation contract.
(root / 'docs/BRANDING_COMPATIBILITY.md').unlink()
(root / 'docs/NAMESPACE_ISOLATION.md').write_text('''# Pitel Instant Replay namespace isolation

Pitel Instant Replay is intentionally isolated from earlier `sports-replay` builds.
There is no runtime compatibility bridge or automatic migration.

## Runtime namespace

- module/package: `pitel-instant-replay`
- capture source ID: `pitel_instant_replay_capture`
- playback source ID: `pitel_instant_replay`
- Event Output source ID: `pitel_instant_replay_event_output`
- frontend/source hotkeys: `PitelInstantReplay.*`
- dock ID: `pitel_instant_replay_dock`
- Qt dock object names: `PitelInstantReplayDock` / `PitelInstantReplayOperatorScroll`
- macOS bundle ID: `com.videolab666.pitel-instant-replay`
- module config directory: determined only by the `pitel-instant-replay` module name
- default recording root: `Videos/Pitel Instant Replay/Recorder`
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
''', encoding='utf-8')

# Keep legal provenance, but remove active fork/upstream workflow instructions.
(root / 'UPSTREAM.md').unlink()
(root / 'THIRD_PARTY_NOTICES.md').write_text('''# Third-party notices

Pitel Instant Replay contains code derived from the GPL-2.0-or-later project
`Voodoo25/obs-sports-replay` and subsequent modifications. This historical
attribution is retained for licensing/provenance only and does not define any
Pitel Instant Replay runtime identifier, package name, configuration namespace,
hotkey, dock ID or storage path.

Do not remove copyright or GPL notices from inherited files. See `LICENSE`.
''', encoding='utf-8')

# Obsolete upstream forum/release drafts are not part of the standalone product.
for name in ('docs/obs-forum-post.md', 'docs/obs-forum-post-bbcode.txt'):
    (root / name).unlink(missing_ok=True)

p = root / 'docs/VMIX_REPLAY_ROADMAP.md'
text = p.read_text(encoding='utf-8')
text = text.replace('**Upstream:** `Voodoo25/obs-sports-replay`  \n', '')
text = text.replace('Evolve the existing plugin rather than rewrite it.',
                    'Evolve the Pitel Instant Replay codebase without destabilizing proven replay paths.')
text, n = re.subn(
    r'## 28\. Upstream maintenance\n.*?## 29\. References\n\n- Upstream: https://github\.com/Voodoo25/obs-sports-replay\n',
    '## 28. Standalone project maintenance\n\n'
    'Keep Pitel Instant Replay runtime and packaging identifiers self-contained. '
    'Historical third-party provenance is documented only in `THIRD_PARTY_NOTICES.md`.\n\n'
    '## 29. References\n\n', text, flags=re.S)
if n != 1:
    raise SystemExit(f'expected one roadmap upstream section, got {n}')
p.write_text(text, encoding='utf-8')

p = root / 'README.md'
text = p.read_text(encoding='utf-8')
text, n = re.subn(
    r'## Support development\n.*?## Author\n\nDeveloped by \*\*Systec\*\*.*?\n\n'
    r'## License\n\nGPL-2\.0-or-later — Copyright \(C\) 2026 Systec .*?\n',
    '## Project\n\n'
    'Pitel Instant Replay is maintained in '
    '[`videolab666/obinstrp`](https://github.com/videolab666/obinstrp).\n\n'
    '## License\n\nGPL-2.0-or-later. See `LICENSE` and `THIRD_PARTY_NOTICES.md` for licensing '
    'and historical third-party provenance.\n',
    text, flags=re.S)
if n != 1:
    raise SystemExit(f'expected one README legacy publisher/support section, got {n}')
p.write_text(text, encoding='utf-8')

# Runtime/product trees must contain zero legacy identifiers. Legal provenance is docs-only.
for rel in ('src', 'data', 'installer', 'tools'):
    for p in (root / rel).rglob('*'):
        if not p.is_file():
            continue
        try:
            text = p.read_text(encoding='utf-8')
        except UnicodeDecodeError:
            continue
        for token in ('sports_replay', 'SportsReplay', 'sports-replay'):
            if token in text:
                raise SystemExit(f'legacy runtime token {token!r} remains in {p}')

for name in ('buildspec.json', 'installer/pitel-instant-replay.iss', 'src/plugin-support.h',
             'src/sr-credit.h', 'README.md'):
    text = (root / name).read_text(encoding='utf-8')
    if 'systecinformatica' in text or 'by Systec' in text:
        raise SystemExit(f'old public publisher metadata remains in {name}')

print('Pitel standalone namespace isolation patch complete')
