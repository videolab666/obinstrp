/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-session-export.h"

#include "sr-camera-identity.h"
#include "sr-event-db.h"
#include "sr-event-export.h"
#include "sr-master-audio-catalog.h"
#include "sr-recording-run-catalog.h"
#include "sr-segment-catalog.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <atomic>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTimer>
#include <QWidget>

namespace {

enum class ExportMode { Clips, Iso };

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString safeFilePart(const QString &value)
{
	QString safe;
	safe.reserve(value.size());
	for (const QChar character : value) {
		if (character.isLetterOrNumber() || character == QChar('-') || character == QChar('_'))
			safe.append(character);
		else if (safe.isEmpty() || safe.back() != QChar('_'))
			safe.append(QChar('_'));
	}
	while (safe.endsWith(QChar('_')))
		safe.chop(1);
	return safe.isEmpty() ? QStringLiteral("item") : safe.left(96);
}

QString sessionDisplayName(const QString &sessionPath)
{
	const QByteArray utf8 = sessionPath.toUtf8();
	char *raw = sr_session_get_display_name(utf8.constData());
	const QString name = raw && *raw ? QString::fromUtf8(raw) : QFileInfo(sessionPath).fileName();
	bfree(raw);
	return name;
}

QString unusedMp4Path(const QDir &directory, const QString &stem)
{
	QString path = directory.filePath(stem + QStringLiteral(".mp4"));
	for (unsigned suffix = 2; QFileInfo::exists(path); suffix++)
		path = directory.filePath(stem + QStringLiteral("-%1.mp4").arg(suffix));
	return path;
}

bool globalToMedia(uint64_t globalNs, int64_t offsetNs, uint64_t *mediaNs)
{
	if (!mediaNs)
		return false;
	if (offsetNs >= 0) {
		const uint64_t magnitude = (uint64_t)offsetNs;
		if (globalNs > UINT64_MAX - magnitude)
			return false;
		*mediaNs = globalNs + magnitude;
		return true;
	}
	const uint64_t magnitude = (uint64_t)(-(offsetNs + 1)) + 1ULL;
	if (globalNs < magnitude)
		return false;
	*mediaNs = globalNs - magnitude;
	return true;
}

bool mediaToGlobal(uint64_t mediaNs, int64_t offsetNs, uint64_t *globalNs)
{
	if (!globalNs)
		return false;
	if (offsetNs >= 0) {
		const uint64_t magnitude = (uint64_t)offsetNs;
		if (mediaNs < magnitude)
			return false;
		*globalNs = mediaNs - magnitude;
		return true;
	}
	const uint64_t magnitude = (uint64_t)(-(offsetNs + 1)) + 1ULL;
	if (mediaNs > UINT64_MAX - magnitude)
		return false;
	*globalNs = mediaNs + magnitude;
	return true;
}

struct CameraInfo {
	std::string name;
	std::string stableKey;
	int64_t syncOffsetNs = 0;
};

struct ExportTask {
	std::string sessionDir;
	std::string camera;
	std::string outputPath;
	std::string audioDirectory;
	uint64_t inNs = 0;
	uint64_t outNs = 0;
	int64_t syncOffsetNs = 0;
	bool includeAudio = false;
};

bool cameraInfoFromName(const std::string &sessionDir, const std::string &name, CameraInfo *info)
{
	if (!info || name.empty())
		return false;
	info->name = name;
	if (sr_camera_is_program_name(name.c_str())) {
		info->stableKey = SR_PROGRAM_CAMERA_KEY;
		info->syncOffsetNs = 0;
		return true;
	}

	char key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	int64_t offset = 0;
	if (!sr_session_resolve_camera(sessionDir.c_str(), name.c_str(), key, sizeof(key), &offset))
		return false;
	info->stableKey = key;
	info->syncOffsetNs = offset;
	return true;
}

bool cameraInfoFromRecord(const sr_camera_record &record, CameraInfo *info)
{
	if (!info || !record.display_name || !*record.display_name || !record.stable_key || !*record.stable_key)
		return false;
	info->name = record.display_name;
	info->stableKey = record.stable_key;
	info->syncOffsetNs = record.sync_offset_ns;
	return true;
}

bool cameraMediaBounds(const std::string &sessionDir, const CameraInfo &camera, uint64_t *firstNs, uint64_t *lastNs)
{
	if (firstNs)
		*firstNs = 0;
	if (lastNs)
		*lastNs = 0;
	struct sr_segment_descriptor *segments = nullptr;
	size_t count = 0;
	if (!sr_segment_catalog_scan(sessionDir.c_str(), camera.name.c_str(), &segments, &count) || !count) {
		sr_segment_catalog_free(segments, count);
		return false;
	}

	uint64_t first = UINT64_MAX;
	uint64_t last = 0;
	for (size_t i = 0; i < count; i++) {
		if (segments[i].start_ns < first)
			first = segments[i].start_ns;
		if (segments[i].end_ns > last)
			last = segments[i].end_ns;
	}
	sr_segment_catalog_free(segments, count);
	if (first == UINT64_MAX)
		return false;
	if (firstNs)
		*firstNs = first;
	if (lastNs)
		*lastNs = last;
	return true;
}

bool cameraRunMediaBounds(const std::string &sessionDir, const CameraInfo &camera, uint64_t runStartNs,
			  uint64_t runEndNs, uint64_t *firstNs, uint64_t *lastNs)
{
	if (firstNs)
		*firstNs = 0;
	if (lastNs)
		*lastNs = 0;
	if (runEndNs < runStartNs)
		return false;

	uint64_t mediaStartNs = 0;
	uint64_t mediaEndNs = 0;
	if (!globalToMedia(runStartNs, camera.syncOffsetNs, &mediaStartNs) ||
	    !globalToMedia(runEndNs, camera.syncOffsetNs, &mediaEndNs) || mediaEndNs < mediaStartNs)
		return false;

	struct sr_segment_descriptor *segments = nullptr;
	size_t count = 0;
	if (!sr_segment_catalog_scan(sessionDir.c_str(), camera.name.c_str(), &segments, &count) || !count) {
		sr_segment_catalog_free(segments, count);
		return false;
	}

	uint64_t first = UINT64_MAX;
	uint64_t last = 0;
	for (size_t i = 0; i < count; i++) {
		if (segments[i].end_ns < mediaStartNs || segments[i].start_ns > mediaEndNs)
			continue;
		const uint64_t overlapStart = segments[i].start_ns > mediaStartNs ? segments[i].start_ns : mediaStartNs;
		const uint64_t overlapEnd = segments[i].end_ns < mediaEndNs ? segments[i].end_ns : mediaEndNs;
		if (overlapEnd < overlapStart)
			continue;
		if (overlapStart < first)
			first = overlapStart;
		if (overlapEnd > last)
			last = overlapEnd;
	}
	sr_segment_catalog_free(segments, count);
	if (first == UINT64_MAX || last < first)
		return false;
	if (firstNs)
		*firstNs = first;
	if (lastNs)
		*lastNs = last;
	return true;
}

bool cameraHasEventStart(const std::string &sessionDir, const CameraInfo &camera, uint64_t eventInNs)
{
	uint64_t mediaInNs = 0;
	if (!globalToMedia(eventInNs, camera.syncOffsetNs, &mediaInNs))
		return false;
	struct sr_segment_descriptor *segments = nullptr;
	size_t count = 0;
	const bool scanned = sr_segment_catalog_scan(sessionDir.c_str(), camera.name.c_str(), &segments, &count);
	const bool found = scanned && sr_segment_catalog_find(segments, count, mediaInNs) != nullptr;
	sr_segment_catalog_free(segments, count);
	return found;
}

std::vector<CameraInfo> sessionCameras(const std::string &sessionDir)
{
	std::vector<CameraInfo> cameras;
	std::set<std::string> seen;
	char **names = nullptr;
	size_t count = 0;
	if (sr_session_list_camera_names(sessionDir.c_str(), &names, &count)) {
		for (size_t i = 0; i < count; i++) {
			const std::string name = names[i] ? names[i] : "";
			CameraInfo info;
			if (!name.empty() && seen.insert(name).second && cameraInfoFromName(sessionDir, name, &info))
				cameras.emplace_back(std::move(info));
		}
	}
	sr_session_free_camera_names(names, count);

	CameraInfo program;
	program.name = SR_PROGRAM_CAMERA_NAME;
	program.stableKey = SR_PROGRAM_CAMERA_KEY;
	uint64_t ignoredFirst = 0;
	uint64_t ignoredLast = 0;
	if (seen.insert(program.name).second && cameraMediaBounds(sessionDir, program, &ignoredFirst, &ignoredLast))
		cameras.emplace_back(std::move(program));
	return cameras;
}

bool directoryHasAudio(const std::string &directory)
{
	if (directory.empty())
		return false;
	struct sr_master_audio_descriptor *segments = nullptr;
	size_t count = 0;
	const bool ok = sr_audio_catalog_scan_directory(directory.c_str(), &segments, &count) && count > 0;
	sr_master_audio_catalog_free(segments, count);
	return ok;
}

std::string cameraAudioDirectory(const std::string &sessionDir, const CameraInfo &camera)
{
	if (sr_camera_is_program_name(camera.name.c_str()))
		return {};

	char *stable = sr_camera_directory_for_key(sessionDir.c_str(), camera.stableKey.c_str());
	std::string result;
	if (stable && directoryHasAudio(stable))
		result = stable;
	bfree(stable);
	if (!result.empty())
		return result;

	char *legacy = sr_camera_legacy_directory(sessionDir.c_str(), camera.name.c_str());
	if (legacy && directoryHasAudio(legacy))
		result = legacy;
	bfree(legacy);
	return result;
}

CameraInfo chooseEventCamera(const std::string &sessionDir, sr_event_db *db, const sr_event_record &event,
			     const std::vector<CameraInfo> &cameras, bool *found)
{
	if (found)
		*found = false;
	if (event.preferred_camera_id) {
		sr_camera_record record = {};
		if (sr_event_db_get_camera(db, event.preferred_camera_id, &record)) {
			CameraInfo preferred;
			const bool valid = cameraInfoFromRecord(record, &preferred) &&
					   cameraHasEventStart(sessionDir, preferred, event.in_ns);
			sr_camera_record_free(&record);
			if (valid) {
				if (found)
					*found = true;
				return preferred;
			}
		}
	}

	/* Prefer actual camera ISOs before PROGRAM when an Event has no explicit
	 * preferred angle. PROGRAM is appended after archived cameras. */
	for (const CameraInfo &camera : cameras) {
		if (cameraHasEventStart(sessionDir, camera, event.in_ns)) {
			if (found)
				*found = true;
			return camera;
		}
	}
	return {};
}

class SessionExportOperation final : public QObject {
public:
	SessionExportOperation(QWidget *parent, ExportMode mode, QString sessionPath, QString outputDirectory)
		: QObject(parent),
		  parentWidget(parent),
		  exportMode(mode),
		  sessionDir(std::move(sessionPath)),
		  outputDir(std::move(outputDirectory))
	{
		dialog = new QProgressDialog(T("Session.ExportPreparing"), T("Session.ExportCancel"), 0, 100, parent);
		dialog->setWindowTitle(exportMode == ExportMode::Clips ? T("Session.ExportClips")
								       : T("Session.ExportIso"));
		dialog->setWindowModality(Qt::WindowModal);
		dialog->setMinimumDuration(0);
		dialog->setAutoClose(false);
		dialog->setAutoReset(false);
		dialog->setValue(0);
		connect(dialog, &QProgressDialog::canceled, this,
			[this]() { cancel.store(true, std::memory_order_relaxed); });

		timer = new QTimer(this);
		timer->setInterval(100);
		connect(timer, &QTimer::timeout, this, [this]() { poll(); });
	}

