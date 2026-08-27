from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


root = Path('.')
dock_path = root / 'src/sr-event-dock.cpp'
en_path = root / 'data/locale/en-US.ini'
es_path = root / 'data/locale/es-ES.ini'
workflow_path = root / '.github/workflows/validate-program-output.yml'
script_path = root / '.github/scripts/fix-timeline-angle-ui.py'

dock = dock_path.read_text(encoding='utf-8')

# ---------------------------------------------------------------------------
# 1. Timeline interaction: marker tags are explicit draggable handles.
#    Clicking/dragging the timeline itself always controls the red playhead,
#    even when playhead and IN/OUT share the same timestamp.
# ---------------------------------------------------------------------------
old_press = r'''
	void mousePressEvent(QMouseEvent *event) override
	{
		if (!isEnabled() || !haveRecording || event->button() != Qt::LeftButton) {
			QWidget::mousePressEvent(event);
			return;
		}
		setFocus(Qt::MouseFocusReason);
		const int x = event->position().toPoint().x();
		constexpr int hit = 10;
		if (haveSelection && qAbs(x - xFromTimestamp(selectionInNs)) <= hit)
			dragTarget = DragTarget::In;
		else if (haveSelection && qAbs(x - xFromTimestamp(selectionOutNs)) <= hit)
			dragTarget = DragTarget::Out;
		else if (qAbs(x - xFromTimestamp(playheadNs)) <= hit)
			dragTarget = DragTarget::Playhead;
		else
			dragTarget = DragTarget::Playhead;
		applyDrag(timestampFromX(x));
		event->accept();
	}
'''
new_press = r'''
	void mousePressEvent(QMouseEvent *event) override
	{
		if (!isEnabled() || !haveRecording || event->button() != Qt::LeftButton) {
			QWidget::mousePressEvent(event);
			return;
		}
		setFocus(Qt::MouseFocusReason);
		const QPoint position = event->position().toPoint();
		const QRect timeline = timelineRect();
		dragOffsetPixels = 0;
		if (haveSelection && markerHitRect(selectionInNs, false, timeline).contains(position)) {
			dragTarget = DragTarget::In;
			dragOffsetPixels = position.x() - xFromTimestamp(selectionInNs);
			applyDrag(selectionInNs);
		} else if (haveSelection && markerHitRect(selectionOutNs, true, timeline).contains(position)) {
			dragTarget = DragTarget::Out;
			dragOffsetPixels = position.x() - xFromTimestamp(selectionOutNs);
			applyDrag(selectionOutNs);
		} else {
			/* The playhead owns the whole timeline body. IN/OUT can only be
			 * grabbed by their blue label handles, so an overlapping red cursor
			 * never drags an Event boundary by accident. */
			dragTarget = DragTarget::Playhead;
			applyDrag(timestampFromX(position.x()));
		}
		event->accept();
	}
'''
dock = replace_once(dock, old_press, new_press, 'timeline mouse press')

old_move = r'''
	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (dragTarget == DragTarget::None || !(event->buttons() & Qt::LeftButton)) {
			QWidget::mouseMoveEvent(event);
			return;
		}
		applyDrag(timestampFromX(event->position().toPoint().x()));
		event->accept();
	}
'''
new_move = r'''
	void mouseMoveEvent(QMouseEvent *event) override
	{
		const QPoint position = event->position().toPoint();
		if (dragTarget == DragTarget::None || !(event->buttons() & Qt::LeftButton)) {
			const QRect timeline = timelineRect();
			const bool markerHover =
				haveSelection && (markerHitRect(selectionInNs, false, timeline).contains(position) ||
						  markerHitRect(selectionOutNs, true, timeline).contains(position));
			setCursor(markerHover ? Qt::SizeHorCursor : Qt::ArrowCursor);
			QWidget::mouseMoveEvent(event);
			return;
		}
		const int effectiveX = position.x() -
				       ((dragTarget == DragTarget::In || dragTarget == DragTarget::Out) ? dragOffsetPixels : 0);
		applyDrag(timestampFromX(effectiveX));
		event->accept();
	}
'''
dock = replace_once(dock, old_move, new_move, 'timeline mouse move')

