from pathlib import Path
import re

LEGACY_LOCALE_KEYS = {
    "PitelInstantReplay",
    "Duration",
    "CaptureSource",
    "NoSource",
    "Speed",
    "Backward",
    "AutoPlay",
    "CaptureNow",
    "EndAction",
    "EndAction.Freeze",
    "EndAction.Return",
    "EndAction.Loop",
    "EndAction.Camera",
    "IntroClip",
    "OutroClip",
    "SaveDir",
    "Dock.TabClips",
    "Dock.Folder",
    "Dock.OutputSource",
    "Dock.ReplayScene",
    "Dock.PickFolder",
    "Dock.Hint",
    "Credit.By",
    "MediaFilter",
    "Hotkey.Capture",
    "Hotkey.PlayPause",
    "Hotkey.Restart",
    "Hotkey.Faster",
    "Hotkey.Slower",
    "Hotkey.NormalSpeed",
    "Hotkey.HalfSpeed",
    "Hotkey.QuarterSpeed",
    "Hotkey.ReverseToggle",
    "Hotkey.PlayLast",
    "Hotkey.SendToProgram",
    "Hotkey.CaptureOnly",
    "RunMuted",
}


def clean_locale(path: str, description: str, disk_description: str, tooltip: str) -> None:
    file = Path(path)
    output = []
    for line in file.read_text(encoding="utf-8").splitlines():
        key = line.split("=", 1)[0].strip() if "=" in line else ""
        if key in LEGACY_LOCALE_KEYS:
            continue
        if key == "Description":
            line = f'Description="{description}"'
        elif key == "DiskRecording.Description":
            line = f'DiskRecording.Description="{disk_description}"'
        elif key == "EventDock.Performance.Tooltip":
            line = f'EventDock.Performance.Tooltip="{tooltip}"'
        output.append(line)
    file.write_text("\n".join(output) + "\n", encoding="utf-8")


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"expected exactly one {label}, found {text.count(old)}")
    return text.replace(old, new)


clean_locale(
    "data/locale/en-US.ini",
    "Professional disk-based instant replay for OBS Studio: continuous ISO/PROGRAM recording, Events, multi-angle A/B replay, slow motion and Session management.",
    "Continuously write this camera to crash-recoverable Session segments while global replay recording is active. Recording is controlled from the Pitel Instant Replay operator dock.",
    "Encoder submit: avg %1 ms, last %2 ms; finalized segments %3; queue peak %4",
)
clean_locale(
    "data/locale/es-ES.ini",
    "Repetición instantánea profesional basada en disco para OBS Studio: grabación continua ISO/PROGRAM, Eventos, replay multicámara A/B, cámara lenta y gestión de sesiones.",
    "Graba esta cámara continuamente en segmentos recuperables de la sesión mientras la grabación global de replay está activa. La grabación se controla desde el panel de operador de Pitel Instant Replay.",
    "Envío al codificador: prom. %1 ms, último %2 ms; segmentos finalizados %3; pico de cola %4",
)

file = Path("src/sr-config.h")
text = file.read_text(encoding="utf-8")
text = replace_once(text, "#define SR_CONFIG_SCHEMA_VERSION 8", "#define SR_CONFIG_SCHEMA_VERSION 9", "config schema")
text, count = re.subn(
    r"/\* Deprecated compatibility accessors for profiles created by the pre-Session\n \* loose-MP4 replay implementation\. New UI and recording code use session_root\. \*/\nchar \*sr_config_get_save_dir\(void\);\nvoid sr_config_set_save_dir\(const char \*save_dir\);\n\n",
    "",
    text,
)
if count != 1:
    raise RuntimeError(f"expected one legacy save-dir declaration block, found {count}")
file.write_text(text, encoding="utf-8")

file = Path("src/sr-config.c")
text = file.read_text(encoding="utf-8")
for old in (
    "static char *g_legacy_save_dir;\n",
    "\tbfree(g_legacy_save_dir);\n",
    "\tg_legacy_save_dir = NULL;\n",
    '\tobs_data_set_string(json, "save_dir", g_legacy_save_dir ? g_legacy_save_dir : "");\n',
    '\tobs_data_set_bool(json, "session_root_follows_save_dir", false);\n',
    "\tg_legacy_save_dir = copy_string(legacy_save);\n",
):
    text = replace_once(text, old, "", old.strip())
text, count = re.subn(
    r"\nchar \*sr_config_get_save_dir\(void\)\n\{.*?\n\}\n\nvoid sr_config_set_save_dir\(const char \*save_dir\)\n\{.*?\n\}\n",
    "\n",
    text,
    flags=re.S,
)
if count != 1:
    raise RuntimeError(f"expected one legacy save-dir implementation block, found {count}")
file.write_text(text, encoding="utf-8")

file = Path("src/sr-capture.h")
text = file.read_text(encoding="utf-8")
text, count = re.subn(
    r"/\* Migration-only identifier for old scene collections\..*?\*/\n#define SR_PLAYBACK_ID \"pitel_instant_replay\"\n",
    "",
    text,
    flags=re.S,
)
if count != 1:
    raise RuntimeError(f"expected one playback migration define, found {count}")
text = replace_once(
    text,
    "\t/* Kept for UI ABI compatibility. The legacy capture RAM ring was removed;\n\t * therefore this value is always zero. */\n\tuint64_t ram_bytes;\n",
    "",
    "RAM performance field",
)
file.write_text(text, encoding="utf-8")

file = Path("src/sr-replay-setup.c")
text = file.read_text(encoding="utf-8")
text = replace_once(
    text,
    "strcmp(id, SR_PLAYBACK_ID) == 0",
    'strcmp(id, "pitel_instant_replay") == 0',
    "legacy playback migration filter",
)
file.write_text(text, encoding="utf-8")

file = Path("src/pir-capture-filter.c")
text = file.read_text(encoding="utf-8")
text = replace_once(text, "\tperformance.ram_bytes = 0;\n", "", "RAM performance assignment")
file.write_text(text, encoding="utf-8")

file = Path("src/sr-event-dock.cpp")
text = file.read_text(encoding="utf-8")
text = replace_once(
    text,
    "\t\t\t\t\t\t  .arg((double)entry.ram_bytes / (1024.0 * 1024.0), 0, 'f', 1)\n",
    "",
    "RAM performance tooltip argument",
)
file.write_text(text, encoding="utf-8")

file = Path("src/sr-segment-writer.h")
text = file.read_text(encoding="utf-8")
text = replace_once(
    text,
    "/* Creates a per-camera asynchronous writer. Packets passed to push are cloned,\n * so the caller retains ownership and may immediately pass the original to the\n * legacy RAM replay buffer. */",
    "/* Creates a per-camera asynchronous writer. Packets passed to push are cloned,\n * so the caller retains ownership and may immediately release or reuse the original. */",
    "legacy RAM writer comment",
)
file.write_text(text, encoding="utf-8")

for name in ("src/sr-capture-session.c", "src/sr-audio-session.c"):
    file = Path(name)
    text = file.read_text(encoding="utf-8")
    if "capture-filter.c" not in text:
        raise RuntimeError(f"expected capture-filter.c comment in {name}")
    file.write_text(text.replace("capture-filter.c", "pir-capture-filter.c"), encoding="utf-8")
