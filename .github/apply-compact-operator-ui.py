from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"anchor not found: {label}")
    return text.replace(old, new, 1)


# --- Operator dock ---------------------------------------------------------
p = Path("src/sr-event-dock.cpp")
s = p.read_text(encoding="utf-8")

s = replace_once(
    s,
    '#include "sr-event-controller.h"\n#include "sr-event-export.h"',
    '#include "sr-event-controller.h"\n#include "sr-dock.h"\n#include "sr-event-export.h"',
    "sr-dock include",
)
s = replace_once(s, "#include <atomic>\n#include <memory>", "#include <atomic>\n#include <functional>\n#include <memory>", "functional include")
s = replace_once(
    s,
    "#include <QMessageBox>\n#include <QPushButton>",
    "#include <QMessageBox>\n#include <QMouseEvent>\n#include <QPainter>\n#include <QPushButton>",
    "mouse/painter includes",
)
s = replace_once(
    s,
    "#include <QStandardPaths>\n#include <QStringList>",
    "#include <QStandardPaths>\n#include <QStyle>\n#include <QStyleOptionSlider>\n#include <QStringList>",
    "style includes",
)
s = replace_once(s, "#include <QTimer>\n#include <QVBoxLayout>", "#include <QTimer>\n#include <QToolButton>\n#include <QVBoxLayout>", "toolbutton include")

range_slider = r'''
class SrRangeSlider : public QSlider {
public:
	using RangeHandler = std::function<void(int, int)>;
	using ClickHandler = std::function<void(int)>;

	explicit SrRangeSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) {}

	void setRangeHandler(RangeHandler handler) { rangeHandler = std::move(handler); }
	void setClickHandler(ClickHandler handler) { clickHandler = std::move(handler); }

	void clearSelection()
	{
		selecting = false;
		hasSelection = false;
		update();
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (!isEnabled() || event->button() != Qt::LeftButton) {
			QSlider::mousePressEvent(event);
			return;
		}

		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
		const QPoint position = event->position().toPoint();
		if (handle.contains(position)) {
			selecting = false;
			QSlider::mousePressEvent(event);
			return;
		}

		selectionStart = valueAtX(position.x());
		selectionEnd = selectionStart;
		selectionMoved = false;
		selecting = true;
		event->accept();
		update();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!selecting) {
			QSlider::mouseMoveEvent(event);
			return;
		}

		selectionEnd = valueAtX(event->position().toPoint().x());
		selectionMoved = selectionMoved || qAbs(selectionEnd - selectionStart) >= 5;
		event->accept();
		update();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (!selecting || event->button() != Qt::LeftButton) {
			QSlider::mouseReleaseEvent(event);
			return;
		}

		selectionEnd = valueAtX(event->position().toPoint().x());
		const int rangeIn = qMin(selectionStart, selectionEnd);
		const int rangeOut = qMax(selectionStart, selectionEnd);
		selecting = false;
		if (selectionMoved && rangeOut > rangeIn) {
			hasSelection = true;
			selectionStart = rangeIn;
			selectionEnd = rangeOut;
			if (rangeHandler)
				rangeHandler(rangeIn, rangeOut);
		} else {
			hasSelection = false;
			setValue(selectionEnd);
			if (clickHandler)
				clickHandler(selectionEnd);
		}
		event->accept();
		update();
	}

	void paintEvent(QPaintEvent *event) override
	{
		QSlider::paintEvent(event);
		if (!selecting && !hasSelection)
			return;

		const int rangeIn = qMin(selectionStart, selectionEnd);
		const int rangeOut = qMax(selectionStart, selectionEnd);
		const int left = pixelForValue(rangeIn);
		const int right = pixelForValue(rangeOut);
		if (right <= left)
			return;

		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
		QColor highlight = palette().color(QPalette::Highlight);
		highlight.setAlpha(115);
		QPainter painter(this);
		painter.setPen(Qt::NoPen);
		painter.setBrush(highlight);
		painter.drawRoundedRect(QRect(left, groove.center().y() - 4, right - left, 8), 3, 3);
	}

private:
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

	int pixelForValue(int value) const
	{
		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
		const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
		const int span = qMax(1, groove.width() - handle.width());
		return groove.x() + handle.width() / 2 +
		       QStyle::sliderPositionFromValue(minimum(), maximum(), value, span, option.upsideDown);
	}

	RangeHandler rangeHandler;
	ClickHandler clickHandler;
	int selectionStart = 0;
	int selectionEnd = 0;
	bool selecting = false;
	bool selectionMoved = false;
	bool hasSelection = false;
};

'''
s = replace_once(s, "class SrEventDock : public QWidget {", range_slider + "class SrEventDock : public QWidget {", "range slider class")

