from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


p = Path("src/sr-multiview-dock.cpp")
text = p.read_text(encoding="utf-8")
text = text.replace(
    "int dstLinesize[4] = {converted.bytesPerLine(), 0, 0, 0};",
    "int dstLinesize[4] = {static_cast<int>(converted.bytesPerLine()), 0, 0, 0};",
)
p.write_text(text, encoding="utf-8")

replace_once(
    "src/sr-multiview-dock.cpp",
    '''\tvoid setControlsEnabled(bool enabled)\n\t{\n\t\tfor (QWidget *widget : {static_cast<QWidget *>(autoAngle), playPause, playFromIn, gotoIn, setIn, setOut,\n\t\t\t\t\tgotoOut, prevFrame, nextFrame, loop, fit, live})\n\t\t\twidget->setEnabled(enabled);\n\t\ttimeline->setEnabled(enabled);\n\t}\n''',
    '''\tvoid setControlsEnabled(bool enabled)\n\t{\n\t\tQWidget *controls[] = {autoAngle, playPause, playFromIn, gotoIn, setIn, setOut,\n\t\t\t\t       gotoOut, prevFrame, nextFrame, loop, fit, live};\n\t\tfor (QWidget *widget : controls)\n\t\t\twidget->setEnabled(enabled);\n\t\ttimeline->setEnabled(enabled);\n\t}\n''',
    "controls QWidget array",
)

replace_once(
    "src/sr-multiview-dock.cpp",
    '''\tvoid updateTileState(const sr_event_editor_snapshot &snapshot)\n\t{\n\t\tconst QString selected = QString::fromUtf8(snapshot.selected_camera);\n\t\tconst QString preview = QString::fromUtf8(snapshot.preview_camera);\n\t\tfor (const auto &tile : tiles) {\n\t\t\ttile->setSelected(!selected.isEmpty() && tile->camera() == selected);\n\t\t\ttile->setPreview(!preview.isEmpty() && tile->camera() == preview);\n\t\t\tsr_replay_coverage_info coverage = {};\n\t\t\tconst QByteArray camera = tile->camera().toUtf8();\n\t\t\tif (!snapshot.available ||\n\t\t\t    !sr_replay_coverage_query(camera.constData(), snapshot.in_ns, snapshot.out_ns, &coverage))\n\t\t\t\tcoverage.coverage = SR_REPLAY_COVERAGE_NONE;\n\t\t\tconst bool atCursor = coverage.coverage != SR_REPLAY_COVERAGE_NONE &&\n\t\t\t\t\t      snapshot.playhead_ns >= coverage.playable_in_ns &&\n\t\t\t\t\t      snapshot.playhead_ns <= coverage.playable_out_ns;\n\t\t\ttile->setCoverage(coverage.coverage, atCursor);\n\t\t}\n\t}\n''',
    '''\tvoid refreshCoverageCache(const sr_event_editor_snapshot &snapshot, bool force)\n\t{\n\t\tconst qint64 nowMs = clock.elapsed();\n\t\tconst bool rangeChanged = snapshot.event_id != coverageEventId || snapshot.in_ns != coverageInNs ||\n\t\t\t\t\t snapshot.out_ns != coverageOutNs;\n\t\tif (!force && !rangeChanged && nowMs - lastCoverageRefreshMs < 1000)\n\t\t\treturn;\n\n\t\tcoverageCache.clear();\n\t\tif (snapshot.available) {\n\t\t\tfor (const auto &tile : tiles) {\n\t\t\t\tsr_replay_coverage_info coverage = {};\n\t\t\t\tconst QByteArray camera = tile->camera().toUtf8();\n\t\t\t\tif (!sr_replay_coverage_query(camera.constData(), snapshot.in_ns, snapshot.out_ns, &coverage))\n\t\t\t\t\tcoverage.coverage = SR_REPLAY_COVERAGE_NONE;\n\t\t\t\tcoverageCache.insert(tile->camera(), coverage);\n\t\t\t}\n\t\t}\n\t\tcoverageEventId = snapshot.event_id;\n\t\tcoverageInNs = snapshot.in_ns;\n\t\tcoverageOutNs = snapshot.out_ns;\n\t\tlastCoverageRefreshMs = nowMs;\n\t}\n\n\tbool cachedCoverage(const QString &camera, sr_replay_coverage_info *coverage) const\n\t{\n\t\tif (!coverage)\n\t\t\treturn false;\n\t\tconst auto it = coverageCache.constFind(camera);\n\t\tif (it == coverageCache.constEnd()) {\n\t\t\t*coverage = {};\n\t\t\treturn false;\n\t\t}\n\t\t*coverage = it.value();\n\t\treturn coverage->coverage != SR_REPLAY_COVERAGE_NONE;\n\t}\n\n\tvoid updateTileState(const sr_event_editor_snapshot &snapshot)\n\t{\n\t\tconst QString selected = QString::fromUtf8(snapshot.selected_camera);\n\t\tconst QString preview = QString::fromUtf8(snapshot.preview_camera);\n\t\tfor (const auto &tile : tiles) {\n\t\t\ttile->setSelected(!selected.isEmpty() && tile->camera() == selected);\n\t\t\ttile->setPreview(!preview.isEmpty() && tile->camera() == preview);\n\t\t\tsr_replay_coverage_info coverage = {};\n\t\t\tcachedCoverage(tile->camera(), &coverage);\n\t\t\tconst bool atCursor = coverage.coverage != SR_REPLAY_COVERAGE_NONE &&\n\t\t\t\t\t      snapshot.playhead_ns >= coverage.playable_in_ns &&\n\t\t\t\t\t      snapshot.playhead_ns <= coverage.playable_out_ns;\n\t\t\ttile->setCoverage(coverage.coverage, atCursor);\n\t\t}\n\t}\n''',
    "coverage cache and tile state",
)