dock = replace_once(
    dock,
    '''\t\tconst DragTarget released = dragTarget;\n\t\tdragTarget = DragTarget::None;\n\t\tif ((released == DragTarget::In || released == DragTarget::Out) && haveSelection && rangeHandler)''',
    '''\t\tconst DragTarget released = dragTarget;\n\t\tdragTarget = DragTarget::None;\n\t\tdragOffsetPixels = 0;\n\t\tif ((released == DragTarget::In || released == DragTarget::Out) && haveSelection && rangeHandler)''',
    'timeline release drag offset',
)

old_marker = r'''
	void paintMarker(QPainter &painter, uint64_t timestampNs, const QString &label, bool right,
			 const QRect &timeline)
	{
		if (timestampNs < viewStartNs || timestampNs > viewEndNs)
			return;
		const int x = xFromTimestamp(timestampNs);
		QColor marker = palette().color(QPalette::Highlight);
		painter.setPen(QPen(marker, 2));
		painter.drawLine(x, timeline.top() + 14, x, timeline.bottom());
		QRect tag(right ? x - 34 : x, timeline.top() + 16, 34, 15);
		painter.fillRect(tag, marker);
		painter.setPen(palette().color(QPalette::HighlightedText));
		painter.drawText(tag, Qt::AlignCenter, label);
	}
'''
new_marker = r'''
	QRect markerTagRect(uint64_t timestampNs, bool right, const QRect &timeline) const
	{
		const int x = xFromTimestamp(timestampNs);
		return QRect(right ? x - 38 : x, timeline.top() + 15, 38, 17);
	}

	QRect markerHitRect(uint64_t timestampNs, bool right, const QRect &timeline) const
	{
		return markerTagRect(timestampNs, right, timeline).adjusted(-3, -3, 3, 3);
	}

	void paintMarker(QPainter &painter, uint64_t timestampNs, const QString &label, bool right,
			 const QRect &timeline)
	{
		if (timestampNs < viewStartNs || timestampNs > viewEndNs)
			return;
		const int x = xFromTimestamp(timestampNs);
		QColor marker = palette().color(QPalette::Highlight);
		painter.setPen(QPen(marker, 2));
		painter.drawLine(x, timeline.top() + 14, x, timeline.bottom());
		const QRect tag = markerTagRect(timestampNs, right, timeline);
		painter.fillRect(tag, marker);
		painter.setPen(palette().color(QPalette::HighlightedText));
		painter.drawText(tag, Qt::AlignCenter, label);
	}
'''
dock = replace_once(dock, old_marker, new_marker, 'marker handle geometry')
dock = replace_once(
    dock,
    '\tDragTarget dragTarget = DragTarget::None;\n\tuint64_t recordStartNs = 0;',
    '\tDragTarget dragTarget = DragTarget::None;\n\tint dragOffsetPixels = 0;\n\tuint64_t recordStartNs = 0;',
    'marker drag offset member',
)

