from pathlib import Path
import re


def read(path):
    return Path(path).read_text(encoding="utf-8")


def write(path, text):
    Path(path).write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one match, got {count}")
    return text.replace(old, new, 1)


def sub_once(text, pattern, repl, label):
    text, count = re.subn(pattern, repl, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{label}: expected exactly one regex match, got {count}")
    return text


# ---------------------------------------------------------------------------
# Capture status: track a real runtime recording duration and never restore a
# saved REC=true state when an OBS scene collection is loaded.
# ---------------------------------------------------------------------------
path = "src/sr-capture.h"
text = read(path)
text = replace_once(
    text,
    "\tuint64_t packets_written;\n\tuint64_t bytes_written;\n};",
    "\tuint64_t packets_written;\n\tuint64_t bytes_written;\n\tuint64_t recording_duration_ns;\n};",
    "sr-capture summary duration",
)
write(path, text)

path = "src/capture-filter.c"
text = read(path)
text = replace_once(
    text,
    "\tbool master_audio_acquired;\n\n\t/* Format the current encoder was opened with.",
    "\tbool master_audio_acquired;\n\tuint64_t recording_start_ns;\n\n\t/* Format the current encoder was opened with.",
    "capture start member",
)
text = replace_once(
    text,
    "\tstruct sr_capture_recording_summary status = {\n\t\t.camera_count = 1,\n\t\t.requested_count = c->disk_recording ? 1 : 0,\n\t\t.active_count = c->writer ? 1 : 0,\n\t\t.failed_count = (c->writer_failed || c->encoder_failed) ? 1 : 0,\n\t};",
    "\tconst uint64_t video_now = obs_get_video_frame_time();\n\tstruct sr_capture_recording_summary status = {\n\t\t.camera_count = 1,\n\t\t.requested_count = c->disk_recording ? 1 : 0,\n\t\t.active_count = c->writer ? 1 : 0,\n\t\t.failed_count = (c->writer_failed || c->encoder_failed) ? 1 : 0,\n\t\t.recording_duration_ns = c->disk_recording && c->recording_start_ns && video_now >= c->recording_start_ns\n\t\t\t\t\t ? video_now - c->recording_start_ns\n\t\t\t\t\t : 0,\n\t};",
    "capture publish duration",
)
text = replace_once(
    text,
    "\t\tif (!disk_recording)\n\t\t\tc->restart_writer = true;\n\t\tc->disk_recording = disk_recording;\n\t\tc->writer_failed = false;\n\t\tif (disk_recording)\n\t\t\tset_parent_showing_hold(c, true);",
    "\t\tif (!disk_recording)\n\t\t\tc->restart_writer = true;\n\t\tc->disk_recording = disk_recording;\n\t\tc->recording_start_ns = disk_recording ? obs_get_video_frame_time() : 0;\n\t\tc->writer_failed = false;\n\t\tif (disk_recording)\n\t\t\tset_parent_showing_hold(c, true);",
    "capture start transition",
)
text = replace_once(
    text,
    "\tc->gop_ms = SR_GOP_500MS;\n\tsr_capture_update(c, settings);",
    "\tc->gop_ms = SR_GOP_500MS;\n\n\t/* REC is a runtime operator action, not a scene-collection preference.\n\t * OBS persists filter settings, so a previous shutdown can leave this\n\t * flag true in the collection. Clear it before the first update so merely\n\t * opening OBS can never start replay recording. */\n\tobs_data_set_bool(settings, S_DISK_RECORDING, false);\n\tsr_capture_update(c, settings);",
    "capture startup REC reset",
)
text = replace_once(
    text,
    "\t\tctx->summary->packets_written += status.packets_written;\n\t\tctx->summary->bytes_written += status.bytes_written;",
    "\t\tctx->summary->packets_written += status.packets_written;\n\t\tctx->summary->bytes_written += status.bytes_written;\n\t\tif (status.recording_duration_ns > ctx->summary->recording_duration_ns)\n\t\t\tctx->summary->recording_duration_ns = status.recording_duration_ns;",
    "capture aggregate duration",
)
write(path, text)

path = "src/sr-program-recorder.c"
text = read(path)
text = replace_once(
    text,
    "\tuint64_t encode_calls;\n\tuint64_t encode_time_ns_total;",
    "\tuint64_t recording_start_ns;\n\tuint64_t encode_calls;\n\tuint64_t encode_time_ns_total;",
    "program start member",
)
text = replace_once(
    text,
    "\tpthread_mutex_lock(&g_program.mutex);\n\tg_program.recording_requested = enabled;\n\tif (enabled) {\n\t\tg_program.encoder_failed = false;\n\t\tg_program.writer_failed = false;\n\t} else if (!g_program.callback_registered) {\n\t\trelease_resources_locked(&g_program);\n\t}",
    "\tpthread_mutex_lock(&g_program.mutex);\n\tconst bool was_requested = g_program.recording_requested;\n\tg_program.recording_requested = enabled;\n\tif (enabled) {\n\t\tif (!was_requested)\n\t\t\tg_program.recording_start_ns = obs_get_video_frame_time();\n\t\tg_program.encoder_failed = false;\n\t\tg_program.writer_failed = false;\n\t} else {\n\t\tg_program.recording_start_ns = 0;\n\t\tif (!g_program.callback_registered)\n\t\t\trelease_resources_locked(&g_program);\n\t}",
    "program recording transition",
)
text = replace_once(
    text,
    "\tif (g_program.writer) {\n\t\tstruct sr_segment_writer_stats stats = {0};",
    "\tif (g_program.recording_requested && g_program.recording_start_ns) {\n\t\tconst uint64_t now = obs_get_video_frame_time();\n\t\tif (now >= g_program.recording_start_ns) {\n\t\t\tconst uint64_t duration = now - g_program.recording_start_ns;\n\t\t\tif (duration > summary->recording_duration_ns)\n\t\t\t\tsummary->recording_duration_ns = duration;\n\t\t}\n\t}\n\tif (g_program.writer) {\n\t\tstruct sr_segment_writer_stats stats = {0};",
    "program summary duration",
)
write(path, text)

# ---------------------------------------------------------------------------
# Explicitly clear any persisted REC intent after OBS finishes loading (and
# after switching scene collections). Source selection remains persistent.
# ---------------------------------------------------------------------------
path = "src/plugin-main.c"
text = read(path)
text = replace_once(
    text,
    "#include <obs-module.h>\n#include <plugin-support.h>",
    "#include <obs-module.h>\n#include <obs-frontend-api.h>\n#include <plugin-support.h>",
    "frontend include",
)
text = replace_once(
    text,
    '#include "sr-camera-list.h"\n#include "sr-config.h"',
    '#include "sr-camera-list.h"\n#include "sr-capture.h"\n#include "sr-config.h"',
    "capture include",
)
marker = "\nbool obs_module_load(void)\n{"
callback = r'''

static void frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING && event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		return;

	size_t source_count = 0;
	sr_capture_set_all_disk_recording(false, &source_count);
	obs_log(LOG_INFO,
		"Pitel Instant Replay: replay source selection restored, REC state reset to STOPPED for %zu source(s)",
		source_count);
}
'''
text = replace_once(text, marker, callback + marker, "frontend callback")
text = replace_once(
    text,
    "void obs_module_post_load(void)\n{\n\tsr_scene_tracker_start();",
    "void obs_module_post_load(void)\n{\n\tobs_frontend_add_event_callback(frontend_event, NULL);\n\tsr_scene_tracker_start();",
    "frontend callback register",
)
text = replace_once(
    text,
    "void obs_module_unload(void)\n{\n\tsr_scene_tracker_stop();",
    "void obs_module_unload(void)\n{\n\tobs_frontend_remove_event_callback(frontend_event, NULL);\n\tsr_scene_tracker_stop();",
    "frontend callback unregister",
)
write(path, text)

# ---------------------------------------------------------------------------
# Dock UI: fixed SETUP label + separate selected source count; duration/FPS in
# the REC line; packet counters moved into Hardware / Performance.
# ---------------------------------------------------------------------------
path = "src/sr-event-dock.cpp"
text = read(path)
insert_marker = "\nQStringList captureCameraNames()\n{"
helpers = r'''

QString recordingDurationText(uint64_t ns)
{
	const uint64_t totalSeconds = ns / NS_PER_SECOND;
	const uint64_t hours = totalSeconds / 3600ULL;
	const uint64_t minutes = (totalSeconds / 60ULL) % 60ULL;
	const uint64_t seconds = totalSeconds % 60ULL;
	return QStringLiteral("%1:%2:%3")
		.arg(hours, 2, 10, QChar('0'))
		.arg(minutes, 2, 10, QChar('0'))
		.arg(seconds, 2, 10, QChar('0'));
}

QString recordingFpsText()
{
	struct obs_video_info video = {};
	if (!obs_get_video_info(&video) || !video.fps_num || !video.fps_den)
		return QStringLiteral("—");
	if (video.fps_num % video.fps_den == 0)
		return QString::number(video.fps_num / video.fps_den);
	return QString::number((double)video.fps_num / (double)video.fps_den, 'f', 2);
}
'''
text = replace_once(text, insert_marker, helpers + insert_marker, "record status helpers")
text = replace_once(
    text,
    "\t\trecordStatus = new QLabel(this);\n\t\trecordStatus->setWordWrap(true);\n\t\tsetupButton = new QToolButton(this);",
    "\t\trecordStatus = new QLabel(this);\n\t\trecordStatus->setWordWrap(true);\n\t\tsetupSourceStatus = new QLabel(this);\n\t\tsetupSourceStatus->setAlignment(Qt::AlignCenter);\n\t\tsetupSourceStatus->setMinimumWidth(76);\n\t\tsetupSourceStatus->setStyleSheet(QStringLiteral(\"color: gray;\"));\n\t\tsetupButton = new QToolButton(this);",
    "setup source label create",
)
text = replace_once(
    text,
    "\t\trecordBar->addWidget(repairABButton);\n\t\trecordBar->addWidget(settingsGear);\n\t\trecordBar->addWidget(setupButton);",
    "\t\trecordBar->addWidget(repairABButton);\n\t\trecordBar->addWidget(settingsGear);\n\t\trecordBar->addWidget(setupSourceStatus);\n\t\trecordBar->addWidget(setupButton);",
    "setup source label layout",
)
text = replace_once(text, "performanceTable->setColumnCount(7);", "performanceTable->setColumnCount(8);", "performance columns")
text = replace_once(
    text,
    "\t\t\t T(\"EventDock.Performance.Queue\"), T(\"EventDock.Performance.Drops\"),\n\t\t\t T(\"EventDock.Performance.Disk\")});",
    "\t\t\t T(\"EventDock.Performance.Queue\"), T(\"EventDock.Performance.Packets\"),\n\t\t\t T(\"EventDock.Performance.Drops\"), T(\"EventDock.Performance.Disk\")});",
    "performance headers",
)
text = sub_once(
    text,
    r"\tvoid refreshSetupStatus\(\)\n\t\{.*?\n\t\}\n\n\tbool openReplaySetup\(\)",
    r'''	void refreshSetupStatus()
	{
		if (!setupButton || !setupSourceStatus)
			return;
		setupButton->setText(T("EventDock.Setup.Button"));
		setupButton->setStyleSheet(QString());

		sr_replay_setup_snapshot snapshot = {};
		if (!sr_replay_setup_get_snapshot(&snapshot)) {
			setupSourceStatus->setText(T("EventDock.Setup.SourceCount").arg(QStringLiteral("—")));
			setupSourceStatus->setStyleSheet(QStringLiteral("color: gray;"));
			setupButton->setToolTip(T("EventDock.Setup.Unavailable"));
			return;
		}

		const size_t selectedSources =
			snapshot.enabled_capture_source_count + (snapshot.program_output_enabled ? 1 : 0);
		setupSourceStatus->setText(T("EventDock.Setup.SourceCount").arg(selectedSources));
		setupSourceStatus->setStyleSheet(
			selectedSources ? QStringLiteral("color: #30c85a; font-weight: bold;")
					: QStringLiteral("color: #d8a000; font-weight: bold;"));

		const QString sceneA = snapshot.bus_a_ready ? QString::fromUtf8(snapshot.scene_a)
							    : T("EventDock.Setup.Missing");
		const QString sceneB = snapshot.bus_b_ready ? QString::fromUtf8(snapshot.scene_b)
							    : T("EventDock.Setup.Missing");
		setupButton->setToolTip(T("EventDock.Setup.Tooltip")
						.arg(snapshot.enabled_capture_source_count)
						.arg(snapshot.compatible_source_count)
						.arg(sceneA)
						.arg(sceneB));
		sr_replay_setup_free_snapshot(&snapshot);
	}

	bool openReplaySetup()''',
    "refresh setup status",
)
text = sub_once(
    text,
    r"\tvoid refreshRecordingStatus\(\)\n\t\{.*?\n\t\}\n\n\tvoid refreshHardwareStatus\(\)",
    r'''	void refreshRecordingStatus()
	{
		if (!recordStatus)
			return;
		sr_capture_recording_summary summary = {};
		if (!sr_capture_get_recording_summary(&summary) || !summary.camera_count) {
			updateRecordToggle(nullptr);
			recordStatus->setText(T("EventDock.RecordNoCameras"));
			recordStatus->setStyleSheet(QStringLiteral("color: #d8a000;"));
			return;
		}

		updateRecordToggle(&summary);

		if (summary.reserve_blocked_count) {
			recordStatus->setText(T("EventDock.RecordReserve")
						      .arg(summary.reserve_blocked_count)
						      .arg(summary.camera_count));
			recordStatus->setStyleSheet(QStringLiteral("color: #ff5b5b; font-weight: bold;"));
		} else if (summary.failed_count) {
			recordStatus->setText(
				T("EventDock.RecordError").arg(summary.failed_count).arg(summary.camera_count));
			recordStatus->setStyleSheet(QStringLiteral("color: #ff5b5b; font-weight: bold;"));
		} else if (summary.active_count && summary.active_count == summary.requested_count) {
			recordStatus->setText(T("EventDock.RecordActive")
						      .arg(summary.active_count)
						      .arg(summary.camera_count)
						      .arg(recordingDurationText(summary.recording_duration_ns))
						      .arg(recordingFpsText())
						      .arg((double)summary.bytes_written / (1024.0 * 1024.0), 0, 'f', 1));
			recordStatus->setStyleSheet(QStringLiteral("color: #30c85a; font-weight: bold;"));
		} else if (summary.requested_count) {
			recordStatus->setText(
				T("EventDock.RecordStarting").arg(summary.active_count).arg(summary.requested_count));
			recordStatus->setStyleSheet(QStringLiteral("color: #d8a000;"));
		} else {
			recordStatus->setText(T("EventDock.RecordIdle").arg(summary.camera_count));
			recordStatus->setStyleSheet(QStringLiteral("color: gray;"));
		}
	}

	void refreshHardwareStatus()''',
    "refresh recording status",
)
text = replace_once(
    text,
    "\t\tuint64_t droppedPackets = 0;",
    "\t\tuint64_t writtenPackets = 0;\n\t\tuint64_t droppedPackets = 0;",
    "performance packet total declaration",
)
text = replace_once(
    text,
    "\t\t\tdroppedPackets += entry.packets_dropped;",
    "\t\t\twrittenPackets += entry.packets_written;\n\t\t\tdroppedPackets += entry.packets_dropped;",
    "performance packet total",
)
text = replace_once(
    text,
    "\t\t\tconst QString drops = QString::number(entry.packets_dropped);\n\t\t\tconst QString disk = captureDiskState(entry);",
    "\t\t\tconst QString packets = QString::number(entry.packets_written);\n\t\t\tconst QString drops = QString::number(entry.packets_dropped);\n\t\t\tconst QString disk = captureDiskState(entry);",
    "performance packets value",
)
text = replace_once(
    text,
    "\t\t\tconst QString values[] = {\n\t\t\t\tQString::fromUtf8(entry.camera_name), path, video, gop, queue, drops, disk};",
    "\t\t\tconst QString values[] = {\n\t\t\t\tQString::fromUtf8(entry.camera_name), path, video, gop, queue, packets, drops, disk};",
    "performance packets array",
)
text = replace_once(
    text,
    "\t\t\t\t.arg(errorCount)\n\t\t\t\t.arg(fallbackCount)\n\t\t\t\t.arg(droppedPackets) +",
    "\t\t\t\t.arg(errorCount)\n\t\t\t\t.arg(fallbackCount)\n\t\t\t\t.arg(writtenPackets)\n\t\t\t\t.arg(droppedPackets) +",
    "performance summary packets",
)
text = replace_once(
    text,
    "\tQToolButton *setupButton = nullptr;\n\tQLabel *recordStatus = nullptr;",
    "\tQToolButton *setupButton = nullptr;\n\tQLabel *setupSourceStatus = nullptr;\n\tQLabel *recordStatus = nullptr;",
    "setup member",
)
write(path, text)

# ---------------------------------------------------------------------------
# Locale text. Keep placeholder order synchronized with the UI code.
# ---------------------------------------------------------------------------
for path, values in {
    "data/locale/en-US.ini": {
        "EventDock.RecordActive": 'EventDock.RecordActive="● REC — %1/%2 source(s) · %3 · %4 fps · %5 MB"',
        "EventDock.RecordBeforeMark": 'EventDock.RecordBeforeMark="Start recording and wait for REC to become active before creating an Event"',
        "EventDock.Performance.Packets": 'EventDock.Performance.Packets="Packets"',
        "EventDock.Performance.Summary": 'EventDock.Performance.Summary="Capture: GPU %1 | CPU %2 | waiting %3 | errors %4 | GPU fallback %5 | packets %6 | dropped %7"',
        "EventDock.Setup.SourceCount": 'EventDock.Setup.SourceCount="Sources: %1"',
    },
    "data/locale/es-ES.ini": {
        "EventDock.RecordActive": 'EventDock.RecordActive="● REC — %1/%2 fuente(s) · %3 · %4 fps · %5 MB"',
        "EventDock.RecordBeforeMark": 'EventDock.RecordBeforeMark="Inicia la grabación y espera a que REC esté activo antes de crear un Event"',
        "EventDock.Performance.Packets": 'EventDock.Performance.Packets="Paquetes"',
        "EventDock.Performance.Summary": 'EventDock.Performance.Summary="Captura: GPU %1 | CPU %2 | esperando %3 | errores %4 | fallback GPU %5 | paquetes %6 | descartados %7"',
        "EventDock.Setup.SourceCount": 'EventDock.Setup.SourceCount="Fuentes: %1"',
    },
}.items():
    text = read(path)
    for key, line in values.items():
        pattern = rf"^{re.escape(key)}=.*$"
        if re.search(pattern, text, flags=re.M):
            text = re.sub(pattern, line, text, count=1, flags=re.M)
        else:
            text += "\n" + line + "\n"
    write(path, text)

print("Applied recording status/FPS/packets/setup-count/no-auto-resume patch")
