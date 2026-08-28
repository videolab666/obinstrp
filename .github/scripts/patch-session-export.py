from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    (ROOT / path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


# Extend the existing packet-copy MP4 exporter with an optional AAC directory.
path = "src/sr-event-export.h"
text = read(path)
text = replace_once(
    text,
    "\tbool include_master_audio;\n};",
    "\tbool include_master_audio;\n\t/* Optional AAC segment directory. NULL keeps the existing session master-audio behavior. */\n\tconst char *audio_directory_override;\n};",
    "event export spec audio override",
)
write(path, text)

path = "src/sr-event-export.c"
text = read(path)
text = replace_once(
    text,
    "\tif (!sr_master_audio_catalog_scan(spec->session_dir, &cursor->segments, &cursor->segment_count) ||\n\t    !cursor->segment_count)\n\t\treturn false;",
    "\tconst bool scanned = spec->audio_directory_override && *spec->audio_directory_override\n\t\t\t\t     ? sr_audio_catalog_scan_directory(spec->audio_directory_override, &cursor->segments,\n\t\t\t\t\t\t\t       &cursor->segment_count)\n\t\t\t\t     : sr_master_audio_catalog_scan(spec->session_dir, &cursor->segments,\n\t\t\t\t\t\t\t    &cursor->segment_count);\n\tif (!scanned || !cursor->segment_count)\n\t\treturn false;",
    "event export audio catalog",
)
write(path, text)

# Wire two Session-level export buttons into the Storage / Session Manager panel.
path = "src/sr-session-panel.cpp"
text = read(path)
text = replace_once(
    text,
    '#include "sr-session.h"\n#include "sr-storage-manager.h"',
    '#include "sr-session.h"\n#include "sr-session-export.h"\n#include "sr-storage-manager.h"',
    "session panel include",
)
text = replace_once(
    text,
    '\t\tauto *clearTargetButton = new QPushButton(T("Session.ClearTarget"), this);\n\t\tauto *deleteButton = new QPushButton(T("Storage.DeleteSelected"), this);',
    '\t\tauto *clearTargetButton = new QPushButton(T("Session.ClearTarget"), this);\n\t\texportClipsButton = new QPushButton(T("Session.ExportClips"), this);\n\t\texportIsoButton = new QPushButton(T("Session.ExportIso"), this);\n\t\tauto *deleteButton = new QPushButton(T("Storage.DeleteSelected"), this);',
    "session panel export buttons",
)
text = replace_once(
    text,
    "\t\tsecondary->addWidget(refreshButton);\n\t\tsecondary->addWidget(clearTargetButton);\n\t\tsecondary->addStretch(1);",
    "\t\tsecondary->addWidget(refreshButton);\n\t\tsecondary->addWidget(clearTargetButton);\n\t\tsecondary->addWidget(exportClipsButton);\n\t\tsecondary->addWidget(exportIsoButton);\n\t\tsecondary->addStretch(1);",
    "session panel button layout",
)
text = replace_once(
    text,
    "\t\tconnect(clearTargetButton, &QPushButton::clicked, this, [this]() { clearTarget(); });\n\t\tconnect(deleteButton, &QPushButton::clicked, this, [this]() { deleteSelected(); });",
    "\t\tconnect(clearTargetButton, &QPushButton::clicked, this, [this]() { clearTarget(); });\n\t\tconnect(exportClipsButton, &QPushButton::clicked, this, [this]() { exportClips(); });\n\t\tconnect(exportIsoButton, &QPushButton::clicked, this, [this]() { exportIso(); });\n\t\tconnect(deleteButton, &QPushButton::clicked, this, [this]() { deleteSelected(); });",
    "session panel export connections",
)
text = replace_once(
    text,
    "\t\topenButton->setEnabled(one);\n\t\tresumeButton->setEnabled(one && !sr_session_recording_is_active());\n\t\trenameButton->setEnabled(one);",
    "\t\topenButton->setEnabled(one);\n\t\tresumeButton->setEnabled(one && !sr_session_recording_is_active());\n\t\trenameButton->setEnabled(one);\n\t\texportClipsButton->setEnabled(one);\n\t\texportIsoButton->setEnabled(one);",
    "session panel export enabled state",
)
text = replace_once(
    text,
    "\tvoid deleteSelected() { deleteSessions(selectedPaths(), false); }",
    "\tvoid exportClips()\n\t{\n\t\tconst QString path = singleSelectedPath();\n\t\tif (path.isEmpty())\n\t\t\treturn;\n\t\tconst QByteArray utf8 = path.toUtf8();\n\t\tsr_session_export_all_clips(this, utf8.constData());\n\t}\n\n\tvoid exportIso()\n\t{\n\t\tconst QString path = singleSelectedPath();\n\t\tif (path.isEmpty())\n\t\t\treturn;\n\t\tconst QByteArray utf8 = path.toUtf8();\n\t\tsr_session_export_iso(this, utf8.constData());\n\t}\n\n\tvoid deleteSelected() { deleteSessions(selectedPaths(), false); }",
    "session panel export methods",
)
text = replace_once(
    text,
    "\tQPushButton *renameButton = nullptr;\n\tQPushButton *returnToRecordingButton = nullptr;",
    "\tQPushButton *renameButton = nullptr;\n\tQPushButton *returnToRecordingButton = nullptr;\n\tQPushButton *exportClipsButton = nullptr;\n\tQPushButton *exportIsoButton = nullptr;",
    "session panel export members",
)
write(path, text)

# Compile the new controller.
path = "CMakeLists.txt"
text = read(path)
text = replace_once(
    text,
    "    src/sr-session-panel.cpp\n)",
    "    src/sr-session-panel.cpp\n    src/sr-session-export.cpp\n)",
    "CMake session export source",
)
write(path, text)

# Let the ordinary development CI validate this temporary feature branch.
path = ".github/workflows/validate-program-output.yml"
text = read(path)
text = replace_once(
    text,
    "      - feature/session-manager\n",
    "      - feature/session-manager\n      - feature/session-export\n",
    "development CI branch",
)
write(path, text)

strings = {
    "data/locale/en-US.ini": r'''
Session.ExportClips="Export clips…"
Session.ExportIso="Export ISO…"
Session.ExportTitle="Session Export"
Session.ExportFolder="Choose export destination folder"
Session.ExportPreparing="Preparing Session export…"
Session.ExportCancel="Cancel"
Session.ExportStopRecording="Stop recording this Session before exporting it. Session export uses a stable snapshot of finalized media."
Session.ExportCreateFolderFailed="Could not create the export folder"
Session.ExportDatabaseError="Could not read this Session's Event database"
Session.ExportNoEvents="This Session has no saved replay Events"
Session.ExportNoMedia="No exportable recorded media was found in this Session"
Session.ExportCancelled="Session export cancelled; incomplete temporary files were removed"
Session.ExportComplete="Export complete: %1 MP4 file(s) created, %2 item(s) skipped.\n\n%3"
Session.ExportPartial="Export finished with errors: %1 file(s) created, %2 failed, %3 skipped.\n\nFirst error: %4"
''',
    "data/locale/es-ES.ini": r'''
Session.ExportClips="Exportar clips…"
Session.ExportIso="Exportar ISO…"
Session.ExportTitle="Exportación de sesión"
Session.ExportFolder="Elegir carpeta de destino"
Session.ExportPreparing="Preparando exportación de la sesión…"
Session.ExportCancel="Cancelar"
Session.ExportStopRecording="Detenga la grabación de esta sesión antes de exportarla. La exportación usa una instantánea estable de los medios finalizados."
Session.ExportCreateFolderFailed="No se pudo crear la carpeta de exportación"
Session.ExportDatabaseError="No se pudo leer la base de datos de eventos de esta sesión"
Session.ExportNoEvents="Esta sesión no tiene eventos de replay guardados"
Session.ExportNoMedia="No se encontraron medios grabados exportables en esta sesión"
Session.ExportCancelled="Exportación cancelada; se eliminaron los archivos temporales incompletos"
Session.ExportComplete="Exportación completa: %1 archivo(s) MP4 creado(s), %2 elemento(s) omitido(s).\n\n%3"
Session.ExportPartial="La exportación terminó con errores: %1 archivo(s) creado(s), %2 fallidos, %3 omitidos.\n\nPrimer error: %4"
''',
}
for path, block in strings.items():
    text = read(path)
    if 'Session.ExportClips=' in text:
        raise RuntimeError(f"{path}: Session export strings already exist")
    if not text.endswith("\n"):
        text += "\n"
    text += block.lstrip("\n")
    write(path, text)