# ---------------------------------------------------------------------------
# 2. Event range edits keep the preview bus and red cursor where the operator
#    left them. A DB update must not reset the edit preview to Event IN.
# ---------------------------------------------------------------------------
dock = replace_once(
    dock,
    '''\t\tconst uint64_t eventId = selectedEventId();\n\t\tsr_event_record event = {};\n\t\tif (!eventId || !sr_event_controller_get_event(controller, eventId, &event)) {\n\t\t\tsetStatus("EventDock.Failed");\n\t\t\tsyncTimeline();\n\t\t\treturn;\n\t\t}\n\n\t\tsr_event_write update = {};''',
    '''\t\tconst uint64_t eventId = selectedEventId();\n\t\tsr_event_record event = {};\n\t\tif (!eventId || !sr_event_controller_get_event(controller, eventId, &event)) {\n\t\t\tsetStatus("EventDock.Failed");\n\t\t\tsyncTimeline();\n\t\t\treturn;\n\t\t}\n\n\t\tconst uint64_t preservedCursor = editTimeline ? editTimeline->playheadTimestamp() : inNs;\n\t\tconst enum sr_replay_bus preservedBus = transportBus();\n\t\tsr_replay_channel_state preservedState = {};\n\t\tconst bool keepEditPreview =\n\t\t\tsr_replay_channel_get_state(preservedBus, &preservedState) && preservedState.cued &&\n\t\t\tpreservedState.preview_mode && preservedState.event_id == eventId;\n\t\tconst QString preservedCamera = keepEditPreview ? QString::fromUtf8(preservedState.camera_name) : QString();\n\n\t\tsr_event_write update = {};''',
    'preserve edit preview state',
)

old_after_update = r'''
		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			const auto bus = static_cast<sr_replay_bus>(i);
			sr_replay_channel_state state = {};
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.event_id == eventId)
				sr_replay_channel_clear(bus);
		}
		eventThumbnailCache.erase(eventId);
		anglePreviewCache.erase(eventId);
		previewTargetEventId = 0;
		previewLoadedEventId = 0;
		editPreviewEventId = 0;
		editPreviewCamera.clear();
		status->setText(T("EventDock.Timeline.EditSaved").arg((double)(outNs - inNs) / 1e9, 0, 'f', 3));
		refresh(eventId);
		refreshAngleCoverage();
		previewSelectedEvent(true);
		syncTimeline();
'''
new_after_update = r'''
		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			const auto bus = static_cast<sr_replay_bus>(i);
			sr_replay_channel_state state = {};
			if (!sr_replay_channel_get_state(bus, &state) || !state.cued || state.event_id != eventId)
				continue;
			if (keepEditPreview && bus == preservedBus && state.preview_mode)
				continue;
			sr_replay_channel_clear(bus);
		}
		eventThumbnailCache.erase(eventId);
		anglePreviewCache.erase(eventId);
		previewTargetEventId = 0;
		previewLoadedEventId = 0;
		if (keepEditPreview) {
			editPreviewEventId = eventId;
			editPreviewBus = preservedBus;
			editPreviewCamera = preservedCamera;
			sr_replay_channel_pause(preservedBus, true);
			sr_replay_channel_seek(preservedBus, preservedCursor);
		} else {
			editPreviewEventId = eventId;
			editPreviewBus = preservedBus;
			editPreviewCamera.clear();
			if (editTimeline)
				editTimeline->setPlayhead(preservedCursor);
		}
		status->setText(T("EventDock.Timeline.EditSaved").arg((double)(outNs - inNs) / 1e9, 0, 'f', 3));
		refresh(eventId);
		refreshAngleCoverage();
		if (!keepEditPreview) {
			previewSelectedEvent(true);
			previewSeekTo(preservedCursor);
		}
		syncTimeline();
'''
dock = replace_once(dock, old_after_update, new_after_update, 'preserve cursor after range edit')

