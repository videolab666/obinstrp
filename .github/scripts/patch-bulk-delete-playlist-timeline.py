from pathlib import Path


def replace_once(text, old, new, label):
    if old not in text:
        raise RuntimeError(f"missing pattern: {label}")
    return text.replace(old, new, 1)

root = Path('.')
dock_path = root / 'src/sr-event-dock.cpp'
playlist_h_path = root / 'src/sr-replay-playlist.h'
playlist_c_path = root / 'src/sr-replay-playlist.c'
en_path = root / 'data/locale/en-US.ini'
es_path = root / 'data/locale/es-ES.ini'

dock = dock_path.read_text(encoding='utf-8')

# QColor include.
dock = replace_once(dock, '#include <QComboBox>\n', '#include <QColor>\n#include <QComboBox>\n', 'QColor include')

# SrRangeSlider sequence-progress behavior and tint.
dock = replace_once(
    dock,
    '\tvoid setRangeHandler(RangeHandler handler) { rangeHandler = std::move(handler); }\n\tvoid setClickHandler(ClickHandler handler) { clickHandler = std::move(handler); }\n',
    '\tvoid setRangeHandler(RangeHandler handler) { rangeHandler = std::move(handler); }\n\tvoid setClickHandler(ClickHandler handler) { clickHandler = std::move(handler); }\n\tvoid setSequenceProgress(bool active)\n\t{\n\t\tsequenceProgress = active;\n\t\tif (active)\n\t\t\tclearSelection();\n\t}\n\tvoid setProgressTint(const QColor &color)\n\t{\n\t\tprogressTint = color;\n\t\tupdate();\n\t}\n\tvoid clearProgressTint()\n\t{\n\t\tprogressTint = QColor();\n\t\tupdate();\n\t}\n',
    'range slider methods')

dock = replace_once(
    dock,
    '\tvoid mousePressEvent(QMouseEvent *event) override\n\t{\n\t\tif (!isEnabled() || event->button() != Qt::LeftButton) {',
    '\tvoid mousePressEvent(QMouseEvent *event) override\n\t{\n\t\tif (sequenceProgress) {\n\t\t\tevent->accept();\n\t\t\treturn;\n\t\t}\n\t\tif (!isEnabled() || event->button() != Qt::LeftButton) {',
    'range slider block sequence mouse')

old_paint = '''\tvoid paintEvent(QPaintEvent *event) override
\t{
\t\tQSlider::paintEvent(event);
\t\tif (!selecting && !hasSelection)
\t\t\treturn;

\t\tconst int rangeIn = qMin(selectionStart, selectionEnd);
\t\tconst int rangeOut = qMax(selectionStart, selectionEnd);
\t\tconst int left = pixelForValue(rangeIn);
\t\tconst int right = pixelForValue(rangeOut);
\t\tif (right <= left)
\t\t\treturn;

\t\tQStyleOptionSlider option;
\t\tinitStyleOption(&option);
\t\tconst QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
\t\tQColor highlight = palette().color(QPalette::Highlight);
\t\thighlight.setAlpha(115);
\t\tQPainter painter(this);
\t\tpainter.setPen(Qt::NoPen);
\t\tpainter.setBrush(highlight);
\t\tpainter.drawRoundedRect(QRect(left, groove.center().y() - 4, right - left, 8), 3, 3);
\t}
'''
new_paint = '''\tvoid paintEvent(QPaintEvent *event) override
\t{
\t\tQSlider::paintEvent(event);

\t\tQStyleOptionSlider option;
\t\tinitStyleOption(&option);
\t\tconst QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
\t\tif (progressTint.isValid()) {
\t\t\tconst int left = pixelForValue(minimum());
\t\t\tconst int right = pixelForValue(value());
\t\t\tif (right > left) {
\t\t\t\tQColor tint = progressTint;
\t\t\t\ttint.setAlpha(210);
\t\t\t\tQPainter progressPainter(this);
\t\t\t\tprogressPainter.setPen(Qt::NoPen);
\t\t\t\tprogressPainter.setBrush(tint);
\t\t\t\tprogressPainter.drawRoundedRect(QRect(left, groove.center().y() - 4, right - left, 8), 3, 3);
\t\t\t}
\t\t}

\t\tif (!selecting && !hasSelection)
\t\t\treturn;

\t\tconst int rangeIn = qMin(selectionStart, selectionEnd);
\t\tconst int rangeOut = qMax(selectionStart, selectionEnd);
\t\tconst int left = pixelForValue(rangeIn);
\t\tconst int right = pixelForValue(rangeOut);
\t\tif (right <= left)
\t\t\treturn;

\t\tQColor highlight = palette().color(QPalette::Highlight);
\t\thighlight.setAlpha(115);
\t\tQPainter painter(this);
\t\tpainter.setPen(Qt::NoPen);
\t\tpainter.setBrush(highlight);
\t\tpainter.drawRoundedRect(QRect(left, groove.center().y() - 4, right - left, 8), 3, 3);
\t}
'''
dock = replace_once(dock, old_paint, new_paint, 'range slider paint')