replace_once(
    "src/sr-multiview-dock.cpp",
    '''\t\t\tsr_replay_coverage_info coverage = {};\n\t\t\tconst QByteArray camera = tile->camera().toUtf8();\n\t\t\tif (!sr_replay_coverage_query(camera.constData(), snapshot.in_ns, snapshot.out_ns, &coverage) ||\n\t\t\t    coverage.coverage == SR_REPLAY_COVERAGE_NONE ||\n\t\t\t    snapshot.playhead_ns < coverage.playable_in_ns ||\n\t\t\t    snapshot.playhead_ns > coverage.playable_out_ns)\n\t\t\t\tcontinue;\n''',
    '''\t\t\tsr_replay_coverage_info coverage = {};\n\t\t\tif (!cachedCoverage(tile->camera(), &coverage) || snapshot.playhead_ns < coverage.playable_in_ns ||\n\t\t\t    snapshot.playhead_ns > coverage.playable_out_ns)\n\t\t\t\tcontinue;\n''',
    "request frames cached coverage",
)

replace_once(
    "src/sr-multiview-dock.cpp",
    '''\t\ttimeline->setState(snapshot);\n\t\tupdateTileState(snapshot);\n\n\t\tconst bool eventChanged = snapshot.event_id != lastEventId;\n\t\tconst bool cursorChanged = snapshot.playhead_ns != lastPlayheadNs;''',
    '''\t\ttimeline->setState(snapshot);\n\t\tconst bool eventChanged = snapshot.event_id != lastEventId;\n\t\trefreshCoverageCache(snapshot, forceDecode || eventChanged);\n\t\tupdateTileState(snapshot);\n\n\t\tconst bool cursorChanged = snapshot.playhead_ns != lastPlayheadNs;''',
    "tick cached coverage refresh",
)

replace_once(
    "src/sr-multiview-dock.cpp",
    '''\t\tQMap<QString, qint64> lastRequestMs;\n\tsr_event_editor_snapshot lastSnapshot = {};\n\tuint64_t lastEventId = 0;''',
    '''\tQMap<QString, qint64> lastRequestMs;\n\tQMap<QString, sr_replay_coverage_info> coverageCache;\n\tsr_event_editor_snapshot lastSnapshot = {};\n\tuint64_t coverageEventId = 0;\n\tuint64_t coverageInNs = 0;\n\tuint64_t coverageOutNs = 0;\n\tqint64 lastCoverageRefreshMs = 0;\n\tuint64_t lastEventId = 0;''',
    "coverage cache members",
)

replace_once(
    "src/sr-multiview-dock.cpp",
    '''\t\tcameraNames = names;\n\t\tif (cameraNames.size() > 9)''',
    '''\t\tcameraNames = names;\n\t\tcoverageCache.clear();\n\t\tcoverageEventId = 0;\n\t\tlastCoverageRefreshMs = 0;\n\t\tif (cameraNames.size() > 9)''',
    "camera change coverage reset",
)

# The constructor no longer owns the controller directly; remove the stale member
# left by the first draft. The shared Event dock bridge is authoritative.
text = Path("src/sr-multiview-dock.cpp").read_text(encoding="utf-8")
text = text.replace("\tsr_event_controller *controller = nullptr;\n", "")
Path("src/sr-multiview-dock.cpp").write_text(text, encoding="utf-8")