# ---------------------------------------------------------------------------
# 3. Make alternate-angle review persistent in EDIT. Clicking an angle remains
#    a preview operation; it does not save Preferred until the explicit star
#    button is pressed.
# ---------------------------------------------------------------------------
old_select_angle = r'''
	void selectAngle(const QString &camera)
	{
		const enum sr_replay_bus bus = transportBus();
		uint64_t eventId = selectedEventId();
		sr_replay_channel_state state = {};
		const bool haveState = sr_replay_channel_get_state(bus, &state);
		if (!eventId && haveState && state.cued)
			eventId = state.event_id;
		if (!eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}

		const QByteArray cameraUtf8 = camera.toUtf8();
		const bool switching = haveState && state.cued && state.event_id == eventId;
		const bool ok = switching ? sr_replay_channel_switch_camera(bus, cameraUtf8.constData())
					  : sr_replay_channel_cue(bus, eventId, cameraUtf8.constData());
		if (!ok) {
			setStatus("EventDock.AngleSwitchFailed");
			refreshAngleCoverage();
			return;
		}

		const int comboIndex = cameraCombo ? cameraCombo->findData(camera) : -1;
		if (comboIndex >= 0)
			cameraCombo->setCurrentIndex(comboIndex);
		status->setText(T("EventDock.AngleSelected")
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
					.arg(camera));
		syncTransportControls();
		refreshAngleCoverage();
	}
'''
new_select_angle = r'''
	void selectAngle(const QString &camera)
	{
		const enum sr_replay_bus bus = transportBus();
		uint64_t eventId = selectedEventId();
		sr_replay_channel_state state = {};
		const bool haveState = sr_replay_channel_get_state(bus, &state);
		if (!eventId && haveState && state.cued)
			eventId = state.event_id;
		if (!eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}

		const QByteArray cameraUtf8 = camera.toUtf8();
		bool ok = false;
		if (!replayPlayoutActive()) {
			updateEditTimelineBounds();
			sr_event_record event = {};
			if (!controller || !sr_event_controller_get_event(controller, eventId, &event)) {
				setStatus("EventDock.Failed");
				return;
			}
			uint64_t target = editTimeline ? editTimeline->playheadTimestamp() : event.in_ns;
			if (!target)
				target = event.in_ns;
			const uint64_t rangeIn = editTimelineHaveBounds ? editTimelineStartNs : event.in_ns;
			const uint64_t rangeOut = editTimelineHaveBounds ? editTimelineEndNs : event.out_ns;
			target = qBound(rangeIn, target, rangeOut);
			if (haveState && state.cued && state.preview_mode && state.event_id == eventId)
				ok = sr_replay_channel_switch_camera(bus, cameraUtf8.constData());
			if (ok) {
				editPreviewEventId = eventId;
				editPreviewBus = bus;
				editPreviewCamera = camera;
				sr_replay_channel_pause(bus, true);
			} else {
				ok = cueEditPreviewAt(camera, eventId, target, rangeIn, rangeOut);
			}
			sr_event_controller_free_event(&event);
		} else {
			const bool switching = haveState && state.cued && state.event_id == eventId;
			ok = switching ? sr_replay_channel_switch_camera(bus, cameraUtf8.constData())
				       : sr_replay_channel_cue(bus, eventId, cameraUtf8.constData());
		}
		if (!ok) {
			setStatus("EventDock.AngleSwitchFailed");
			refreshAngleCoverage();
			return;
		}

		const int comboIndex = cameraCombo ? cameraCombo->findData(camera) : -1;
		if (comboIndex >= 0)
			cameraCombo->setCurrentIndex(comboIndex);
		status->setText(T("EventDock.AngleSelected")
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
					.arg(camera));
		syncTransportControls();
		refreshAngleCoverage();
	}
'''
dock = replace_once(dock, old_select_angle, new_select_angle, 'persistent edit angle preview')

# Keep the manually reviewed angle as the first retry candidate for this Event.
dock = replace_once(
    dock,
    '''\t\tif (event.preferred_camera_id) {\n\t\t\tchar *preferred = nullptr;''',
    '''\t\tif (editPreviewEventId == event.id)\n\t\t\taddCandidate(editPreviewCamera);\n\t\tif (event.preferred_camera_id) {\n\t\t\tchar *preferred = nullptr;''',
    'preserve reviewed angle candidate',
)

# Explain directly on each angle tile that previewing and storing Preferred are
# separate operations.
dock = replace_once(
    dock,
    '''\t\t\tif (camera == preferredCamera)\n\t\t\t\ttooltip += QStringLiteral(" — ") + T("EventDock.Preferred");\n\t\t\tbutton->setProperty("coverageTooltip", tooltip);''',
    '''\t\t\tif (camera == preferredCamera)\n\t\t\t\ttooltip += QStringLiteral(" — ") + T("EventDock.Preferred");\n\t\t\ttooltip += QStringLiteral("\\n") + T("EventDock.AnglePreviewHint");\n\t\t\tbutton->setProperty("coverageTooltip", tooltip);''',
    'angle preview tooltip',
)