s = replace_once(
    s,
    "\t\tauto *root = new QVBoxLayout(this);\n\t\troot->setContentsMargins(4, 4, 4, 4);\n\t\troot->setSpacing(4);",
    "\t\tauto *root = new QVBoxLayout(this);\n\t\troot->setContentsMargins(2, 2, 2, 2);\n\t\troot->setSpacing(2);\n\t\tsetStyleSheet(QStringLiteral(\n\t\t\t\"QPushButton { padding: 1px 5px; min-height: 20px; }\"\n\t\t\t\"QToolButton { padding: 1px 4px; min-height: 20px; }\"\n\t\t\t\"QComboBox { padding: 1px 4px; min-height: 20px; }\"\n\t\t\t\"QTableWidget::item { padding-top: 0px; padding-bottom: 0px; }\"));",
    "compact root",
)
s = s.replace('"border: 1px solid palette(mid); border-radius: 3px; padding: 5px; }"', '"border: 1px solid palette(mid); border-radius: 3px; padding: 3px; }"', 1)

old_record = '''\t\tauto *recordBar = new QHBoxLayout();
\t\tauto *startRecord = new QPushButton(T("EventDock.RecordStart"), this);
\t\tauto *stopRecord = new QPushButton(T("EventDock.RecordStop"), this);
\t\tstartRecord->setStyleSheet(QStringLiteral("font-weight: bold;"));
\t\tstopRecord->setStyleSheet(QStringLiteral("font-weight: bold;"));
\t\trecordStatus = new QLabel(this);
\t\trecordStatus->setWordWrap(true);
\t\trecordBar->addWidget(startRecord);
\t\trecordBar->addWidget(stopRecord);
\t\trecordBar->addWidget(recordStatus, 1);
\t\troot->addLayout(recordBar);'''
new_record = '''\t\tauto *recordBar = new QHBoxLayout();
\t\trecordBar->setSpacing(3);
\t\trecordToggle = new QToolButton(this);
\t\trecordToggle->setText(QStringLiteral("● REC"));
\t\trecordToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
\t\trecordToggle->setMinimumWidth(58);
\t\trecordToggle->setAutoRaise(false);
\t\tauto *settingsGear = new QToolButton(this);
\t\tsettingsGear->setText(QString::fromUtf8("\\xE2\\x9A\\x99"));
\t\tsettingsGear->setToolTip(T("Dock.Settings"));
\t\tsettingsGear->setAutoRaise(true);
\t\tsettingsGear->setFixedWidth(28);
\t\trecordStatus = new QLabel(this);
\t\trecordStatus->setWordWrap(true);
\t\trecordBar->addWidget(recordToggle);
\t\trecordBar->addWidget(settingsGear);
\t\trecordBar->addWidget(recordStatus, 1);
\t\troot->addLayout(recordBar);'''
s = replace_once(s, old_record, new_record, "record toolbar")

