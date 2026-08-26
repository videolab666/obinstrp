from pathlib import Path
import re


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one literal match, got {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


def regex_once(path, pattern, replacement):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    text, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one regex match, got {count}: {pattern[:80]}")
    p.write_text(text, encoding="utf-8")


DOCK = Path("src/sr-event-dock.cpp")

replace_once(
    DOCK,
    "constexpr size_t ANGLE_PREVIEW_CACHE_EVENTS = 12;\n",
    "constexpr size_t ANGLE_PREVIEW_CACHE_EVENTS = 12;\nconstexpr int TIMELINE_SCALE = 1000000;\n",
)

new_slider = r'''class SrRangeSlider : public QSlider {
public:
	enum class Mode {
		Transport,
		Sequence,
		Edit,
	};

	using RangeHandler = std::function<void(int, int)>;
	using ClickHandler = std::function<void(int)>;

	explicit SrRangeSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) {}

	void setRangeHandler(RangeHandler handler) { rangeHandler = std::move(handler); }
	void setClickHandler(ClickHandler handler) { clickHandler = std::move(handler); }

	void setMode(Mode nextMode)
	{
		if (mode == nextMode)
			return;
		mode = nextMode;
		dragHandle = EditHandle::None;
		if (mode != Mode::Edit)
			hasEditSelection = false;
		update();
	}

	Mode currentMode() const { return mode; }
	bool editingRange() const { return mode == Mode::Edit && dragHandle != EditHandle::None; }

	void setEditSelection(int rangeIn, int rangeOut)
	{
		rangeIn = qBound(minimum(), rangeIn, maximum());
		rangeOut = qBound(minimum(), rangeOut, maximum());
		if (rangeOut <= rangeIn) {
			clearSelection();
			return;
		}
		hasEditSelection = true;
		editIn = rangeIn;
		editOut = rangeOut;
		update();
	}

	void clearSelection()
	{
		dragHandle = EditHandle::None;
		hasEditSelection = false;
		update();
	}

	void setProgressTint(const QColor &color)
	{
		progressTint = color;
		update();
	}

	void clearProgressTint()
	{
		progressTint = QColor();
		update();
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (!isEnabled() || event->button() != Qt::LeftButton) {
			QSlider::mousePressEvent(event);
			return;
		}

		if (mode == Mode::Sequence) {
			event->accept();
			return;
		}

		if (mode != Mode::Edit) {
			QSlider::mousePressEvent(event);
			return;
		}

		const QPoint position = event->position().toPoint();
		if (hasEditSelection) {
			const int inDistance = qAbs(position.x() - pixelForValue(editIn));
			const int outDistance = qAbs(position.x() - pixelForValue(editOut));
			constexpr int hitRadius = 12;
			if (qMin(inDistance, outDistance) <= hitRadius) {
				dragHandle = inDistance <= outDistance ? EditHandle::In : EditHandle::Out;
				event->accept();
				return;
			}
		}

		const int clicked = valueAtX(position.x());
		setValue(clicked);
		if (clickHandler)
			clickHandler(clicked);
		event->accept();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (mode != Mode::Edit || dragHandle == EditHandle::None) {
			QSlider::mouseMoveEvent(event);
			return;
		}

		const int next = valueAtX(event->position().toPoint().x());
		if (dragHandle == EditHandle::In)
			editIn = qBound(minimum(), next, qMax(minimum(), editOut - 1));
		else
			editOut = qBound(qMin(maximum(), editIn + 1), next, maximum());
		event->accept();
		update();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (mode != Mode::Edit || dragHandle == EditHandle::None || event->button() != Qt::LeftButton) {
			QSlider::mouseReleaseEvent(event);
			return;
		}

		const EditHandle released = dragHandle;
		dragHandle = EditHandle::None;
		if (released != EditHandle::None && rangeHandler)
			rangeHandler(editIn, editOut);
		event->accept();
		update();
	}

	void paintEvent(QPaintEvent *event) override
	{
		if (mode != Mode::Edit) {
			QSlider::paintEvent(event);

			if (!progressTint.isValid())
				return;
			QStyleOptionSlider option;
			initStyleOption(&option);
			const QRect groove =
				style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
			const int left = pixelForValue(minimum());
			const int right = pixelForValue(value());
			if (right <= left)
				return;
			QColor tint = progressTint;
			tint.setAlpha(210);
			QPainter painter(this);
			painter.setPen(Qt::NoPen);
			painter.setBrush(tint);
			painter.drawRoundedRect(QRect(left, groove.center().y() - 4, right - left, 8), 3, 3);
			return;
		}

		QStyleOptionSlider option;
		initStyleOption(&option);
		option.subControls = QStyle::SC_SliderGroove | QStyle::SC_SliderTickmarks;
		QPainter painter(this);
		style()->drawComplexControl(QStyle::CC_Slider, &option, &painter, this);

		const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
		if (!hasEditSelection)
			return;

		const int left = pixelForValue(editIn);
		const int right = pixelForValue(editOut);
		QColor highlight = palette().color(QPalette::Highlight);
		QColor fill = highlight;
		fill.setAlpha(105);
		painter.setPen(Qt::NoPen);
		painter.setBrush(fill);
		painter.drawRoundedRect(QRect(left, groove.center().y() - 5, qMax(1, right - left), 10), 3, 3);

		const int playhead = pixelForValue(value());
		if (value() >= editIn && value() <= editOut) {
			QColor playheadColor = palette().color(QPalette::Text);
			playheadColor.setAlpha(190);
			painter.setPen(QPen(playheadColor, 1));
			painter.drawLine(playhead, groove.center().y() - 9, playhead, groove.center().y() + 9);
		}

		auto drawHandle = [&](int x, const QString &label) {
			painter.setPen(QPen(highlight, 2));
			painter.setBrush(highlight);
			painter.drawRoundedRect(QRect(x - 5, groove.center().y() - 8, 10, 16), 3, 3);
			QRect labelRect(x - 18, 0, 36, qMax(11, groove.top()));
			painter.setPen(palette().color(QPalette::Text));
			painter.drawText(labelRect, Qt::AlignHCenter | Qt::AlignBottom, label);
		};
		drawHandle(left, QStringLiteral("IN"));
		drawHandle(right, QStringLiteral("OUT"));
	}

private:
	enum class EditHandle {
		None,
		In,
		Out,
	};

	int valueAtX(int x) const
	{
		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
		const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
		const int sliderMin = groove.x();
		const int sliderMax = groove.right() - handle.width() + 1;
		const int pos = qBound(0, x - sliderMin - handle.width() / 2, qMax(0, sliderMax - sliderMin));
		return QStyle::sliderValueFromPosition(minimum(), maximum(), pos, qMax(1, sliderMax - sliderMin),
						       option.upsideDown);
	}

	int pixelForValue(int sliderValue) const
	{
		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
		const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
		const int span = qMax(1, groove.width() - handle.width());
		return groove.x() + handle.width() / 2 +
		       QStyle::sliderPositionFromValue(minimum(), maximum(), sliderValue, span, option.upsideDown);
	}

	RangeHandler rangeHandler;
	ClickHandler clickHandler;
	Mode mode = Mode::Transport;
	EditHandle dragHandle = EditHandle::None;
	int editIn = 0;
	int editOut = 0;
	bool hasEditSelection = false;
	QColor progressTint;
};'''

regex_once(
    DOCK,
    r"class SrRangeSlider : public QSlider \{.*?\n\};\n\nclass SrEventTable",
    new_slider + "\n\nclass SrEventTable",
)

replace_once(
    DOCK,
    '''\t\tauto *timelineBar = new QHBoxLayout();
\t\ttimelineBar->setSpacing(3);
\t\ttimelineBar->addWidget(new QLabel(T("EventDock.Timeline"), this));
\t\ttimelineSlider = new SrRangeSlider(this);
\t\ttimelineSlider->setRange(0, 10000);
\t\ttimelineSlider->setSingleStep(1);
\t\ttimelineSlider->setPageStep(100);
\t\ttimelineSlider->setEnabled(false);
\t\ttimelineSlider->setToolTip(T("EventDock.Timeline.Tooltip"));
\t\ttimelineBar->addWidget(timelineSlider, 1);
\t\ttimelineTime = new QLabel(QStringLiteral("--:--.--- / --:--.---"), this);
\t\ttimelineTime->setMinimumWidth(130);
\t\ttimelineBar->addWidget(timelineTime);
\t\troot->addLayout(timelineBar);
''',
    '''\t\tauto *timelineBar = new QHBoxLayout();
\t\ttimelineBar->setSpacing(3);
\t\ttimelineBar->addWidget(new QLabel(T("EventDock.Timeline"), this));
\t\ttimelineModeLabel = new QLabel(T("EventDock.Timeline.EditMode"), this);
\t\ttimelineModeLabel->setAlignment(Qt::AlignCenter);
\t\ttimelineModeLabel->setMinimumWidth(58);
\t\ttimelineBar->addWidget(timelineModeLabel);
\t\ttimelineSlider = new SrRangeSlider(this);
\t\ttimelineSlider->setRange(0, TIMELINE_SCALE);
\t\ttimelineSlider->setSingleStep(1);
\t\ttimelineSlider->setPageStep(TIMELINE_SCALE / 100);
\t\ttimelineSlider->setMinimumHeight(30);
\t\ttimelineSlider->setEnabled(false);
\t\ttimelineSlider->setToolTip(T("EventDock.Timeline.EditTooltip"));
\t\ttimelineBar->addWidget(timelineSlider, 1);
\t\ttimelineTime = new QLabel(QStringLiteral("--:--.--- / --:--.---"), this);
\t\ttimelineTime->setMinimumWidth(275);
\t\ttimelineBar->addWidget(timelineTime);
\t\troot->addLayout(timelineBar);
''',
)

replace_once(
    DOCK,
    '''\t\tconnect(busCombo, &QComboBox::currentIndexChanged, this, [this](int) { syncTransportControls(); });
''',
    '''\t\tconnect(busCombo, &QComboBox::currentIndexChanged, this, [this](int) {
\t\t\tsyncTransportControls();
\t\t\tif (!replayPlayoutActive()) {
\t\t\t\teditPreviewEventId = 0;
\t\t\t\tpreviewSelectedEvent(true);
\t\t\t}
\t\t});
\t\tconnect(cameraCombo, &QComboBox::currentIndexChanged, this, [this](int) {
\t\t\tif (!replayPlayoutActive()) {
\t\t\t\teditPreviewEventId = 0;
\t\t\t\tpreviewSelectedEvent(true);
\t\t\t\tsyncTimeline();
\t\t\t}
\t\t});
''',
)

replace_once(
    DOCK,
    '''\t\tconnect(timelineSlider, &QSlider::sliderPressed, this, [this]() {
\t\t\ttimelineDragging = true;
\t\t\tsr_replay_channel_pause(transportBus(), true);
\t\t});
\t\tconnect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });
\t\tconnect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
\t\t\tseekTimeline(timelineSlider->value());
\t\t\ttimelineDragging = false;
\t\t\tsyncTimeline();
\t\t});
\t\ttimelineSlider->setClickHandler([this](int value) {
\t\t\tseekTimeline(value);
\t\t\tsyncTimeline();
\t\t});
\t\ttimelineSlider->setRangeHandler(
\t\t\t[this](int rangeIn, int rangeOut) { createRangeEvent(rangeIn, rangeOut); });
''',
    '''\t\tconnect(timelineSlider, &QSlider::sliderPressed, this, [this]() {
\t\t\ttimelineDragging = true;
\t\t\tsr_replay_channel_pause(timelineTransportBus(), true);
\t\t});
\t\tconnect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });
\t\tconnect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
\t\t\tseekTimeline(timelineSlider->value());
\t\t\ttimelineDragging = false;
\t\t\tsyncTimeline();
\t\t});
\t\ttimelineSlider->setClickHandler([this](int value) {
\t\t\tseekEditTimeline(value);
\t\t\tsyncTimeline();
\t\t});
\t\ttimelineSlider->setRangeHandler(
\t\t\t[this](int rangeIn, int rangeOut) { editSelectedEventRange(rangeIn, rangeOut); });
''',
)

replace_once(
    DOCK,
    '''\t\tconnect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
\t\t\tif (!syncingEventViews)
\t\t\t\tsyncGallerySelectionFromTable();
\t\t\trefreshAngleCoverage();
\t\t});
\t\tconnect(thumbnailList, &QListWidget::itemSelectionChanged, this, [this]() {
\t\t\tif (!syncingEventViews)
\t\t\t\tsyncTableSelectionFromGallery();
\t\t});
''',
    '''\t\tconnect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
\t\t\tif (!syncingEventViews)
\t\t\t\tsyncGallerySelectionFromTable();
\t\t\trefreshAngleCoverage();
\t\t\tif (!replayPlayoutActive()) {
\t\t\t\teditPreviewEventId = 0;
\t\t\t\tQTimer::singleShot(0, this, [this]() {
\t\t\t\t\tpreviewSelectedEvent(false);
\t\t\t\t\tsyncTimeline();
\t\t\t\t});
\t\t\t}
\t\t});
\t\tconnect(thumbnailList, &QListWidget::itemSelectionChanged, this, [this]() {
\t\t\tif (!syncingEventViews)
\t\t\t\tsyncTableSelectionFromGallery();
\t\t\trefreshAngleCoverage();
\t\t\tif (!replayPlayoutActive()) {
\t\t\t\teditPreviewEventId = 0;
\t\t\t\tQTimer::singleShot(0, this, [this]() {
\t\t\t\t\tpreviewSelectedEvent(false);
\t\t\t\t\tsyncTimeline();
\t\t\t\t});
\t\t\t}
\t\t});
''',
)

new_timeline_methods = r'''	bool replayPlayoutActive() const
	{
		enum sr_replay_bus programBus;
		if (sr_replay_take_program_bus(&programBus))
			return true;

		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			const auto bus = static_cast<sr_replay_bus>(i);
			sr_replay_playlist_state playlist = {};
			if (sr_replay_playlist_get_state(bus, &playlist) && playlist.active)
				return true;
			sr_replay_channel_state state = {};
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.playing)
				return true;
		}
		return false;
	}

	enum sr_replay_bus timelineTransportBus() const
	{
		enum sr_replay_bus programBus;
		if (sr_replay_take_program_bus(&programBus))
			return programBus;

		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			const auto bus = static_cast<sr_replay_bus>(i);
			sr_replay_playlist_state playlist = {};
			if (sr_replay_playlist_get_state(bus, &playlist) && playlist.active)
				return bus;
		}
		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			const auto bus = static_cast<sr_replay_bus>(i);
			sr_replay_channel_state state = {};
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.playing)
				return bus;
		}
		return transportBus();
	}

	void updateEditTimelineBounds()
	{
		sr_capture_recording_summary recording = {};
		if (sr_capture_get_recording_summary(&recording) && recording.recording_start_ns) {
			const uint64_t recordingStart = recording.recording_start_ns;
			uint64_t recordingEnd = recordingStart;
			if (recording.recording_duration_ns <= UINT64_MAX - recordingStart)
				recordingEnd = recordingStart + recording.recording_duration_ns;
			if (recording.requested_count) {
				const uint64_t now = obs_get_video_frame_time();
				if (now > recordingEnd)
					recordingEnd = now;
			}
			if (!editTimelineHaveBounds || recordingStart < editTimelineStartNs)
				editTimelineStartNs = recordingStart;
			if (!editTimelineHaveBounds || recordingEnd > editTimelineEndNs)
				editTimelineEndNs = recordingEnd;
			editTimelineHaveBounds = editTimelineEndNs > editTimelineStartNs;
		}

		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (controller && eventId && sr_event_controller_get_event(controller, eventId, &event)) {
			if (!editTimelineHaveBounds) {
				editTimelineStartNs = event.in_ns;
				editTimelineEndNs = event.out_ns;
				editTimelineHaveBounds = event.out_ns > event.in_ns;
			} else {
				if (event.in_ns < editTimelineStartNs)
					editTimelineStartNs = event.in_ns;
				if (event.out_ns > editTimelineEndNs)
					editTimelineEndNs = event.out_ns;
			}
			sr_event_controller_free_event(&event);
		}
	}

	int editTimelineValue(uint64_t timestampNs) const
	{
		if (!editTimelineHaveBounds || editTimelineEndNs <= editTimelineStartNs)
			return 0;
		if (timestampNs <= editTimelineStartNs)
			return 0;
		if (timestampNs >= editTimelineEndNs)
			return TIMELINE_SCALE;
		const uint64_t duration = editTimelineEndNs - editTimelineStartNs;
		return (int)((long double)(timestampNs - editTimelineStartNs) * TIMELINE_SCALE /
			     (long double)duration);
	}

	uint64_t editTimelineTimestamp(int value) const
	{
		if (!editTimelineHaveBounds || editTimelineEndNs <= editTimelineStartNs)
			return 0;
		value = qBound(0, value, TIMELINE_SCALE);
		const uint64_t duration = editTimelineEndNs - editTimelineStartNs;
		const uint64_t offset =
			(uint64_t)((long double)duration * (long double)value / (long double)TIMELINE_SCALE);
		return value >= TIMELINE_SCALE ? editTimelineEndNs : editTimelineStartNs + offset;
	}

	void setTimelineModeBadge(bool editMode)
	{
		if (!timelineModeLabel)
			return;
		if (editMode) {
			timelineModeLabel->setText(T("EventDock.Timeline.EditMode"));
			timelineModeLabel->setStyleSheet(QStringLiteral(
				"font-weight: bold; color: white; background: #2b7a4b; border-radius: 3px; padding: 1px 4px;"));
		} else {
			timelineModeLabel->setText(T("EventDock.Timeline.PlayoutMode"));
			timelineModeLabel->setStyleSheet(QStringLiteral(
				"font-weight: bold; color: white; background: #9b3434; border-radius: 3px; padding: 1px 4px;"));
		}
	}

	bool previewSelectedEvent(bool force)
	{
		if (!controller || replayPlayoutActive())
			return false;
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return false;

		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event))
			return false;

		QStringList candidates;
		auto addCandidate = [&candidates](const QString &candidate) {
			if (!candidate.isEmpty() && !candidates.contains(candidate))
				candidates.append(candidate);
		};

		if (event.preferred_camera_id) {
			char *preferred = nullptr;
			if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &preferred) && preferred)
				addCandidate(QString::fromUtf8(preferred));
			bfree(preferred);
		}
		addCandidate(selectedCamera());
		for (QToolButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_FULL)
				addCandidate(button->property("cameraName").toString());
		}
		for (QToolButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_PARTIAL)
				addCandidate(button->property("cameraName").toString());
		}
		for (const QString &camera : captureCameraNames())
			addCandidate(camera);
		sr_event_controller_free_event(&event);

		const enum sr_replay_bus bus = transportBus();
		for (const QString &camera : candidates) {
			sr_replay_channel_state current = {};
			if (!force && sr_replay_channel_get_state(bus, &current) && current.cued &&
			    current.event_id == eventId && QString::fromUtf8(current.camera_name) == camera) {
				editPreviewEventId = eventId;
				editPreviewBus = bus;
				editPreviewCamera = camera;
				return true;
			}

			const QByteArray cameraUtf8 = camera.toUtf8();
			if (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData()))
				continue;

			editPreviewEventId = eventId;
			editPreviewBus = bus;
			editPreviewCamera = camera;
			if (cameraCombo) {
				const int index = cameraCombo->findData(camera);
				if (index >= 0) {
					const QSignalBlocker blocker(cameraCombo);
					cameraCombo->setCurrentIndex(index);
				}
			}
			refreshTransportStatus();
			syncAngleButtonState();
			return true;
		}

		editPreviewEventId = 0;
		editPreviewCamera.clear();
		return false;
	}

	void syncEditTimeline()
	{
		updateEditTimelineBounds();
		setTimelineModeBadge(true);
		timelineSlider->setMode(SrRangeSlider::Mode::Edit);
		timelineSlider->clearProgressTint();
		timelineSlider->setToolTip(T("EventDock.Timeline.EditTooltip"));
		if (timelineSlider->editingRange())
			return;

		if (!editTimelineHaveBounds || editTimelineEndNs <= editTimelineStartNs) {
			timelineEventId = 0;
			timelineSlider->clearSelection();
			timelineSlider->setEnabled(false);
			timelineSlider->setValue(0);
			timelineTime->setText(T("EventDock.Timeline.NoRecording"));
			return;
		}

		const uint64_t total = editTimelineEndNs - editTimelineStartNs;
		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (!eventId || !controller || !sr_event_controller_get_event(controller, eventId, &event) ||
		    event.out_ns <= event.in_ns) {
			if (event.id)
				sr_event_controller_free_event(&event);
			timelineEventId = 0;
			timelineSlider->clearSelection();
			timelineSlider->setEnabled(false);
			timelineSlider->setValue(TIMELINE_SCALE);
			timelineTime->setText(T("EventDock.Timeline.RecordLength").arg(replayClockText(total)));
			return;
		}

		timelineEventId = event.id;
		const int rangeIn = editTimelineValue(event.in_ns);
		const int rangeOut = editTimelineValue(event.out_ns);
		timelineSlider->setEnabled(true);
		timelineSlider->setEditSelection(rangeIn, qMax(rangeIn + 1, rangeOut));

		int playheadValue = rangeIn;
		sr_replay_channel_state preview = {};
		if (sr_replay_channel_get_state(transportBus(), &preview) && preview.cued && preview.event_id == event.id)
			playheadValue = editTimelineValue(preview.playhead_ns);
		timelineSlider->setValue(qBound(rangeIn, playheadValue, qMax(rangeIn, rangeOut)));

		const uint64_t relativeIn = event.in_ns > editTimelineStartNs ? event.in_ns - editTimelineStartNs : 0;
		const uint64_t relativeOut = event.out_ns > editTimelineStartNs ? event.out_ns - editTimelineStartNs : 0;
		timelineTime->setText(T("EventDock.Timeline.EditRange")
					      .arg(replayClockText(relativeIn))
					      .arg(replayClockText(relativeOut))
					      .arg(replayClockText(event.out_ns - event.in_ns))
					      .arg(replayClockText(total)));
		sr_event_controller_free_event(&event);
	}

	void syncTimeline()
	{
		if (!timelineSlider || !timelineTime)
			return;

		uint64_t sequenceElapsed = 0;
		uint64_t sequenceTotal = 0;
		uint64_t sequenceRemaining = 0;
		if (playlistTimelineProgress(&sequenceElapsed, &sequenceTotal, &sequenceRemaining)) {
			timelineEventId = 0;
			setTimelineModeBadge(false);
			timelineSlider->setMode(SrRangeSlider::Mode::Sequence);
			timelineSlider->setEnabled(true);
			timelineSlider->setToolTip(T("EventDock.Timeline.PlaylistTooltip"));
			const int value =
				(int)((long double)sequenceElapsed * TIMELINE_SCALE / (long double)sequenceTotal);
			timelineSlider->setValue(qBound(0, value, TIMELINE_SCALE));
			if (sequenceRemaining > 15ULL * NS_PER_SECOND)
				timelineSlider->setProgressTint(QColor(QStringLiteral("#2fb34a")));
			else if (sequenceRemaining > 10ULL * NS_PER_SECOND)
				timelineSlider->setProgressTint(QColor(QStringLiteral("#d2a216")));
			else
				timelineSlider->setProgressTint(QColor(QStringLiteral("#d33b3b")));
			timelineTime->setText(replayClockText(sequenceElapsed) + QStringLiteral(" / ") +
					      replayClockText(sequenceTotal) + QStringLiteral("   −") +
					      replayClockText(sequenceRemaining));
			editPreviewEventId = 0;
			editPreviewCamera.clear();
			return;
		}

		if (!replayPlayoutActive()) {
			syncEditTimeline();
			return;
		}

		editPreviewEventId = 0;
		editPreviewCamera.clear();
		setTimelineModeBadge(false);
		timelineSlider->setMode(SrRangeSlider::Mode::Transport);
		timelineSlider->clearProgressTint();
		timelineSlider->setToolTip(T("EventDock.Timeline.Tooltip"));
		const enum sr_replay_bus bus = timelineTransportBus();
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(bus, &state) || !state.cued || state.out_ns <= state.in_ns) {
			timelineEventId = 0;
			timelineSlider->clearSelection();
			timelineSlider->setEnabled(false);
			if (!timelineDragging)
				timelineSlider->setValue(0);
			timelineTime->setText(QStringLiteral("--:--.--- / --:--.---"));
			return;
		}

		if (timelineEventId != state.event_id) {
			timelineEventId = state.event_id;
			timelineSlider->clearSelection();
		}
		timelineSlider->setEnabled(true);
		const uint64_t duration = state.out_ns - state.in_ns;
		const uint64_t position = state.playhead_ns <= state.in_ns    ? 0
					  : state.playhead_ns >= state.out_ns ? duration
									      : state.playhead_ns - state.in_ns;
		if (!timelineDragging) {
			const int sliderValue =
				(int)((long double)position * TIMELINE_SCALE / (long double)duration);
			timelineSlider->setValue(sliderValue);
		}
		timelineTime->setText(replayClockText(position) + QStringLiteral(" / ") + replayClockText(duration));
	}

	void seekEditTimeline(int value)
	{
		if (replayPlayoutActive() || !editTimelineHaveBounds)
			return;
		const uint64_t eventId = selectedEventId();
		if (!eventId || !previewSelectedEvent(false))
			return;

		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event))
			return;
		uint64_t target = editTimelineTimestamp(value);
		if (target < event.in_ns)
			target = event.in_ns;
		if (target > event.out_ns)
			target = event.out_ns;
		const enum sr_replay_bus bus = transportBus();
		sr_replay_channel_seek(bus, target);
		timelineSlider->setValue(editTimelineValue(target));
		sr_event_controller_free_event(&event);
	}

	void seekTimeline(int value)
	{
		if (!replayPlayoutActive()) {
			seekEditTimeline(value);
			return;
		}

		const enum sr_replay_bus bus = timelineTransportBus();
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(bus, &state) || !state.cued || state.out_ns <= state.in_ns)
			return;

		value = qBound(0, value, TIMELINE_SCALE);
		const uint64_t duration = state.out_ns - state.in_ns;
		const uint64_t offset =
			(uint64_t)((long double)duration * (long double)value / (long double)TIMELINE_SCALE);
		const uint64_t target = offset >= duration ? state.out_ns : state.in_ns + offset;
		sr_replay_channel_pause(bus, true);
		sr_replay_channel_seek(bus, target);
		timelineTime->setText(replayClockText(offset > duration ? duration : offset) + QStringLiteral(" / ") +
				      replayClockText(duration));
	}

	void editSelectedEventRange(int rangeIn, int rangeOut)
	{
		if (!controller || replayPlayoutActive() || !editTimelineHaveBounds || rangeOut <= rangeIn) {
			syncTimeline();
			return;
		}

		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (!eventId || !sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			syncTimeline();
			return;
		}

		const uint64_t inNs = editTimelineTimestamp(rangeIn);
		const uint64_t outNs = editTimelineTimestamp(rangeOut);
		struct obs_video_info video = {};
		uint64_t minimumDuration = 1000000ULL;
		if (obs_get_video_info(&video) && video.fps_num && video.fps_den)
			minimumDuration =
				(uint64_t)((long double)NS_PER_SECOND * video.fps_den / (long double)video.fps_num);
		if (outNs <= inNs || outNs - inNs < minimumDuration) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.Timeline.TooShort");
			syncTimeline();
			return;
		}

		sr_event_write update = {};
		update.in_ns = inNs;
		update.out_ns = outNs;
		update.preferred_camera_id = event.preferred_camera_id;
		update.speed_percent = event.speed_percent;
		update.speed_override = event.speed_override;
		update.audio_mode = event.audio_mode;
		update.protected_event = event.protected_event;
		update.played = event.played;
		update.pending = event.pending;
		update.name = event.name;
		update.tag = event.tag;
		const bool ok = sr_event_controller_update_event(controller, eventId, &update);
		sr_event_controller_free_event(&event);
		if (!ok) {
			setStatus("EventDock.Failed");
			syncTimeline();
			return;
		}

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
		status->setText(T("EventDock.Timeline.EditSaved")
					.arg((double)(outNs - inNs) / 1e9, 0, 'f', 3));
		refresh(eventId);
		refreshAngleCoverage();
		previewSelectedEvent(true);
		syncTimeline();
	}
'''

regex_once(
    DOCK,
    r"\tvoid syncTimeline\(\)\n\t\{.*?\n\tvoid jogMoved\(int value\)",
    new_timeline_methods + "\n\tvoid jogMoved(int value)",
)

replace_once(
    DOCK,
    '''\tQLabel *timelineTime = nullptr;
''',
    '''\tQLabel *timelineTime = nullptr;
\tQLabel *timelineModeLabel = nullptr;
''',
)

replace_once(
    DOCK,
    '''\tuint64_t timelineEventId = 0;
\tuint64_t previewTargetEventId = 0;
''',
    '''\tuint64_t timelineEventId = 0;
\tuint64_t editTimelineStartNs = 0;
\tuint64_t editTimelineEndNs = 0;
\tbool editTimelineHaveBounds = false;
\tuint64_t editPreviewEventId = 0;
\tenum sr_replay_bus editPreviewBus = SR_REPLAY_BUS_A;
\tQString editPreviewCamera;
\tuint64_t previewTargetEventId = 0;
''',
)

# Locale additions / tooltip replacement.
replace_once(
    "data/locale/en-US.ini",
    'EventDock.Timeline.Tooltip="Drag the handle to scrub the selected A/B bus. Drag on the timeline groove to select a range and create a new Event with the same IN/OUT workflow."\n',
    'EventDock.Timeline.Tooltip="PLAYOUT mode: scrub the currently active replay bus. Event editing is locked while a replay or playlist owns A/B."\n'
    'EventDock.Timeline.EditMode="EDIT"\n'
    'EventDock.Timeline.PlayoutMode="PLAYOUT"\n'
    'EventDock.Timeline.EditTooltip="EDIT mode: the scale is the full current recording. The selected Event is highlighted; drag its IN/OUT handles to trim it. Selecting an Event cues it off-air on the selected A/B bus for preview. Editing is locked while replay playout is active."\n'
    'EventDock.Timeline.NoRecording="No current recording timeline"\n'
    'EventDock.Timeline.RecordLength="REC %1"\n'
    'EventDock.Timeline.EditRange="IN %1 · OUT %2 · LEN %3 / REC %4"\n'
    'EventDock.Timeline.EditSaved="Event trimmed to %1 s"\n'
    'EventDock.Timeline.TooShort="Event must be at least one video frame long"\n',
)

replace_once(
    "data/locale/es-ES.ini",
    'EventDock.Timeline.Tooltip="Arrastra el control para desplazarte por el bus A/B seleccionado. Arrastra sobre la línea de tiempo para seleccionar un rango y crear un nuevo Event con el mismo flujo IN/OUT."\n',
    'EventDock.Timeline.Tooltip="Modo PLAYOUT: desplázate por el bus de replay activo. La edición del Event queda bloqueada mientras un replay o una lista usa A/B."\n'
    'EventDock.Timeline.EditMode="EDIT"\n'
    'EventDock.Timeline.PlayoutMode="PLAYOUT"\n'
    'EventDock.Timeline.EditTooltip="Modo EDIT: la escala representa toda la grabación actual. El Event seleccionado queda resaltado; arrastra sus marcadores IN/OUT para recortarlo. Al seleccionar un Event se precarga fuera de programa en el bus A/B seleccionado. La edición se bloquea durante el playout."\n'
    'EventDock.Timeline.NoRecording="No hay una línea de tiempo de grabación actual"\n'
    'EventDock.Timeline.RecordLength="REC %1"\n'
    'EventDock.Timeline.EditRange="IN %1 · OUT %2 · DUR %3 / REC %4"\n'
    'EventDock.Timeline.EditSaved="Event recortado a %1 s"\n'
    'EventDock.Timeline.TooShort="El Event debe durar al menos un fotograma"\n',
)

# GitHub-hosted Homebrew recently started requiring explicit trust for the OBS tap.
replace_once(
    ".github/actions/run-clang-format/action.yaml",
    '''        print ::group::Install clang-format-19
        brew install --quiet obsproject/tools/clang-format@19
        print ::endgroup::
''',
    '''        print ::group::Install clang-format-19
        brew tap obsproject/tools
        if brew help trust >/dev/null 2>&1; then
          brew trust obsproject/tools
        fi
        brew install --quiet obsproject/tools/clang-format@19
        print ::endgroup::
''',
)

print("full recording timeline edit patch applied")
