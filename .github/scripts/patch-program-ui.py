from pathlib import Path

ROOT = Path('.')

def load(path):
    return (ROOT / path).read_text(encoding='utf-8')

def save(path, text):
    (ROOT / path).write_text(text, encoding='utf-8')

def replace_exact(text, old, new, *, count=1, label='replacement'):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f'{label}: expected {count} occurrence(s), found {actual}')
    return text.replace(old, new, count)

# Replay Setup UI and Program-aware preflight/status/export.
p = 'src/sr-event-dock.cpp'
s = load(p)
s = replace_exact(s, '#include "sr-camera-list.h"\n',
                  '#include "sr-camera-list.h"\n#include "sr-camera-identity.h"\n', label='dock identity include')
s = replace_exact(s, '#include "sr-replay-playlist.h"\n',
                  '#include "sr-replay-playlist.h"\n#include "sr-program-recorder.h"\n', label='dock Program include')
s = replace_exact(s, '#include <QComboBox>\n', '#include <QCheckBox>\n#include <QComboBox>\n', label='dock checkbox include')
s = replace_exact(s,
                  '\t\tconst bool needsAB = eventTransitionConfigured();\n'
                  '\t\tconst bool healthy = snapshot.enabled_capture_source_count > 0 &&\n'
                  '\t\t\t\t     (!needsAB || snapshot.event_transition_ready);\n'
                  '\t\tif (!snapshot.enabled_capture_source_count) {',
                  '\t\tconst bool needsAB = eventTransitionConfigured();\n'
                  '\t\tconst size_t selectedSources = snapshot.enabled_capture_source_count +\n'
                  '\t\t\t\t\t       (snapshot.program_output_enabled ? 1 : 0);\n'
                  '\t\tconst bool healthy = selectedSources > 0 && (!needsAB || snapshot.event_transition_ready);\n'
                  '\t\tif (!selectedSources) {', label='dock Program health')
s = replace_exact(s,
                  '\t\t\tsetupButton->setText(QStringLiteral("%1 %2 cam · %3")\n'
                  '\t\t\t\t\t\t     .arg(healthy ? QStringLiteral("✓") : QStringLiteral("⚠"))\n'
                  '\t\t\t\t\t\t     .arg(snapshot.enabled_capture_source_count)\n'
                  '\t\t\t\t\t\t     .arg(mode));',
                  '\t\t\tsetupButton->setText(QStringLiteral("%1 %2 src · %3")\n'
                  '\t\t\t\t\t\t     .arg(healthy ? QStringLiteral("✓") : QStringLiteral("⚠"))\n'
                  '\t\t\t\t\t\t     .arg(selectedSources)\n'
                  '\t\t\t\t\t\t     .arg(mode));', label='dock source count')
s = replace_exact(s,
                  '\t\tlayout->addWidget(hint);\n\n\t\tauto *sources = new QTableWidget(&dialog);',
                  '\t\tlayout->addWidget(hint);\n\n'
                  '\t\tauto *programOutput = new QCheckBox(T("EventDock.Setup.ProgramOutput"), &dialog);\n'
                  '\t\tprogramOutput->setChecked(sr_program_recorder_selected());\n'
                  '\t\tprogramOutput->setEnabled(sr_program_recorder_supported());\n'
                  '\t\tprogramOutput->setToolTip(sr_program_recorder_supported() ? T("EventDock.Setup.ProgramOutput.Tooltip")\n'
                  '\t\t\t\t\t\t\t\t : T("EventDock.Setup.ProgramOutput.Unsupported"));\n'
                  '\t\tlayout->addWidget(programOutput);\n\n'
                  '\t\tauto *sources = new QTableWidget(&dialog);', label='dock Program checkbox')