# Compact vertical space without hiding any controls.
s = replace_once(s, "performanceLayout->setContentsMargins(5, 5, 5, 5);\n\t\tperformanceLayout->setSpacing(3);", "performanceLayout->setContentsMargins(3, 3, 3, 3);\n\t\tperformanceLayout->setSpacing(1);", "performance margins")
s = replace_once(s, "performanceTable->setMinimumHeight(88);\n\t\tperformanceTable->setMaximumHeight(160);", "performanceTable->verticalHeader()->setDefaultSectionSize(20);\n\t\tperformanceTable->setMinimumHeight(72);\n\t\tperformanceTable->setMaximumHeight(120);", "performance height")
s = replace_once(s, "label->setMinimumHeight(28);", "label->setMinimumHeight(22);", "program height")
s = replace_once(s, "table->verticalHeader()->setVisible(false);", "table->verticalHeader()->setVisible(false);\n\t\ttable->verticalHeader()->setDefaultSectionSize(22);", "event row height")
s = replace_once(s, "angleGrid->setHorizontalSpacing(4);\n\t\tangleGrid->setVerticalSpacing(3);", "angleGrid->setHorizontalSpacing(2);\n\t\tangleGrid->setVerticalSpacing(1);", "angle spacing")
s = s.replace("cueBar->addSpacing(12);", "cueBar->addSpacing(6);", 1)
s = s.replace("jogShuttleBar->addSpacing(8);", "jogShuttleBar->addSpacing(4);", 1)
s = s.replace("takeBar->addSpacing(10);", "takeBar->addSpacing(5);", 1)
s = s.replace("timelineTime->setMinimumWidth(150);", "timelineTime->setMinimumWidth(130);", 1)
s = s.replace("shuttleValue->setMinimumWidth(48);", "shuttleValue->setMinimumWidth(42);", 1)

for layout_name in ["programBar", "markBar", "actionBar", "exportBar", "cueBar", "angleHeader", "timelineBar", "jogShuttleBar", "takeBar"]:
    old = f"\\t\\tauto *{layout_name} = new QHBoxLayout();"
    # Work with actual tabs, not escaped text.
    old = old.replace("\\t", "\t")
    new = old + f"\n\t\t{layout_name}->setSpacing(3);"
    s = replace_once(s, old, new, f"{layout_name} spacing")

s = replace_once(s, "timelineSlider = new QSlider(Qt::Horizontal, this);", "timelineSlider = new SrRangeSlider(this);", "range timeline instance")

s = replace_once(
    s,
    '''\t\tconnect(startRecord, &QPushButton::clicked, this, [this]() { setAllRecording(true); });
\t\tconnect(stopRecord, &QPushButton::clicked, this, [this]() { setAllRecording(false); });''',
    '''\t\tconnect(recordToggle, &QToolButton::clicked, this, [this]() { toggleAllRecording(); });
\t\tconnect(settingsGear, &QToolButton::clicked, this, []() { sr_dock_open_settings(); });''',
    "record connections",
)

range_connections = '''\t\ttimelineSlider->setClickHandler([this](int value) {
\t\t\tseekTimeline(value);
\t\t\tsyncTimeline();
\t\t});
\t\ttimelineSlider->setRangeHandler([this](int rangeIn, int rangeOut) { createRangeEvent(rangeIn, rangeOut); });
'''
s = replace_once(
    s,
    '''\t\tconnect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
\t\t\tseekTimeline(timelineSlider->value());
\t\t\ttimelineDragging = false;
\t\t\tsyncTimeline();
\t\t});
''',
    '''\t\tconnect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
\t\t\tseekTimeline(timelineSlider->value());
\t\t\ttimelineDragging = false;
\t\t\tsyncTimeline();
\t\t});
''' + range_connections,
    "range timeline connections",
)