dock = replace_once(
    dock,
    '\tbool selectionMoved = false;\n\tbool hasSelection = false;\n};\n',
    '\tbool selectionMoved = false;\n\tbool hasSelection = false;\n\tbool sequenceProgress = false;\n\tQColor progressTint;\n};\n',
    'range slider fields')

# Protect tooltip.
dock = replace_once(
    dock,
    '\tauto *protect = new QPushButton(T("EventDock.Protect"), this);\n\tauto *remove = new QPushButton(T("EventDock.Delete"), this);',
    '\tauto *protect = new QPushButton(T("EventDock.Protect"), this);\n\tprotect->setToolTip(T("EventDock.Protect.Tooltip"));\n\tauto *remove = new QPushButton(T("EventDock.Delete"), this);',
    'protect tooltip')

# Clear stale Angle still only when there is no valid Event at all.
needle = '''\t\tconst bool haveEvent = controller && eventId &&
\t\t\t\t       sr_event_controller_get_event(controller, eventId, &event);

\t\tQString preferredCamera;
'''
replacement = '''\t\tconst bool haveEvent = controller && eventId &&
\t\t\t\t       sr_event_controller_get_event(controller, eventId, &event);
\t\tif (!haveEvent) {
\t\t\tpreviewTargetEventId = 0;
\t\t\tpreviewLoadedEventId = 0;
\t\t\tfor (QToolButton *button : angleButtons)
\t\t\t\tbutton->setIcon(QIcon());
\t\t}

\t\tQString preferredCamera;
'''
dock = replace_once(dock, needle, replacement, 'clear stale angle preview')

# Do not cache an async angle result if its Event was deleted while decoding.
old_cache = '''\t\tconst uint64_t completedEventId = anglePreviewJob->eventId;
\t\tauto &cache = anglePreviewCache[completedEventId];
\t\tfor (const AnglePreviewResult &result : anglePreviewJob->results) {
\t\t\tif (result.rgba.empty())
\t\t\t\tcontinue;
\t\t\tconst QImage image(result.rgba.data(), ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT,
\t\t\t\t\t   ANGLE_PREVIEW_WIDTH * 4, QImage::Format_RGBA8888);
\t\t\tQIcon icon(QPixmap::fromImage(image.copy()));
\t\t\tcache[result.camera] = icon;
\t\t\tif (completedEventId == previewTargetEventId && angleEventId() == previewTargetEventId) {
\t\t\t\tif (QToolButton *button = angleButton(QString::fromUtf8(result.camera.c_str())))
\t\t\t\t\tbutton->setIcon(icon);
\t\t\t}
\t\t}
'''
new_cache = '''\t\tconst uint64_t completedEventId = anglePreviewJob->eventId;
\t\tsr_event_record completedEvent = {};
\t\tconst bool completedEventExists = controller && completedEventId &&
\t\t\tsr_event_controller_get_event(controller, completedEventId, &completedEvent);
\t\tif (completedEventExists) {
\t\t\tsr_event_controller_free_event(&completedEvent);
\t\t\tauto &cache = anglePreviewCache[completedEventId];
\t\t\tfor (const AnglePreviewResult &result : anglePreviewJob->results) {
\t\t\t\tif (result.rgba.empty())
\t\t\t\t\tcontinue;
\t\t\t\tconst QImage image(result.rgba.data(), ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT,
\t\t\t\t\t\t   ANGLE_PREVIEW_WIDTH * 4, QImage::Format_RGBA8888);
\t\t\t\tQIcon icon(QPixmap::fromImage(image.copy()));
\t\t\t\tcache[result.camera] = icon;
\t\t\t\tif (completedEventId == previewTargetEventId && angleEventId() == previewTargetEventId) {
\t\t\t\t\tif (QToolButton *button = angleButton(QString::fromUtf8(result.camera.c_str())))
\t\t\t\t\t\tbutton->setIcon(icon);
\t\t\t\t}
\t\t\t}
\t\t}
'''
dock = replace_once(dock, old_cache, new_cache, 'avoid caching deleted angle event')