s = replace_exact(s,
                  '\t\t\tsummary->setText(T("EventDock.Setup.Summary")\n'
                  '\t\t\t\t\t\t .arg(snapshot.enabled_capture_source_count)\n'
                  '\t\t\t\t\t\t .arg(snapshot.compatible_source_count)\n'
                  '\t\t\t\t\t\t .arg(sceneA)\n'
                  '\t\t\t\t\t\t .arg(sceneB)\n'
                  '\t\t\t\t\t\t .arg(snapshot.event_transition_ready ? T("EventDock.Setup.ReadyAB")\n'
                  '\t\t\t\t\t\t\t\t\t\t      : T("EventDock.Setup.CutOnly")));',
                  '\t\t\tconst QString programState = !snapshot.program_output_supported\n'
                  '\t\t\t\t\t\t\t ? T("EventDock.Setup.ProgramUnsupportedShort")\n'
                  '\t\t\t\t\t\t\t : snapshot.program_output_enabled ? T("EventDock.Setup.ProgramOn")\n'
                  '\t\t\t\t\t\t\t\t\t\t   : T("EventDock.Setup.ProgramOff");\n'
                  '\t\t\tsummary->setText(T("EventDock.Setup.Summary")\n'
                  '\t\t\t\t\t\t .arg(snapshot.enabled_capture_source_count)\n'
                  '\t\t\t\t\t\t .arg(snapshot.compatible_source_count)\n'
                  '\t\t\t\t\t\t .arg(sceneA)\n'
                  '\t\t\t\t\t\t .arg(sceneB)\n'
                  '\t\t\t\t\t\t .arg(snapshot.event_transition_ready ? T("EventDock.Setup.ReadyAB")\n'
                  '\t\t\t\t\t\t\t\t\t\t      : T("EventDock.Setup.CutOnly")) +\n'
                  '\t\t\t\t\t QStringLiteral(" · PROGRAM: ") + programState);', label='dock Program summary')
s = replace_exact(s,
                  '\t\t\tint changes = 0;\n\t\t\tint failures = 0;\n\t\t\tfor (int row = 0; row < sources->rowCount(); row++) {',
                  '\t\t\tint changes = 0;\n\t\t\tint failures = 0;\n'
                  '\t\t\tconst bool desiredProgram = programOutput->isChecked();\n'
                  '\t\t\tif (desiredProgram != sr_program_recorder_selected()) {\n'
                  '\t\t\t\tif (sr_replay_setup_set_program_output(desiredProgram))\n\t\t\t\t\tchanges++;\n'
                  '\t\t\t\telse\n\t\t\t\t\tfailures++;\n\t\t\t}\n'
                  '\t\t\tfor (int row = 0; row < sources->rowCount(); row++) {', label='dock apply Program')
s = replace_exact(s,
                  '\t\tconst bool ready = sr_replay_setup_get_snapshot(&finalSnapshot) &&\n'
                  '\t\t\t\t   finalSnapshot.enabled_capture_source_count > 0;',
                  '\t\tconst bool ready = sr_replay_setup_get_snapshot(&finalSnapshot) &&\n'
                  '\t\t\t\t   (finalSnapshot.enabled_capture_source_count > 0 || finalSnapshot.program_output_enabled);',
                  label='dock Program ready')
s = replace_exact(s, '\t\tbool haveCapture = snapshot.enabled_capture_source_count > 0;\n',
                  '\t\tbool haveCapture = snapshot.enabled_capture_source_count > 0 || snapshot.program_output_enabled;\n',
                  label='dock Program preflight')
s = replace_exact(s,
                  '\t\t\tif (prompt.clickedButton() != openSetup || !openReplaySetup())\n\t\t\t\treturn false;\n\t\t\thaveCapture = true;\n',
                  '\t\t\tif (prompt.clickedButton() != openSetup || !openReplaySetup())\n\t\t\t\treturn false;\n'
                  '\t\t\tsr_replay_setup_snapshot configured = {};\n'
                  '\t\t\thaveCapture = sr_replay_setup_get_snapshot(&configured) &&\n'
                  '\t\t\t\t      (configured.enabled_capture_source_count > 0 || configured.program_output_enabled);\n'
                  '\t\t\tsr_replay_setup_free_snapshot(&configured);\n', label='dock Program recheck')