	~SessionExportOperation() override
	{
		cancel.store(true, std::memory_order_relaxed);
		if (worker.joinable())
			worker.join();
	}

	void start()
	{
		timer->start();
		worker = std::thread([this]() { run(); });
	}

private:
	static bool shouldCancel(void *data)
	{
		return static_cast<SessionExportOperation *>(data)->cancel.load(std::memory_order_relaxed);
	}

	static void reportTaskProgress(void *data, unsigned percent)
	{
		auto *operation = static_cast<SessionExportOperation *>(data);
		const size_t total = operation->taskCount.load(std::memory_order_relaxed);
		const size_t completed = operation->completedForProgress.load(std::memory_order_relaxed);
		const unsigned overall = total ? (unsigned)((completed * 100ULL + percent) / total) : 0U;
		operation->progress.store(overall > 100U ? 100U : overall, std::memory_order_relaxed);
	}

	bool planClips(std::vector<ExportTask> *tasks)
	{
		const QByteArray sessionUtf8 = sessionDir.toUtf8();
		const std::string session(sessionUtf8.constData());
		sr_event_db *db = sr_event_db_open(session.c_str());
		if (!db) {
			planningError = T("Session.ExportDatabaseError");
			return false;
		}

		std::set<uint64_t> eventIds;
		for (unsigned list = 1; list <= SR_EVENT_LIST_COUNT; list++) {
			uint64_t *ids = nullptr;
			size_t count = 0;
			if (!sr_event_db_get_list_events(db, list, &ids, &count)) {
				bfree(ids);
				sr_event_db_close(db);
				planningError = T("Session.ExportDatabaseError");
				return false;
			}
			for (size_t i = 0; i < count; i++)
				eventIds.insert(ids[i]);
			bfree(ids);
		}

		const std::vector<CameraInfo> cameras = sessionCameras(session);
		QDir destination(outputDir);
		for (uint64_t id : eventIds) {
			sr_event_record event = {};
			if (!sr_event_db_get_event(db, id, &event)) {
				skipped++;
				continue;
			}
			if (event.pending || event.out_ns <= event.in_ns) {
				skipped++;
				sr_event_record_free(&event);
				continue;
			}

			bool cameraFound = false;
			const CameraInfo camera = chooseEventCamera(session, db, event, cameras, &cameraFound);
			if (!cameraFound) {
				skipped++;
				sr_event_record_free(&event);
				continue;
			}

			QString label;
			if (event.name && *event.name)
				label = safeFilePart(QString::fromUtf8(event.name));
			else if (event.tag && *event.tag)
				label = safeFilePart(QString::fromUtf8(event.tag));
			QString stem = QStringLiteral("Event_%1").arg(id, 6, 10, QChar('0'));
			if (!label.isEmpty())
				stem += QStringLiteral("_%1").arg(label);
			stem += QStringLiteral("_%1").arg(safeFilePart(QString::fromUtf8(camera.name.c_str())));

			ExportTask task;
			task.sessionDir = session;
			task.camera = camera.name;
			task.outputPath = unusedMp4Path(destination, stem).toUtf8().constData();
			task.inNs = event.in_ns;
			task.outNs = event.out_ns;
			task.syncOffsetNs = camera.syncOffsetNs;
			if (event.audio_mode == SR_EVENT_AUDIO_MASTER ||
			    sr_camera_is_program_name(camera.name.c_str())) {
				task.includeAudio = true;
			} else if (event.audio_mode == SR_EVENT_AUDIO_CAMERA) {
				task.audioDirectory = cameraAudioDirectory(session, camera);
				task.includeAudio = !task.audioDirectory.empty();
			}
			tasks->emplace_back(std::move(task));
			sr_event_record_free(&event);
		}
		sr_event_db_close(db);
		if (tasks->empty()) {
			planningError = eventIds.empty() ? T("Session.ExportNoEvents") : T("Session.ExportNoMedia");
			return false;
		}
		return true;
	}