# Playlist aggregate timeline helpers + replacement syncTimeline.
start = dock.index('\tvoid syncTimeline()\n\t{')
end = dock.index('\n\tvoid seekTimeline(int value)', start)
old_sync = dock[start:end]
new_sync = r'''\tuint64_t playbackRuntimeNs(uint64_t mediaNs, double speedPercent) const
\t{
\t\tconst double speed = speedPercent > 0.0 ? speedPercent : 100.0;
\t\tconst long double runtime = (long double)mediaNs * 100.0L / (long double)speed;
\t\treturn runtime >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)runtime;
\t}

\tbool playlistTimelineProgress(uint64_t *elapsedNs, uint64_t *totalNs, uint64_t *remainingNs) const
\t{
\t\tif (!elapsedNs || !totalNs || !remainingNs || !controller)
\t\t\treturn false;
\t\t*elapsedNs = 0;
\t\t*totalNs = 0;
\t\t*remainingNs = 0;

\t\tconst enum sr_replay_bus bus = activePlaylistBus();
\t\tsr_replay_playlist_state playlist = {};
\t\tif (!sr_replay_playlist_get_state(bus, &playlist) || !playlist.active)
\t\t\treturn false;

\t\tuint64_t *eventIds = nullptr;
\t\tsize_t count = 0;
\t\tsize_t position = 0;
\t\tbool angleSequence = false;
\t\tif (!sr_replay_playlist_snapshot_items(bus, &eventIds, &count, &position, &angleSequence) || !eventIds ||
\t\t    !count) {
\t\t\tbfree(eventIds);
\t\t\treturn false;
\t\t}

\t\tsr_replay_channel_state current = {};
\t\tconst bool haveCurrent = sr_replay_channel_get_state(bus, &current) && current.cued;
\t\tuint64_t total = 0;
\t\tuint64_t elapsed = 0;
\t\tfor (size_t i = 0; i < count; i++) {
\t\t\tuint64_t runtime = 0;
\t\t\tsr_event_record event = {};
\t\t\tif (sr_event_controller_get_event(controller, eventIds[i], &event)) {
\t\t\t\tif (!event.pending && event.out_ns > event.in_ns)
\t\t\t\t\truntime = playbackRuntimeNs(event.out_ns - event.in_ns, event.speed_percent);
\t\t\t\tsr_event_controller_free_event(&event);
\t\t\t}

\t\t\tuint64_t currentElapsed = 0;
\t\t\tif (i == position && haveCurrent && current.event_id == eventIds[i] && current.out_ns > current.in_ns) {
\t\t\t\truntime = playbackRuntimeNs(current.out_ns - current.in_ns, current.speed_percent);
\t\t\t\tconst uint64_t mediaPosition = current.playhead_ns <= current.in_ns
\t\t\t\t\t\t\t       ? 0
\t\t\t\t\t\t\t       : current.playhead_ns >= current.out_ns
\t\t\t\t\t\t\t\t       ? current.out_ns - current.in_ns
\t\t\t\t\t\t\t\t       : current.playhead_ns - current.in_ns;
\t\t\t\tcurrentElapsed = playbackRuntimeNs(mediaPosition, current.speed_percent);
\t\t\t\tif (currentElapsed > runtime)
\t\t\t\t\tcurrentElapsed = runtime;
\t\t\t}

\t\t\tif (UINT64_MAX - total < runtime)
\t\t\t\ttotal = UINT64_MAX;
\t\t\telse
\t\t\t\ttotal += runtime;
\t\t\tif (i < position) {
\t\t\t\tif (UINT64_MAX - elapsed < runtime)
\t\t\t\t\telapsed = UINT64_MAX;
\t\t\t\telse
\t\t\t\t\telapsed += runtime;
\t\t\t} else if (i == position) {
\t\t\t\tif (UINT64_MAX - elapsed < currentElapsed)
\t\t\t\t\telapsed = UINT64_MAX;
\t\t\t\telse
\t\t\t\t\telapsed += currentElapsed;
\t\t\t}
\t\t}
\t\tbfree(eventIds);
\t\tif (!total)
\t\t\treturn false;
\t\tif (elapsed > total)
\t\t\telapsed = total;
\t\t*elapsedNs = elapsed;
\t\t*totalNs = total;
\t\t*remainingNs = total - elapsed;
\t\treturn true;
\t}

\tvoid syncTimeline()
\t{
\t\tif (!timelineSlider || !timelineTime)
\t\t\treturn;

\t\tuint64_t sequenceElapsed = 0;
\t\tuint64_t sequenceTotal = 0;
\t\tuint64_t sequenceRemaining = 0;
\t\tif (playlistTimelineProgress(&sequenceElapsed, &sequenceTotal, &sequenceRemaining)) {
\t\t\ttimelineEventId = 0;
\t\t\ttimelineSlider->setSequenceProgress(true);
\t\t\ttimelineSlider->setEnabled(true);
\t\t\ttimelineSlider->setToolTip(T("EventDock.Timeline.PlaylistTooltip"));
\t\t\tconst int value = (int)((long double)sequenceElapsed * 10000.0L / (long double)sequenceTotal);
\t\t\ttimelineSlider->setValue(qBound(0, value, 10000));
\t\t\tif (sequenceRemaining > 15ULL * NS_PER_SECOND)
\t\t\t\ttimelineSlider->setProgressTint(QColor(QStringLiteral("#2fb34a")));
\t\t\telse if (sequenceRemaining > 10ULL * NS_PER_SECOND)
\t\t\t\ttimelineSlider->setProgressTint(QColor(QStringLiteral("#d2a216")));
\t\t\telse
\t\t\t\ttimelineSlider->setProgressTint(QColor(QStringLiteral("#d33b3b")));
\t\t\ttimelineTime->setText(replayClockText(sequenceElapsed) + QStringLiteral(" / ") +
\t\t\t\t\t  replayClockText(sequenceTotal) + QStringLiteral("   −") +
\t\t\t\t\t  replayClockText(sequenceRemaining));
\t\t\treturn;
\t\t}

\t\ttimelineSlider->setSequenceProgress(false);
\t\ttimelineSlider->clearProgressTint();
\t\ttimelineSlider->setToolTip(T("EventDock.Timeline.Tooltip"));
\t\tsr_replay_channel_state state = {};
\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued ||
\t\t    state.out_ns <= state.in_ns) {
\t\t\ttimelineEventId = 0;
\t\t\ttimelineSlider->clearSelection();
\t\t\ttimelineSlider->setEnabled(false);
\t\t\tif (!timelineDragging)
\t\t\t\ttimelineSlider->setValue(0);
\t\t\ttimelineTime->setText(QStringLiteral("--:--.--- / --:--.---"));
\t\t\treturn;
\t\t}

\t\tif (timelineEventId != state.event_id) {
\t\t\ttimelineEventId = state.event_id;
\t\t\ttimelineSlider->clearSelection();
\t\t}
\t\ttimelineSlider->setEnabled(true);
\t\tconst uint64_t duration = state.out_ns - state.in_ns;
\t\tconst uint64_t position = state.playhead_ns <= state.in_ns    ? 0
\t\t\t\t\t  : state.playhead_ns >= state.out_ns ? duration
\t\t\t\t\t\t\t\t\t      : state.playhead_ns - state.in_ns;
\t\tif (!timelineDragging) {
\t\t\tconst int sliderValue = (int)((long double)position * 10000.0L / (long double)duration);
\t\t\ttimelineSlider->setValue(sliderValue);
\t\t}
\t\ttimelineTime->setText(replayClockText(position) + QStringLiteral(" / ") + replayClockText(duration));
\t}
'''.replace('\\t', '\t')
dock = dock[:start] + new_sync + dock[end:]

