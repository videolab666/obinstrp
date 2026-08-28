from pathlib import Path

root = Path(__file__).resolve().parents[2]

cmake = root / "CMakeLists.txt"
text = cmake.read_text(encoding="utf-8")
needle = "    src/sr-session.c\n    src/sr-event-db.c\n"
replacement = "    src/sr-session.c\n    src/sr-recording-run-catalog.c\n    src/sr-event-db.c\n"
if needle not in text and "src/sr-recording-run-catalog.c" not in text:
    raise SystemExit("CMake insertion point not found")
if "src/sr-recording-run-catalog.c" not in text:
    text = text.replace(needle, replacement, 1)
cmake.write_text(text, encoding="utf-8")

path = root / "src/sr-session-export.cpp"
text = path.read_text(encoding="utf-8")

include_needle = '#include "sr-master-audio-catalog.h"\n#include "sr-segment-catalog.h"\n'
include_replacement = '#include "sr-master-audio-catalog.h"\n#include "sr-recording-run-catalog.h"\n#include "sr-segment-catalog.h"\n'
if "sr-recording-run-catalog.h" not in text:
    if include_needle not in text:
        raise SystemExit("include insertion point not found")
    text = text.replace(include_needle, include_replacement, 1)

helper_anchor = "bool cameraHasEventStart(const std::string &sessionDir, const CameraInfo &camera, uint64_t eventInNs)\n"
helper = r'''bool cameraRunMediaBounds(const std::string &sessionDir, const CameraInfo &camera, uint64_t runStartNs,
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

'''
if "bool cameraRunMediaBounds(" not in text:
    if helper_anchor not in text:
        raise SystemExit("helper insertion point not found")
    text = text.replace(helper_anchor, helper + helper_anchor, 1)

start_marker = "\tbool planIso(std::vector<ExportTask> *tasks)\n\t{"
end_marker = "\n\tvoid run()\n\t{"
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0:
    raise SystemExit("planIso block not found")
new_plan = r'''	bool planIso(std::vector<ExportTask> *tasks)
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
				if (!cameraRunMediaBounds(session, camera, run.timeline_start_ns, run.timeline_end_ns, &mediaFirst,
							  &mediaLast))
					continue;

				uint64_t globalFirst = 0;
				uint64_t globalLast = 0;
				if (!mediaToGlobal(mediaFirst, camera.syncOffsetNs, &globalFirst) ||
				    !mediaToGlobal(mediaLast, camera.syncOffsetNs, &globalLast) || globalLast < globalFirst)
					continue;
				if (globalFirst < run.timeline_start_ns)
					globalFirst = run.timeline_start_ns;
				if (globalLast > run.timeline_end_ns)
					globalLast = run.timeline_end_ns;
				if (globalLast < globalFirst)
					continue;

				const QString stem =
					QStringLiteral("Run_%1_%2")
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
'''
text = text[:start] + new_plan + text[end:]
path.write_text(text, encoding="utf-8")