	bool planIso(std::vector<ExportTask> *tasks)
	{
		const QByteArray sessionUtf8 = sessionDir.toUtf8();
		const std::string session(sessionUtf8.constData());
		const std::vector<CameraInfo> cameras = sessionCameras(session);
		struct sr_recording_run_record *runs = nullptr;
		size_t runCount = 0;
		if (!sr_recording_run_catalog_scan(session.c_str(), &runs, &runCount) || !runCount) {
			bfree(runs);
			planningError = T("Session.ExportNoMedia");
			return false;
		}

		QDir destination(outputDir);
		for (size_t runIndex = 0; runIndex < runCount; runIndex++) {
			const sr_recording_run_record &run = runs[runIndex];
			if (run.timeline_end_ns < run.timeline_start_ns)
				continue;

			for (const CameraInfo &camera : cameras) {
				uint64_t mediaFirst = 0;
				uint64_t mediaLast = 0;
				if (!cameraRunMediaBounds(session, camera, run.timeline_start_ns, run.timeline_end_ns,
							  &mediaFirst, &mediaLast))
					continue;

				uint64_t globalFirst = 0;
				uint64_t globalLast = 0;
				if (!mediaToGlobal(mediaFirst, camera.syncOffsetNs, &globalFirst) ||
				    !mediaToGlobal(mediaLast, camera.syncOffsetNs, &globalLast) ||
				    globalLast < globalFirst)
					continue;
				if (globalFirst < run.timeline_start_ns)
					globalFirst = run.timeline_start_ns;
				if (globalLast > run.timeline_end_ns)
					globalLast = run.timeline_end_ns;
				if (globalLast < globalFirst)
					continue;

				const QString stem = QStringLiteral("Run_%1_%2")
							     .arg(run.id, 6, 10, QChar('0'))
							     .arg(safeFilePart(QString::fromUtf8(camera.name.c_str())));

				ExportTask task;
				task.sessionDir = session;
				task.camera = camera.name;
				task.outputPath = unusedMp4Path(destination, stem).toUtf8().constData();
				task.inNs = globalFirst;
				task.outNs = globalLast == UINT64_MAX ? globalLast : globalLast + 1ULL;
				task.syncOffsetNs = camera.syncOffsetNs;
				task.includeAudio = true;
				if (!sr_camera_is_program_name(camera.name.c_str()))
					task.audioDirectory = cameraAudioDirectory(session, camera);
				/* A camera with no own AAC falls back to session master audio. */
				tasks->emplace_back(std::move(task));
			}
		}
		bfree(runs);
		if (tasks->empty()) {
			planningError = T("Session.ExportNoMedia");
			return false;
		}
		return true;
	}