# Multi-Protect and bulk deletion without confirmation.
start = dock.index('\tvoid toggleProtected()\n\t{')
end = dock.index('\n\tsr_event_controller *controller', start)
old_actions = dock[start:end]
new_actions = r'''\tvoid toggleProtected()
\t{
\t\tconst std::vector<uint64_t> ids = selectedEventIds();
\t\tif (ids.empty())
\t\t\treturn;
\t\tsr_event_record first = {};
\t\tif (!sr_event_controller_get_event(controller, ids.front(), &first)) {
\t\t\tsetStatus("EventDock.Failed");
\t\t\treturn;
\t\t}
\t\tconst bool value = !first.protected_event;
\t\tsr_event_controller_free_event(&first);
\t\tbool ok = true;
\t\tfor (uint64_t id : ids)
\t\t\tok = sr_event_controller_set_protected(controller, id, value) && ok;
\t\tif (!ok)
\t\t\tsetStatus("EventDock.Failed");
\t\trefresh();
\t}

\tvoid clearDeletedEventUi(uint64_t eventId)
\t{
\t\teventThumbnailCache.erase(eventId);
\t\tanglePreviewCache.erase(eventId);
\t\tif (previewTargetEventId == eventId) {
\t\t\tpreviewTargetEventId = 0;
\t\t\tpreviewLoadedEventId = 0;
\t\t}
\t\tfor (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
\t\t\tconst auto bus = static_cast<sr_replay_bus>(i);
\t\t\tsr_replay_playlist_state playlist = {};
\t\t\tif (sr_replay_playlist_get_state(bus, &playlist) && playlist.active && playlist.event_id == eventId)
\t\t\t\tsr_replay_playlist_stop(bus);
\t\t\tsr_replay_channel_state state = {};
\t\t\tif (sr_replay_channel_get_state(bus, &state) && state.cued && state.event_id == eventId)
\t\t\t\tsr_replay_channel_clear(bus);
\t\t}
\t}

\tvoid deleteSelected(bool deleteMedia)
\t{
\t\tconst std::vector<uint64_t> ids = selectedEventIds();
\t\tif (ids.empty())
\t\t\treturn;

\t\tsize_t deleted = 0;
\t\tsize_t protectedSkipped = 0;
\t\tsize_t errors = 0;
\t\tsr_storage_cleanup_result cleanupTotal = {};
\t\tfor (uint64_t eventId : ids) {
\t\t\tsr_event_record event = {};
\t\t\tif (!sr_event_controller_get_event(controller, eventId, &event)) {
\t\t\t\terrors++;
\t\t\t\tcontinue;
\t\t\t}
\t\t\tconst bool protectedEvent = event.protected_event;
\t\t\tsr_event_controller_free_event(&event);
\t\t\tif (protectedEvent) {
\t\t\t\tprotectedSkipped++;
\t\t\t\tcontinue;
\t\t\t}

\t\t\tbool ok = false;
\t\t\tif (!deleteMedia) {
\t\t\t\tok = sr_event_controller_delete_event(controller, eventId);
\t\t\t} else {
\t\t\t\tsr_storage_cleanup_result cleanup = {};
\t\t\t\tok = sr_event_controller_delete_event_with_media(controller, eventId, &cleanup);
\t\t\t\tcleanupTotal.segments_examined += cleanup.segments_examined;
\t\t\t\tcleanupTotal.segments_deleted += cleanup.segments_deleted;
\t\t\t\tcleanupTotal.segments_pinned += cleanup.segments_pinned;
\t\t\t\tcleanupTotal.camera_dirs_scanned += cleanup.camera_dirs_scanned;
\t\t\t\tcleanupTotal.errors += cleanup.errors;
\t\t\t}
\t\t\tif (!ok) {
\t\t\t\terrors++;
\t\t\t\tcontinue;
\t\t\t}
\t\t\tdeleted++;
\t\t\tclearDeletedEventUi(eventId);
\t\t}

\t\terrors += cleanupTotal.errors;
\t\tstatus->setText(T(deleteMedia ? "EventDock.DeleteMediaSummary" : "EventDock.DeleteSummary")
\t\t\t\t\t.arg(deleted)
\t\t\t\t\t.arg(protectedSkipped)
\t\t\t\t\t.arg(errors)
\t\t\t\t\t.arg(cleanupTotal.segments_deleted)
\t\t\t\t\t.arg(cleanupTotal.segments_pinned));
\t\trefresh();
\t}
'''.replace('\\t', '\t')
dock = dock[:start] + new_actions + dock[end:]