s = replace_exact(s, '\t\ttask.includeMasterAudio = event.audio_mode == SR_EVENT_AUDIO_MASTER;\n',
                  '\t\ttask.includeMasterAudio = event.audio_mode == SR_EVENT_AUDIO_MASTER ||\n'
                  '\t\t\t\t\t  sr_camera_is_program_name(task.camera.c_str());\n',
                  label='Program export master audio')
save(p, s)

# English UI strings: camera counters become recorder-source counters where PROGRAM participates.
p = 'data/locale/en-US.ini'
s = load(p)
s = replace_exact(s,
                  'EventDock.Setup.CameraHint="Choose the asynchronous video sources that should be replay cameras. Apply Cameras only adds/removes the Pitel Instant Replay Capture filter; all other source filters and scene items are left untouched. On first setup compatible video sources are preselected for review."\n',
                  'EventDock.Setup.CameraHint="Choose asynchronous video sources for replay ISO cameras. PROGRAM records the final composited OBS output as an additional replay angle."\n'
                  'EventDock.Setup.ProgramOutput="Record PROGRAM output as replay angle"\n'
                  'EventDock.Setup.ProgramOutput.Tooltip="Record the final OBS Program/PGM composition (scenes, transitions, graphics and replays) into the same Event/segment system. Windows D3D11 uses a GPU-resident NVENC/AMF path."\n'
                  'EventDock.Setup.ProgramOutput.Unsupported="PROGRAM replay recording currently requires Windows with the OBS D3D11 renderer and NVENC or AMF."\n'
                  'EventDock.Setup.ProgramOn="On"\nEventDock.Setup.ProgramOff="Off"\nEventDock.Setup.ProgramUnsupportedShort="Unavailable"\n',
                  label='English Program strings')
repls = [
('EventDock.RecordNoCameras="No cameras have a Pitel Instant Replay Capture filter"','EventDock.RecordNoCameras="No replay recording sources are configured"'),
('EventDock.RecordStartRequested="Recording start requested for %1 camera(s)"','EventDock.RecordStartRequested="Recording start requested for %1 source(s)"'),
('EventDock.RecordStopped="Recording stopped on %1 camera(s)"','EventDock.RecordStopped="Recording stopped on %1 source(s)"'),
('EventDock.RecordIdle="STOPPED — %1 camera(s) configured"','EventDock.RecordIdle="STOPPED — %1 replay source(s) configured"'),
('EventDock.RecordStarting="STARTING — %1/%2 camera recorder(s) active"','EventDock.RecordStarting="STARTING — %1/%2 replay recorder(s) active"'),
('EventDock.RecordActive="● REC — %1/%2 camera(s), %3 packets, %4 MB"','EventDock.RecordActive="● REC — %1/%2 source(s), %3 packets, %4 MB"'),
('EventDock.RecordReserve="RECORDING BLOCKED — disk-space reserve reached on %1/%2 camera(s); lower the reserve or choose another disk"','EventDock.RecordReserve="RECORDING BLOCKED — disk-space reserve reached on %1/%2 source(s); lower the reserve or choose another disk"'),
('EventDock.RecordError="RECORDING ERROR — %1/%2 camera(s); check the OBS log"','EventDock.RecordError="RECORDING ERROR — %1/%2 source(s); check the OBS log"'),
('EventDock.Performance.Camera="Camera"','EventDock.Performance.Camera="Source"'),
('EventDock.Setup.NoCapturePreflight="No enabled Pitel Instant Replay Capture filters are configured. Open Replay Setup, choose the camera sources and apply them before recording."','EventDock.Setup.NoCapturePreflight="No replay recording sources are configured. Open Replay Setup and enable one or more ISO cameras and/or PROGRAM output before recording."'),
]
for old, new in repls:
    s = replace_exact(s, old, new, label='English recorder wording')
save(p, s)