	void run()
	{
		std::vector<ExportTask> tasks;
		const bool planned = exportMode == ExportMode::Clips ? planClips(&tasks) : planIso(&tasks);
		if (!planned) {
			done.store(true, std::memory_order_release);
			return;
		}

		taskCount.store(tasks.size(), std::memory_order_relaxed);
		for (size_t i = 0; i < tasks.size(); i++) {
			if (cancel.load(std::memory_order_relaxed))
				break;
			completedForProgress.store(i, std::memory_order_relaxed);
			const ExportTask &task = tasks[i];
			sr_event_export_spec spec = {};
			spec.session_dir = task.sessionDir.c_str();
			spec.camera_name = task.camera.c_str();
			spec.output_path = task.outputPath.c_str();
			spec.event_in_ns = task.inNs;
			spec.event_out_ns = task.outNs;
			spec.camera_sync_offset_ns = task.syncOffsetNs;
			spec.include_master_audio = task.includeAudio;
			spec.audio_directory_override = task.audioDirectory.empty() ? nullptr
										    : task.audioDirectory.c_str();

			sr_event_export_result result = {};
			if (sr_event_export_fast(&spec, shouldCancel, reportTaskProgress, this, &result)) {
				exported++;
			} else if (result.error != SR_EVENT_EXPORT_CANCELLED) {
				failed++;
				if (firstFailure.isEmpty()) {
					firstFailure = QStringLiteral("%1: %2").arg(
						QString::fromUtf8(task.camera.c_str()),
						QString::fromUtf8(sr_event_export_error_text(result.error)));
				}
			}
		}
		if (!cancel.load(std::memory_order_relaxed))
			progress.store(100, std::memory_order_relaxed);
		done.store(true, std::memory_order_release);
	}