dock_path.write_text(dock, encoding='utf-8')

# Playlist snapshot API.
ph = playlist_h_path.read_text(encoding='utf-8')
ph = replace_once(
    ph,
    'bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state);\n',
    'bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state);\n\n/* Snapshot the current sequence items for UI progress/countdown. The caller owns\n * event_ids_out and must free it with bfree(). */\nbool sr_replay_playlist_snapshot_items(enum sr_replay_bus bus, uint64_t **event_ids_out, size_t *count_out,\n\t\t\t\t       size_t *position_out, bool *angle_sequence_out);\n',
    'playlist snapshot declaration')
playlist_h_path.write_text(ph, encoding='utf-8')

pc = playlist_c_path.read_text(encoding='utf-8')
pc += r'''

bool sr_replay_playlist_snapshot_items(enum sr_replay_bus bus, uint64_t **event_ids_out, size_t *count_out,
                                       size_t *position_out, bool *angle_sequence_out)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist || !event_ids_out || !count_out || !position_out || !angle_sequence_out)
		return false;
	*event_ids_out = NULL;
	*count_out = 0;
	*position_out = 0;
	*angle_sequence_out = false;

	pthread_mutex_lock(&g_mutex);
	if (!playlist->active || !playlist->event_ids || !playlist->count) {
		pthread_mutex_unlock(&g_mutex);
		return false;
	}
	uint64_t *copy = bmalloc(playlist->count * sizeof(*copy));
	if (!copy) {
		pthread_mutex_unlock(&g_mutex);
		return false;
	}
	memcpy(copy, playlist->event_ids, playlist->count * sizeof(*copy));
	*event_ids_out = copy;
	*count_out = playlist->count;
	*position_out = playlist->position;
	*angle_sequence_out = playlist->angle_sequence;
	pthread_mutex_unlock(&g_mutex);
	return true;
}
'''.replace('\\t', '\t')
playlist_c_path.write_text(pc, encoding='utf-8')