# Spanish parity for the same controls.
p = 'data/locale/es-ES.ini'
s = load(p)
s = replace_exact(s,
                  'EventDock.Setup.CameraHint="Elija las fuentes de vídeo asíncronas que serán cámaras de replay. Aplicar cámaras solo agrega o quita el filtro Pitel Instant Replay Capture; los demás filtros y elementos de escena no se modifican."\n',
                  'EventDock.Setup.CameraHint="Elija las fuentes asíncronas para cámaras ISO de replay. PROGRAM graba la salida final compuesta de OBS como un ángulo adicional."\n'
                  'EventDock.Setup.ProgramOutput="Grabar salida PROGRAM como ángulo de replay"\n'
                  'EventDock.Setup.ProgramOutput.Tooltip="Graba la composición final Program/PGM de OBS (escenas, transiciones, gráficos y replays) en el mismo sistema de Events/segmentos. Windows D3D11 usa NVENC/AMF sin readback normal a CPU."\n'
                  'EventDock.Setup.ProgramOutput.Unsupported="La grabación PROGRAM requiere Windows, renderer D3D11 de OBS y NVENC o AMF."\n'
                  'EventDock.Setup.ProgramOn="Activo"\nEventDock.Setup.ProgramOff="Inactivo"\nEventDock.Setup.ProgramUnsupportedShort="No disponible"\n',
                  label='Spanish Program strings')
repls = [
('EventDock.RecordNoCameras="Ninguna cámara tiene el filtro Pitel Instant Replay Capture"','EventDock.RecordNoCameras="No hay fuentes de grabación de replay configuradas"'),
('EventDock.RecordStartRequested="Inicio de grabación solicitado para %1 cámara(s)"','EventDock.RecordStartRequested="Inicio de grabación solicitado para %1 fuente(s)"'),
('EventDock.RecordStopped="Grabación detenida en %1 cámara(s)"','EventDock.RecordStopped="Grabación detenida en %1 fuente(s)"'),
('EventDock.RecordIdle="DETENIDO — %1 cámara(s) configurada(s)"','EventDock.RecordIdle="DETENIDO — %1 fuente(s) de replay configurada(s)"'),
('EventDock.RecordActive="● REC — %1/%2 cámara(s), %3 paquetes, %4 MB"','EventDock.RecordActive="● REC — %1/%2 fuente(s), %3 paquetes, %4 MB"'),
('EventDock.RecordReserve="GRABACIÓN BLOQUEADA — reserva de disco alcanzada en %1/%2 cámara(s)"','EventDock.RecordReserve="GRABACIÓN BLOQUEADA — reserva de disco alcanzada en %1/%2 fuente(s)"'),
('EventDock.RecordError="ERROR DE GRABACIÓN — %1/%2 cámara(s); revisá el registro de OBS"','EventDock.RecordError="ERROR DE GRABACIÓN — %1/%2 fuente(s); revisá el registro de OBS"'),
('EventDock.Performance.Camera="Cámara"','EventDock.Performance.Camera="Fuente"'),
('EventDock.Setup.NoCapturePreflight="No hay filtros Pitel Instant Replay Capture activos. Abra Replay Setup, elija cámaras y aplique la configuración."','EventDock.Setup.NoCapturePreflight="No hay fuentes de replay configuradas. Abra Replay Setup y active cámaras ISO y/o la salida PROGRAM antes de grabar."'),
]
for old, new in repls:
    s = replace_exact(s, old, new, label='Spanish recorder wording')
save(p, s)

# User-facing feature note.
p = 'README.md'
s = load(p)
needle = '8. Open the **Storage** tab to see every replay session and its size, inspect the\n   last automatic-cleanup result, or permanently delete a closed session. The\n   active session is protected from manual deletion.\n'
s = replace_exact(s, needle, needle + '9. Replay Setup can also record **PROGRAM**: the final composited OBS Program/PGM output becomes a manual replay angle alongside ISO cameras. On Windows/D3D11 it stays GPU-resident through NVENC/AMF; PROGRAM is intentionally skipped by automatic **Play Each Angle** tours.\n', label='README Program note')
save(p, s)

print('Program UI/docs patch OK')