record_helpers = '''\tvoid updateRecordToggle(const sr_capture_recording_summary *summary)
\t{
\t\tif (!recordToggle)
\t\t\treturn;
\t\tconst bool requested = summary && summary->requested_count > 0;
\t\tconst bool active = summary && summary->active_count > 0;
\t\trecordToggle->setToolTip(T(requested ? "EventDock.RecordStop" : "EventDock.RecordStart"));
\t\tif (active) {
\t\t\trecordToggle->setStyleSheet(QStringLiteral(
\t\t\t\t"QToolButton { color: #ff4040; font-weight: bold; border: 1px solid #7f3030; border-radius: 3px; }"));
\t\t} else if (requested) {
\t\t\trecordToggle->setStyleSheet(QStringLiteral(
\t\t\t\t"QToolButton { color: #d8a000; font-weight: bold; border: 1px solid #806b2a; border-radius: 3px; }"));
\t\t} else {
\t\t\trecordToggle->setStyleSheet(QStringLiteral(
\t\t\t\t"QToolButton { color: #8a8a8a; font-weight: bold; border: 1px solid #5a5a5a; border-radius: 3px; }"));
\t\t}
\t}

\tvoid toggleAllRecording()
\t{
\t\tsr_capture_recording_summary summary = {};
\t\tconst bool requested = sr_capture_get_recording_summary(&summary) && summary.requested_count > 0;
\t\tsetAllRecording(!requested);
\t}

'''
s = replace_once(s, "\tvoid setAllRecording(bool enabled)\n\t{", record_helpers + "\tvoid setAllRecording(bool enabled)\n\t{", "record helpers")

s = replace_once(
    s,
    '''\t\tif (!sr_capture_get_recording_summary(&summary) || !summary.camera_count) {
\t\t\trecordStatus->setText(T("EventDock.RecordNoCameras"));''',
    '''\t\tif (!sr_capture_get_recording_summary(&summary) || !summary.camera_count) {
\t\t\tupdateRecordToggle(nullptr);
\t\t\trecordStatus->setText(T("EventDock.RecordNoCameras"));''',
    "record no camera state",
)
s = replace_once(
    s,
    '''\t\tif (summary.reserve_blocked_count) {''',
    '''\t\tupdateRecordToggle(&summary);

\t\tif (summary.reserve_blocked_count) {''',
    "record toggle refresh",
)

# Range selection is a direct mouse equivalent of current IN/OUT: create a
# metadata Event inside the already cued historical interval.
range_method = '''\n\tvoid createRangeEvent(int rangeIn, int rangeOut)
\t{
\t\tif (!controller || rangeOut <= rangeIn)
\t\t\treturn;

\t\tsr_replay_channel_state state = {};
\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued || state.out_ns <= state.in_ns) {
\t\t\tsetStatus("EventDock.NoCue");
\t\t\treturn;
\t\t}

\t\trangeIn = qBound(0, rangeIn, 10000);
\t\trangeOut = qBound(0, rangeOut, 10000);
\t\tconst uint64_t duration = state.out_ns - state.in_ns;
\t\tconst uint64_t inOffset = (uint64_t)((long double)duration * (long double)rangeIn / 10000.0L);
\t\tconst uint64_t outOffset = (uint64_t)((long double)duration * (long double)rangeOut / 10000.0L);
\t\tconst uint64_t inNs = state.in_ns + inOffset;
\t\tconst uint64_t outNs = rangeOut >= 10000 ? state.out_ns : state.in_ns + outOffset;
\t\tif (outNs <= inNs || !sr_event_controller_mark_in(controller, inNs)) {
\t\t\tsetStatus("EventDock.Failed");
\t\t\treturn;
\t\t}

\t\tuint64_t eventId = 0;
\t\tif (!sr_event_controller_mark_out(controller, outNs, &eventId)) {
\t\t\tsr_event_controller_cancel_mark_in(controller);
\t\t\tsetStatus("EventDock.Failed");
\t\t\treturn;
\t\t}
\t\tsetCreatedStatus(eventId);
\t\trefresh(eventId);
\t}
'''
s = replace_once(s, "\n\tvoid jogMoved(int value)\n\t{", range_method + "\n\tvoid jogMoved(int value)\n\t{", "range event method")