# Locale strings.
en = en_path.read_text(encoding='utf-8')
en = replace_once(en, 'EventDock.Protect="Protect"\n', 'EventDock.Protect="Protect"\nEventDock.Protect.Tooltip="Lock selected Event(s) against Delete and Delete + Media. While a saved Event exists, its referenced replay media remains pinned from storage cleanup."\n', 'en protect tooltip')
en = replace_once(en, 'EventDock.DeleteMedia="Delete + Media"\n', 'EventDock.DeleteMedia="Delete + Media"\nEventDock.DeleteSummary="Deleted %1 Event(s); protected skipped %2; errors %3"\nEventDock.DeleteMediaSummary="Deleted %1 Event(s); protected skipped %2; errors %3; media segments removed %4, shared/pinned kept %5"\n', 'en delete summary')
en = replace_once(en, 'EventDock.Timeline.Tooltip="Drag the handle to scrub the selected A/B bus. Drag on the timeline groove to select a range and create a new Event with the same IN/OUT workflow."\n', 'EventDock.Timeline.Tooltip="Drag the handle to scrub the selected A/B bus. Drag on the timeline groove to select a range and create a new Event with the same IN/OUT workflow."\nEventDock.Timeline.PlaylistTooltip="Sequence progress across all Events/angles being played. Green: >15 s remain; yellow: >10 s; red: 10 s or less. Sequence-wide seek is not enabled yet."\n', 'en playlist timeline tooltip')
en_path.write_text(en, encoding='utf-8')

es = es_path.read_text(encoding='utf-8')
if 'EventDock.Protect="' in es and 'EventDock.Protect.Tooltip=' not in es:
    line = next(line for line in es.splitlines(True) if line.startswith('EventDock.Protect='))
    es = es.replace(line, line + 'EventDock.Protect.Tooltip="Bloquea los eventos seleccionados contra Delete y Delete + Media. Mientras exista un evento guardado, sus medios de replay referenciados quedan protegidos de la limpieza de almacenamiento."\n', 1)
if 'EventDock.DeleteMedia="' in es and 'EventDock.DeleteSummary=' not in es:
    line = next(line for line in es.splitlines(True) if line.startswith('EventDock.DeleteMedia='))
    es = es.replace(line, line + 'EventDock.DeleteSummary="Eliminados %1 evento(s); protegidos omitidos %2; errores %3"\nEventDock.DeleteMediaSummary="Eliminados %1 evento(s); protegidos omitidos %2; errores %3; segmentos eliminados %4, compartidos/protegidos conservados %5"\n', 1)
if 'EventDock.Timeline.Tooltip=' in es and 'EventDock.Timeline.PlaylistTooltip=' not in es:
    line = next(line for line in es.splitlines(True) if line.startswith('EventDock.Timeline.Tooltip='))
    es = es.replace(line, line + 'EventDock.Timeline.PlaylistTooltip="Progreso de toda la secuencia de eventos/ángulos. Verde: >15 s restantes; amarillo: >10 s; rojo: 10 s o menos. La búsqueda global de la secuencia aún no está habilitada."\n', 1)
es_path.write_text(es, encoding='utf-8')

print('bulk delete / stale preview / playlist timeline patch applied')