# ---------------------------------------------------------------------------
# 4. Add a visible per-Event Angle column immediately before Name.
#    Preferred is explicit; otherwise the row says AUTO and shows the current
#    operator camera as the current fallback, without pretending it is pinned.
# ---------------------------------------------------------------------------
old_table_setup = r'''
		table = new SrEventTable(eventViewStack);
		table->setColumnCount(6);
		table->setHorizontalHeaderLabels({T("EventDock.Column.Id"), T("EventDock.Column.Duration"),
						  T("EventDock.Column.Speed"), T("EventDock.Column.State"),
						  T("EventDock.Column.Name"), T("EventDock.Column.Tag")});
'''
new_table_setup = r'''
		table = new SrEventTable(eventViewStack);
		table->setColumnCount(7);
		table->setHorizontalHeaderLabels({T("EventDock.Column.Id"), T("EventDock.Column.Duration"),
						  T("EventDock.Column.Speed"), T("EventDock.Column.State"),
						  T("EventDock.Column.Angle"), T("EventDock.Column.Name"),
						  T("EventDock.Column.Tag")});
'''
dock = replace_once(dock, old_table_setup, new_table_setup, 'event angle column setup')

dock = replace_once(
    dock,
    '''\t\ttable->horizontalHeader()->setStretchLastSection(true);\n\t\ttable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);''',
    '''\t\ttable->horizontalHeader()->setStretchLastSection(false);\n\t\ttable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);\n\t\ttable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);\n\t\ttable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);''',
    'event table resize modes',
)

dock = replace_once(
    dock,
    '''\t\tconnect(table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {\n\t\t\tif (item && (item->column() == 2 || item->column() == 4 || item->column() == 5))''',
    '''\t\tconnect(table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {\n\t\t\tif (item && (item->column() == 2 || item->column() == 5 || item->column() == 6))''',
    'event table editable columns',
)

old_row_tail = r'''
			auto *state = new QTableWidgetItem(stateText(event));
			state->setFlags(state->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 3, state);
			table->setItem((int)i, 4,
				       new QTableWidgetItem(QString::fromUtf8(event.name ? event.name : "")));
			table->setItem((int)i, 5, new QTableWidgetItem(QString::fromUtf8(event.tag ? event.tag : "")));
'''
new_row_tail = r'''
			auto *state = new QTableWidgetItem(stateText(event));
			state->setFlags(state->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 3, state);

			QString preferredCamera;
			if (event.preferred_camera_id) {
				char *preferredName = nullptr;
				if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &preferredName) &&
				    preferredName)
					preferredCamera = QString::fromUtf8(preferredName);
				bfree(preferredName);
			}
			QString angleText;
			if (!preferredCamera.isEmpty())
				angleText = QStringLiteral("★ ") + preferredCamera;
			else if (!selectedCamera().isEmpty())
				angleText = T("EventDock.AngleAutoCurrent").arg(selectedCamera());
			else
				angleText = T("EventDock.AngleAuto");
			auto *angle = new QTableWidgetItem(angleText);
			angle->setFlags(angle->flags() & ~Qt::ItemIsEditable);
			angle->setToolTip(preferredCamera.isEmpty() ? T("EventDock.AngleAuto.Tooltip")
									      : T("EventDock.AnglePreferred.Tooltip").arg(preferredCamera));
			table->setItem((int)i, 4, angle);
			table->setItem((int)i, 5,
				       new QTableWidgetItem(QString::fromUtf8(event.name ? event.name : "")));
			table->setItem((int)i, 6, new QTableWidgetItem(QString::fromUtf8(event.tag ? event.tag : "")));
'''
dock = replace_once(dock, old_row_tail, new_row_tail, 'event row angle cell')