s = replace_once(
    s,
    '''\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued ||
\t\t    state.out_ns <= state.in_ns) {
\t\t\ttimelineSlider->setEnabled(false);''',
    '''\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued ||
\t\t    state.out_ns <= state.in_ns) {
\t\t\ttimelineEventId = 0;
\t\t\ttimelineSlider->clearSelection();
\t\t\ttimelineSlider->setEnabled(false);''',
    "timeline clear without cue",
)
s = replace_once(
    s,
    '''\t\ttimelineSlider->setEnabled(true);
\t\tconst uint64_t duration = state.out_ns - state.in_ns;''',
    '''\t\tif (timelineEventId != state.event_id) {
\t\t\ttimelineEventId = state.event_id;
\t\t\ttimelineSlider->clearSelection();
\t\t}
\t\ttimelineSlider->setEnabled(true);
\t\tconst uint64_t duration = state.out_ns - state.in_ns;''',
    "timeline event selection lifetime",
)

# Program/Cue badges remain visually prominent but no longer consume 28+ px.
s = s.replace("border-radius: 3px; padding: 4px;", "border-radius: 3px; padding: 2px;", 3)

s = replace_once(s, "\tQSlider *timelineSlider = nullptr;", "\tSrRangeSlider *timelineSlider = nullptr;", "timeline member type")
s = replace_once(s, "\tQLabel *recordStatus = nullptr;", "\tQToolButton *recordToggle = nullptr;\n\tQLabel *recordStatus = nullptr;", "record member")
s = replace_once(s, "\tuint64_t previewTargetEventId = 0;", "\tuint64_t timelineEventId = 0;\n\tuint64_t previewTargetEventId = 0;", "timeline event member")

p.write_text(s, encoding="utf-8")


# --- Unified dock: expose the existing settings dialog to operator tab ------
p = Path("src/sr-dock.h")
s = p.read_text(encoding="utf-8")
s = replace_once(
    s,
    "void sr_dock_register(struct sr_event_controller *controller);\n",
    "void sr_dock_register(struct sr_event_controller *controller);\n\n/* Opens the existing unified Pitel Instant Replay settings dialog. UI thread only. */\nvoid sr_dock_open_settings(void);\n",
    "settings API declaration",
)
p.write_text(s, encoding="utf-8")

p = Path("src/sr-dock.cpp")
s = p.read_text(encoding="utf-8")
s = replace_once(
    s,
    "\tvoid markPlayed(const QString &path)\n\t{",
    "\tvoid showSettings() { openSettings(); }\n\n\tvoid markPlayed(const QString &path)\n\t{",
    "public settings wrapper",
)
s = replace_once(
    s,
    "\nvoid sr_dock_mark_played(const char *path)\n{",
    "\nvoid sr_dock_open_settings(void)\n{\n\tif (g_dock)\n\t\tg_dock->showSettings();\n}\n\nvoid sr_dock_mark_played(const char *path)\n{",
    "settings API implementation",
)
p.write_text(s, encoding="utf-8")


# --- Locale help text -------------------------------------------------------
for locale in ["data/locale/en-US.ini", "data/locale/es-ES.ini"]:
    p = Path(locale)
    s = p.read_text(encoding="utf-8")
    if locale.endswith("en-US.ini"):
        old = 'EventDock.Timeline.Tooltip="Drag to scrub the selected A/B bus inside the cued Event; scrubbing pauses the transport"'
        new = 'EventDock.Timeline.Tooltip="Drag the handle to scrub the selected A/B bus. Drag on the timeline groove to select a range and create a new Event with the same IN/OUT workflow."'
    else:
        old = 'EventDock.Timeline.Tooltip="Arrastra para desplazarte por el bus A/B seleccionado dentro del evento cargado; el desplazamiento pausa el transporte"'
        new = 'EventDock.Timeline.Tooltip="Arrastra el control para desplazarte por el bus A/B. Arrastra sobre la pista de la línea de tiempo para seleccionar un rango y crear un nuevo evento con el mismo flujo IN/OUT."'
    if old not in s:
        raise RuntimeError(f"timeline tooltip not found in {locale}")
    s = s.replace(old, new, 1)
    p.write_text(s, encoding="utf-8")