	void poll()
	{
		if (dialog)
			dialog->setValue((int)progress.load(std::memory_order_relaxed));
		if (!done.load(std::memory_order_acquire))
			return;

		timer->stop();
		if (worker.joinable())
			worker.join();
		if (dialog) {
			dialog->setValue(100);
			dialog->close();
			dialog->deleteLater();
			dialog = nullptr;
		}

		if (cancel.load(std::memory_order_relaxed)) {
			QMessageBox::information(parentWidget, T("Session.ExportTitle"), T("Session.ExportCancelled"));
		} else if (taskCount.load(std::memory_order_relaxed) == 0) {
			QMessageBox::warning(parentWidget, T("Session.ExportTitle"), planningError);
		} else if (failed) {
			QMessageBox::warning(
				parentWidget, T("Session.ExportTitle"),
				T("Session.ExportPartial").arg(exported).arg(failed).arg(skipped).arg(firstFailure));
		} else {
			QMessageBox::information(parentWidget, T("Session.ExportTitle"),
						 T("Session.ExportComplete").arg(exported).arg(skipped).arg(outputDir));
		}
		deleteLater();
	}

	QWidget *parentWidget = nullptr;
	ExportMode exportMode;
	QString sessionDir;
	QString outputDir;
	QProgressDialog *dialog = nullptr;
	QTimer *timer = nullptr;
	std::thread worker;
	std::atomic<bool> cancel{false};
	std::atomic<bool> done{false};
	std::atomic<unsigned> progress{0};
	std::atomic<size_t> taskCount{0};
	std::atomic<size_t> completedForProgress{0};
	size_t exported = 0;
	size_t failed = 0;
	size_t skipped = 0;
	QString firstFailure;
	QString planningError;
};

void launchSessionExport(QWidget *parent, const char *sessionDir, ExportMode mode)
{
	if (!parent || !sessionDir || !*sessionDir)
		return;
	if (sr_session_path_is_active(sessionDir)) {
		QMessageBox::information(parent, T("Session.ExportTitle"), T("Session.ExportStopRecording"));
		return;
	}

	const QString sessionPath = QString::fromUtf8(sessionDir);
	const QString parentDirectory = QFileDialog::getExistingDirectory(parent, T("Session.ExportFolder"));
	if (parentDirectory.isEmpty())
		return;

	const QString modeSuffix = mode == ExportMode::Clips ? QStringLiteral("Clips") : QStringLiteral("ISO");
	const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	const QString folderName =
		QStringLiteral("%1_%2_%3").arg(safeFilePart(sessionDisplayName(sessionPath)), modeSuffix, stamp);
	QDir base(parentDirectory);
	if (!base.mkpath(folderName)) {
		QMessageBox::warning(parent, T("Session.ExportTitle"), T("Session.ExportCreateFolderFailed"));
		return;
	}
	const QString outputDirectory = base.filePath(folderName);

	auto *operation = new SessionExportOperation(parent, mode, sessionPath, outputDirectory);
	operation->start();
}

} // namespace

void sr_session_export_all_clips(QWidget *parent, const char *session_dir)
{
	launchSessionExport(parent, session_dir, ExportMode::Clips);
}

void sr_session_export_iso(QWidget *parent, const char *session_dir)
{
	launchSessionExport(parent, session_dir, ExportMode::Iso);
}