dock = replace_once(
    dock,
    '''\t\tif (tableRefreshing || !controller || !item ||\n\t\t    (item->column() != 2 && item->column() != 4 && item->column() != 5))''',
    '''\t\tif (tableRefreshing || !controller || !item ||\n\t\t    (item->column() != 2 && item->column() != 5 && item->column() != 6))''',
    'edit event allowed columns',
)
dock = replace_once(
    dock,
    '''\t\tconst QByteArray name = table->item(item->row(), 4)->text().trimmed().toUtf8();\n\t\tconst QByteArray tag = table->item(item->row(), 5)->text().trimmed().toUtf8();''',
    '''\t\tconst QByteArray name = table->item(item->row(), 5)->text().trimmed().toUtf8();\n\t\tconst QByteArray tag = table->item(item->row(), 6)->text().trimmed().toUtf8();''',
    'edit event name tag indexes',
)

dock_path.write_text(dock, encoding='utf-8')

# Locales. Append only new unique keys so the existing translations remain
# stable and missing-key fallbacks are avoided.
en = en_path.read_text(encoding='utf-8')
if 'EventDock.Column.Angle=' not in en:
    en += '''\nEventDock.Column.Angle="Angle"\nEventDock.AngleAuto="AUTO"\nEventDock.AngleAutoCurrent="AUTO · %1"\nEventDock.AngleAuto.Tooltip="No preferred angle is stored for this Event. Manual Cue uses the current Camera selection; automatic sequences may keep the current bus angle and fall back to another camera with usable coverage."\nEventDock.AnglePreferred.Tooltip="This Event is pinned to %1 as its preferred playback angle. Click another angle to preview it; press ★ Preferred to store a different one."\nEventDock.AnglePreviewHint="Click to preview this angle at the current playhead. This does not change the Event's Preferred angle until you press ★ Preferred."\n'''
en_path.write_text(en, encoding='utf-8')

es = es_path.read_text(encoding='utf-8')
if 'EventDock.Column.Angle=' not in es:
    es += '''\nEventDock.Column.Angle="Ángulo"\nEventDock.AngleAuto="AUTO"\nEventDock.AngleAutoCurrent="AUTO · %1"\nEventDock.AngleAuto.Tooltip="Este Event no tiene un ángulo preferido guardado. Cue manual usa la selección Camera actual; las secuencias automáticas pueden mantener el ángulo del bus y usar otra cámara con cobertura si hace falta."\nEventDock.AnglePreferred.Tooltip="Este Event está fijado a %1 como ángulo preferido. Podés previsualizar otro ángulo y guardarlo con ★ Preferred."\nEventDock.AnglePreviewHint="Hacé clic para previsualizar este ángulo en el cursor actual. No cambia Preferred hasta que presiones ★ Preferred."\n'''
es_path.write_text(es, encoding='utf-8')

# Restore the normal development CI and remove this one-shot applicator from
# the product commit.
workflow_path.write_text('''name: Replay Development CI\nrun-name: Replay Development CI — ${{ github.ref_name }} — ${{ github.event_name }}\n\non:\n  workflow_dispatch:\n  push:\n    branches:\n      - feature/hardware-zero-copy\n\npermissions:\n  contents: read\n\nconcurrency:\n  group: replay-development-ci-${{ github.ref }}\n  cancel-in-progress: true\n\n# Full validation for the active replay development branch.\n# Every push is built; workflow_dispatch keeps a manual Run workflow button\n# available from the default branch. PR validation is intentionally left to\n# the repository's normal PR workflow when this branch is eventually retargeted\n# to main, avoiding duplicate push + pull_request builds during development.\njobs:\n  check-format:\n    name: Check Formatting 🔍\n    uses: ./.github/workflows/check-format.yaml\n    permissions:\n      contents: read\n\n  build-project:\n    name: Build Project 🧱\n    uses: ./.github/workflows/build-project.yaml\n    secrets: inherit\n    permissions:\n      contents: read\n''', encoding='utf-8')
script_path.unlink()
