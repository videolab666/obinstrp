from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one {label}, found {count}")
    return text.replace(old, new)


# The plugin is pre-release. Do not migrate the obsolete loose-MP4 save_dir
# into Session storage; only the current session_root setting is recognized.
file = Path("src/sr-config.c")
text = file.read_text(encoding="utf-8")
text = replace_once(text, "#include <util/dstr.h>\n", "", "obsolete dstr include")
start = text.index("static char *join_sessions(")
end = text.index("static uint64_t positive_or_default", start)
text = text[:start] + text[end:]
old = '''\tconst char *legacy_save = json ? obs_data_get_string(json, "save_dir") : "";\n\tconst char *saved_session = json ? obs_data_get_string(json, "session_root") : "";\n\tif (saved_session && *saved_session)\n\t\tg_session_root = copy_string(saved_session);\n\telse if (legacy_save && *legacy_save)\n\t\tg_session_root = join_sessions(legacy_save);\n\telse\n\t\tg_session_root = default_storage_root();\n'''
new = '''\tconst char *saved_session = json ? obs_data_get_string(json, "session_root") : "";\n\tif (saved_session && *saved_session)\n\t\tg_session_root = copy_string(saved_session);\n\telse\n\t\tg_session_root = default_storage_root();\n'''
text = replace_once(text, old, new, "legacy save_dir migration block")
file.write_text(text, encoding="utf-8")

# Old Pitel playback-source scene objects are deliberately unsupported.
file = Path("src/sr-replay-setup.c")
text = file.read_text(encoding="utf-8")
text = replace_once(
    text,
    ' || strcmp(id, "pitel_instant_replay") == 0',
    "",
    "old playback source id filter",
)
text = text.replace(
    "even if an older Event Output\n\t * with the same bus is still referenced by another user scene.",
    "even if another Event Output with the same bus is referenced by a user scene.",
)
file.write_text(text, encoding="utf-8")

# Hard gate: no source-level compatibility hooks for the pre-release RAM/MP4
# implementation may survive this pass.
checks = {
    'src/sr-config.c': ('"save_dir"', 'join_sessions', 'legacy_save'),
    'src/sr-replay-setup.c': ('"pitel_instant_replay"',),
}
for name, forbidden in checks.items():
    text = Path(name).read_text(encoding="utf-8")
    for token in forbidden:
        if token in text:
            raise RuntimeError(f"obsolete compatibility token {token!r} remains in {name}")
