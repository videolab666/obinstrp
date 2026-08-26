from pathlib import Path

root = Path('.')

# Isolate configuration from all earlier builds that used config.json directly
# in the pitel-instant-replay module config directory.
p = root / 'src/sr-config.c'
text = p.read_text(encoding='utf-8')
text = text.replace('obs_module_config_path("replays")', 'obs_module_config_path("standalone-v1/replays")')
text = text.replace('obs_module_config_path("")', 'obs_module_config_path("standalone-v1")')
text = text.replace('obs_module_config_path("config.json")', 'obs_module_config_path("standalone-v1/config.json")')
text = text.replace('/* Default location when the user hasn\'t chosen one: <Videos>/Pitel Instant Replay,',
                    '/* Default location when the user hasn\'t chosen one: <Videos>/Pitel Instant Replay/Recorder,')
# ASCII apostrophe variant in current source.
text = text.replace('/* Default location when the user hasn\'t chosen one: <Videos>/Pitel Instant Replay,',
                    '/* Default location when the user hasn\'t chosen one: <Videos>/Pitel Instant Replay/Recorder,')
text = text.replace("/* Default location when the user hasn't chosen one: <Videos>/Pitel Instant Replay,",
                    "/* Default location when the user hasn't chosen one: <Videos>/Pitel Instant Replay/Recorder,")
if 'obs_module_config_path("config.json")' in text or 'obs_module_config_path("")' in text:
    raise SystemExit('old module-root config path remains in sr-config.c')
if text.count('standalone-v1/config.json') != 2:
    raise SystemExit('expected exactly two standalone config.json references')
p.write_text(text, encoding='utf-8')

p = root / 'src/sr-dock.cpp'
text = p.read_text(encoding='utf-8')
text = text.replace('obs_module_config_path("played.json")', 'obs_module_config_path("standalone-v1/played.json")')
text = text.replace('obs_module_config_path("")', 'obs_module_config_path("standalone-v1")')
if 'obs_module_config_path("played.json")' in text:
    raise SystemExit('old module-root played history path remains')
if text.count('standalone-v1/played.json') != 1:
    raise SystemExit('expected one standalone played-history path')
p.write_text(text, encoding='utf-8')

p = root / 'docs/NAMESPACE_ISOLATION.md'
text = p.read_text(encoding='utf-8')
text = text.replace('- module config directory: determined only by the `pitel-instant-replay` module name',
                    '- module config namespace: `pitel-instant-replay/standalone-v1/` (older module-root config files are ignored)')
p.write_text(text, encoding='utf-8')

print('standalone-v1 config namespace applied')
