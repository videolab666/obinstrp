from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


root = Path('.')
dock_path = root / 'src/sr-event-dock.cpp'
channel_h_path = root / 'src/sr-replay-channel.h'
channel_c_path = root / 'src/sr-replay-channel.c'
workflow_path = root / '.github/workflows/validate-program-output.yml'
script_path = root / '.github/scripts/apply-professional-timeline.py'

# ---------------------------------------------------------------------------
# Replay channel: add a transient preview-range mode. This lets the EDIT
# timeline scrub anywhere in recorded media without changing the Event itself.
# ---------------------------------------------------------------------------
channel_h = channel_h_path.read_text(encoding='utf-8')
channel_h = replace_once(
    channel_h,
    '\tbool partial_coverage;\n\tbool decoder_open;',
    '\tbool partial_coverage;\n\tbool preview_mode;\n\tbool decoder_open;',
    'channel state preview flag',
)
channel_h = replace_once(
    channel_h,
    'bool sr_replay_channel_cue(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name);\n',
    '''bool sr_replay_channel_cue(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name);\n\n/* Cues a transient EDIT preview range. Unlike a normal Event cue, the visible\n * transport bounds may span the recording around the requested playhead. The\n * Event database is not modified and taking the bus should re-cue the Event\n * normally first. */\nbool sr_replay_channel_cue_preview(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name,\n\t\t\t\t   uint64_t range_in_ns, uint64_t range_out_ns, uint64_t playhead_ns);\n''',
    'preview cue declaration',
)
channel_h_path.write_text(channel_h, encoding='utf-8')

channel_c = channel_c_path.read_text(encoding='utf-8')
channel_c = replace_once(
    channel_c,
    '\tbool partial_coverage;\n\tbool need_frame;',
    '\tbool partial_coverage;\n\tbool preview_mode;\n\tbool need_frame;',
    'channel preview member',
)
channel_c = replace_once(
    channel_c,
    '\tchannel->partial_coverage = false;\n\tchannel->need_frame = false;',
    '\tchannel->partial_coverage = false;\n\tchannel->preview_mode = false;\n\tchannel->need_frame = false;',
    'clear preview state',
)
channel_c = replace_once(
    channel_c,
    '\tstate->partial_coverage = channel->partial_coverage;\n\tif (channel->player) {',
    '\tstate->partial_coverage = channel->partial_coverage;\n\tstate->preview_mode = channel->preview_mode;\n\tif (channel->player) {',
    'publish preview state',
)

preview_function = r'''
bool sr_replay_channel_cue_preview(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name,
				   uint64_t range_in_ns, uint64_t range_out_ns, uint64_t playhead_ns)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel || !event_id || !camera_name || !*camera_name || range_out_ns <= range_in_ns)
		return false;

	if (playhead_ns < range_in_ns)
		playhead_ns = range_in_ns;
	if (playhead_ns > range_out_ns)
		playhead_ns = range_out_ns;

	struct sr_replay_coverage_info coverage = {0};
	if (!sr_replay_coverage_query_at(camera_name, range_in_ns, range_out_ns, playhead_ns, &coverage) ||
	    coverage.coverage == SR_REPLAY_COVERAGE_NONE)
		return false;

	uint64_t first_ns = 0;
	uint64_t last_ns = 0;
	struct sr_disk_player *player = open_camera_player(camera_name, &first_ns, &last_ns);
	if (!player)
		return false;

	uint64_t target_ns = playhead_ns;
	if (target_ns < coverage.playable_in_ns)
		target_ns = coverage.playable_in_ns;
	if (target_ns > coverage.playable_out_ns)
		target_ns = coverage.playable_out_ns;

	uint64_t target_media_ns = 0;
	AVFrame *probe_frame = NULL;
	if (!global_to_camera_media(target_ns, coverage.sync_offset_ns, &target_media_ns) ||
	    !sr_disk_player_decode_at(player, target_media_ns, &probe_frame, NULL) || !probe_frame) {
		av_frame_free(&probe_frame);
		sr_disk_player_destroy(player);
		return false;
	}
	av_frame_free(&probe_frame);

	char *new_camera_name = bstrdup(camera_name);
	if (!new_camera_name) {
		sr_disk_player_destroy(player);
		return false;
	}

	double speed = sr_replay_channel_get_controller_speed();
	struct sr_event_record event = {0};
	if (g_channels && g_channels->events && sr_event_controller_get_event(g_channels->events, event_id, &event)) {
		const bool override = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL && event.speed_override;
		if (sr_config_get_replay_speed_policy() != SR_REPLAY_SPEED_GLOBAL || override)
			speed = event.speed_percent;
		sr_event_controller_free_event(&event);
	}

	pthread_mutex_lock(&channel->mutex);
	const enum sr_replay_audio_mode audio_mode = channel->audio_mode;
	clear_locked(channel);
	channel->audio_mode = audio_mode;
	channel->player = player;
	channel->camera_name = new_camera_name;
	channel->event_id = event_id;
	channel->event_in_ns = range_in_ns;
	channel->event_out_ns = range_out_ns;
	channel->in_ns = coverage.playable_in_ns;
	channel->out_ns = coverage.playable_out_ns;
	channel->playhead_ns = target_ns;
	channel->sync_offset_ns = coverage.sync_offset_ns;
	channel->speed_percent = speed;
	channel->cued = true;
	channel->playing = false;
	channel->paused = true;
	channel->partial_coverage = coverage.coverage != SR_REPLAY_COVERAGE_FULL;
	channel->preview_mode = true;
	channel->need_frame = true;
	pthread_mutex_unlock(&channel->mutex);

	blog(LOG_DEBUG, "Pitel Instant Replay: EDIT preview Event %llu on bus %c, camera '%s', %.3f s",
	     (unsigned long long)event_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', camera_name,
	     (double)(target_ns - range_in_ns) / 1e9);
	return true;
}

'''
anchor = 'bool sr_replay_channel_switch_camera(enum sr_replay_bus bus, const char *camera_name)\n'
if preview_function.strip() not in channel_c:
    channel_c = replace_once(channel_c, anchor, preview_function + anchor, 'insert preview cue implementation')
channel_c_path.write_text(channel_c, encoding='utf-8')

# ---------------------------------------------------------------------------
# Dock: professional EDIT timeline widget with a real time-domain viewport.
# ---------------------------------------------------------------------------
dock = dock_path.read_text(encoding='utf-8')
dock = replace_once(dock, '#include <cstring>\n#include <atomic>', '#include <cstring>\n#include <cmath>\n#include <atomic>', 'cmath include')
dock = replace_once(dock, '#include <QItemSelectionModel>\n#include <QLabel>', '#include <QItemSelectionModel>\n#include <QKeyEvent>\n#include <QLabel>', 'key include')
dock = replace_once(dock, '#include <QWidget>\n', '#include <QWheelEvent>\n#include <QWidget>\n', 'wheel include')

professional_timeline_class = r'''
class SrReplayTimeline : public QWidget {
public:
	enum class DragTarget {
		None,
		Playhead,
		In,
		Out,
	};

	enum class Command {
		TogglePlay,
		GotoIn,
		GotoOut,
		SetIn,
		SetOut,
		StepBack,
		StepForward,
		StepBackFast,
		StepForwardFast,
	};

	using ScrubHandler = std::function<void(uint64_t, DragTarget)>;
	using RangeHandler = std::function<void(uint64_t, uint64_t)>;
	using CommandHandler = std::function<void(Command)>;

	explicit SrReplayTimeline(QWidget *parent = nullptr) : QWidget(parent)
	{
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);
		setMinimumHeight(72);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setScrubHandler(ScrubHandler handler) { scrubHandler = std::move(handler); }
	void setRangeHandler(RangeHandler handler) { rangeHandler = std::move(handler); }
	void setCommandHandler(CommandHandler handler) { commandHandler = std::move(handler); }

	void setMinimumDuration(uint64_t ns)
	{
		minimumDurationNs = std::max<uint64_t>(1, ns);
	}

	void setRecordingBounds(uint64_t startNs, uint64_t endNs)
	{
		if (!startNs || endNs <= startNs) {
			haveRecording = false;
			recordStartNs = 0;
			recordEndNs = 0;
			viewStartNs = 0;
			viewEndNs = 0;
			update();
			return;
		}

		const bool first = !haveRecording;
		recordStartNs = startNs;
		recordEndNs = endNs;
		haveRecording = true;
		if (first || !zoomLocked || viewEndNs <= viewStartNs)
			fitView();
		else
			clampView();
		update();
	}

	void setSelection(uint64_t inNs, uint64_t outNs)
	{
		if (!haveRecording || outNs <= inNs) {
			haveSelection = false;
			update();
			return;
		}
		haveSelection = true;
		selectionInNs = inNs;
		selectionOutNs = outNs;
		if (zoomLocked && (selectionOutNs < viewStartNs || selectionInNs > viewEndNs)) {
			const uint64_t midpoint = selectionInNs + (selectionOutNs - selectionInNs) / 2;
			centerView(midpoint);
		}
		update();
	}

	void clearSelection()
	{
		haveSelection = false;
		dragTarget = DragTarget::None;
		update();
	}

	void setPlayhead(uint64_t timestampNs)
	{
		if (dragTarget != DragTarget::None)
			return;
		playheadNs = clampToRecording(timestampNs);
		update();
	}

	uint64_t playheadTimestamp() const { return playheadNs; }
	uint64_t selectionIn() const { return selectionInNs; }
	uint64_t selectionOut() const { return selectionOutNs; }
	bool hasSelection() const { return haveSelection; }
	bool isDragging() const { return dragTarget != DragTarget::None; }
	bool isZoomed() const { return zoomLocked; }

	void fitView()
	{
		if (!haveRecording)
			return;
		zoomLocked = false;
		viewStartNs = recordStartNs;
		viewEndNs = recordEndNs;
		update();
	}

	void centerOnLive()
	{
		if (!haveRecording)
			return;
		if (!zoomLocked) {
			playheadNs = recordEndNs;
			update();
			return;
		}
		const uint64_t span = viewSpan();
		viewEndNs = recordEndNs;
		viewStartNs = span >= recordEndNs - recordStartNs ? recordStartNs : recordEndNs - span;
		playheadNs = recordEndNs;
		update();
	}

	double zoomFactor() const
	{
		if (!haveRecording || viewSpan() == 0)
			return 1.0;
		return (double)(recordEndNs - recordStartNs) / (double)viewSpan();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, false);
		painter.fillRect(rect(), palette().window());
		if (!haveRecording || viewEndNs <= viewStartNs) {
			painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
			painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No recording"));
			return;
		}

		const QRect timeline = timelineRect();
		QColor track = palette().color(QPalette::Base);
		track.setAlpha(180);
		painter.fillRect(timeline, track);
		painter.setPen(palette().color(QPalette::Mid));
		painter.drawRect(timeline.adjusted(0, 0, -1, -1));

		paintRuler(painter, timeline);

		if (haveSelection) {
			const int left = xFromTimestamp(selectionInNs);
			const int right = xFromTimestamp(selectionOutNs);
			const int clippedLeft = qBound(timeline.left(), left, timeline.right());
			const int clippedRight = qBound(timeline.left(), right, timeline.right());
			if (clippedRight > clippedLeft) {
				QColor fill = palette().color(QPalette::Highlight);
				fill.setAlpha(95);
				painter.fillRect(QRect(clippedLeft, timeline.top() + 18, clippedRight - clippedLeft,
						       timeline.height() - 19), fill);
			}
			paintMarker(painter, selectionInNs, QStringLiteral("IN"), false, timeline);
			paintMarker(painter, selectionOutNs, QStringLiteral("OUT"), true, timeline);
		}

		if (playheadNs >= viewStartNs && playheadNs <= viewEndNs) {
			const int x = xFromTimestamp(playheadNs);
			QColor red(QStringLiteral("#e24a4a"));
			painter.setPen(QPen(red, 2));
			painter.drawLine(x, timeline.top(), x, timeline.bottom());
			QPolygon triangle;
			triangle << QPoint(x - 5, timeline.top()) << QPoint(x + 5, timeline.top())
				 << QPoint(x, timeline.top() + 7);
			painter.setBrush(red);
			painter.setPen(Qt::NoPen);
			painter.drawPolygon(triangle);
		}

		if (recordEndNs >= viewStartNs && recordEndNs <= viewEndNs) {
			const int liveX = xFromTimestamp(recordEndNs);
			QColor live = palette().color(QPalette::Highlight);
			live.setAlpha(150);
			painter.setPen(QPen(live, 1, Qt::DashLine));
			painter.drawLine(liveX, timeline.top() + 18, liveX, timeline.bottom());
		}

		painter.setPen(palette().color(QPalette::Text));
		const QString zoomText = zoomLocked ? QStringLiteral("%1×").arg(zoomFactor(), 0, 'f', 1)
							    : QStringLiteral("FIT");
		painter.drawText(QRect(timeline.right() - 70, 1, 66, 15), Qt::AlignRight | Qt::AlignVCenter,
				 zoomText);
	}

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

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (dragTarget == DragTarget::None || !(event->buttons() & Qt::LeftButton)) {
			QWidget::mouseMoveEvent(event);
			return;
		}
		applyDrag(timestampFromX(event->position().toPoint().x()));
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton || dragTarget == DragTarget::None) {
			QWidget::mouseReleaseEvent(event);
			return;
		}
		const DragTarget released = dragTarget;
		dragTarget = DragTarget::None;
		if ((released == DragTarget::In || released == DragTarget::Out) && haveSelection && rangeHandler)
			rangeHandler(selectionInNs, selectionOutNs);
		event->accept();
		update();
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && haveRecording) {
			fitView();
			event->accept();
			return;
		}
		QWidget::mouseDoubleClickEvent(event);
	}

	void wheelEvent(QWheelEvent *event) override
	{
		if (!haveRecording) {
			QWidget::wheelEvent(event);
			return;
		}
		int delta = event->angleDelta().y();
		if (!delta)
			delta = event->angleDelta().x();
		if (!delta) {
			event->ignore();
			return;
		}
		const double steps = (double)delta / 120.0;
		if (event->modifiers().testFlag(Qt::ShiftModifier)) {
			if (zoomLocked) {
				const int direction = steps > 0.0 ? -1 : 1;
				const uint64_t amount = std::max<uint64_t>(minimumDurationNs,
											      (uint64_t)((long double)viewSpan() * 0.12L *
													 std::abs(steps)));
				panView(direction, amount);
			}
		} else {
			zoomView(steps);
		}
		event->accept();
	}

	void keyPressEvent(QKeyEvent *event) override
	{
		if (!commandHandler) {
			QWidget::keyPressEvent(event);
			return;
		}
		Command command;
		bool handled = true;
		switch (event->key()) {
		case Qt::Key_Space:
			command = Command::TogglePlay;
			break;
		case Qt::Key_I:
			command = Command::SetIn;
			break;
		case Qt::Key_O:
			command = Command::SetOut;
			break;
		case Qt::Key_BracketLeft:
			command = Command::GotoIn;
			break;
		case Qt::Key_BracketRight:
			command = Command::GotoOut;
			break;
		case Qt::Key_Left:
			command = event->modifiers().testFlag(Qt::ShiftModifier) ? Command::StepBackFast
											       : Command::StepBack;
			break;
		case Qt::Key_Right:
			command = event->modifiers().testFlag(Qt::ShiftModifier) ? Command::StepForwardFast
											       : Command::StepForward;
			break;
		default:
			handled = false;
			break;
		}
		if (!handled) {
			QWidget::keyPressEvent(event);
			return;
		}
		commandHandler(command);
		event->accept();
	}

private:
	QRect timelineRect() const { return rect().adjusted(8, 18, -8, -6); }

	uint64_t viewSpan() const { return viewEndNs > viewStartNs ? viewEndNs - viewStartNs : 0; }

	uint64_t clampToRecording(uint64_t timestampNs) const
	{
		if (!haveRecording)
			return timestampNs;
		if (timestampNs < recordStartNs)
			return recordStartNs;
		if (timestampNs > recordEndNs)
			return recordEndNs;
		return timestampNs;
	}

	int xFromTimestamp(uint64_t timestampNs) const
	{
		const QRect timeline = timelineRect();
		if (viewEndNs <= viewStartNs)
			return timeline.left();
		const long double normalized = ((long double)timestampNs - (long double)viewStartNs) /
						       (long double)(viewEndNs - viewStartNs);
		return timeline.left() + (int)std::llround(normalized * (long double)timeline.width());
	}

	uint64_t timestampFromX(int x) const
	{
		const QRect timeline = timelineRect();
		if (viewEndNs <= viewStartNs || timeline.width() <= 0)
			return viewStartNs;
		const int clamped = qBound(timeline.left(), x, timeline.right());
		const long double normalized =
			(long double)(clamped - timeline.left()) / (long double)std::max(1, timeline.width());
		const uint64_t offset = (uint64_t)((long double)(viewEndNs - viewStartNs) * normalized);
		return clampToRecording(viewStartNs + offset);
	}

	void applyDrag(uint64_t timestampNs)
	{
		timestampNs = clampToRecording(timestampNs);
		if (dragTarget == DragTarget::In && haveSelection) {
			const uint64_t maximum = selectionOutNs > minimumDurationNs ? selectionOutNs - minimumDurationNs
											 : selectionInNs;
			selectionInNs = std::min(timestampNs, maximum);
			playheadNs = selectionInNs;
		} else if (dragTarget == DragTarget::Out && haveSelection) {
			const uint64_t minimum = selectionInNs <= UINT64_MAX - minimumDurationNs
										 ? selectionInNs + minimumDurationNs
										 : selectionOutNs;
			selectionOutNs = std::max(timestampNs, minimum);
			selectionOutNs = clampToRecording(selectionOutNs);
			playheadNs = selectionOutNs;
		} else {
			playheadNs = timestampNs;
		}
		if (scrubHandler)
			scrubHandler(playheadNs, dragTarget);
		update();
	}

	void centerView(uint64_t centerNs)
	{
		if (!haveRecording || !zoomLocked)
			return;
		const uint64_t span = viewSpan();
		if (!span || span >= recordEndNs - recordStartNs) {
			fitView();
			return;
		}
		const uint64_t half = span / 2;
		viewStartNs = centerNs > half ? centerNs - half : recordStartNs;
		viewEndNs = viewStartNs <= UINT64_MAX - span ? viewStartNs + span : recordEndNs;
		clampView();
	}

	void clampView()
	{
		if (!haveRecording)
			return;
		const uint64_t recordSpan = recordEndNs - recordStartNs;
		uint64_t span = viewSpan();
		if (!span || span >= recordSpan) {
			fitView();
			return;
		}
		if (viewStartNs < recordStartNs) {
			viewStartNs = recordStartNs;
			viewEndNs = recordStartNs + span;
		}
		if (viewEndNs > recordEndNs) {
			viewEndNs = recordEndNs;
			viewStartNs = recordEndNs - span;
		}
	}

	void panView(int direction, uint64_t amount)
	{
		if (!zoomLocked || !amount)
			return;
		const uint64_t span = viewSpan();
		if (direction < 0) {
			const uint64_t available = viewStartNs - recordStartNs;
			const uint64_t shift = std::min(amount, available);
			viewStartNs -= shift;
			viewEndNs -= shift;
		} else {
			const uint64_t available = recordEndNs - viewEndNs;
			const uint64_t shift = std::min(amount, available);
			viewStartNs += shift;
			viewEndNs += shift;
		}
		if (viewSpan() != span)
			clampView();
		update();
	}

	void zoomView(double steps)
	{
		if (!haveRecording || steps == 0.0)
			return;
		const uint64_t recordSpan = recordEndNs - recordStartNs;
		const uint64_t currentSpan = zoomLocked ? viewSpan() : recordSpan;
		const long double factor = std::pow(1.25L, (long double)steps);
		uint64_t nextSpan = (uint64_t)((long double)currentSpan / factor);
		const uint64_t minimumSpan = std::max<uint64_t>(minimumDurationNs * 6ULL, 250000000ULL);
		nextSpan = qBound(minimumSpan, nextSpan, recordSpan);
		if (nextSpan >= recordSpan - std::min<uint64_t>(recordSpan / 100, 1000000ULL)) {
			fitView();
			return;
		}
		uint64_t center = zoomLocked ? viewStartNs + currentSpan / 2 : recordStartNs + recordSpan / 2;
		zoomLocked = true;
		viewStartNs = center > nextSpan / 2 ? center - nextSpan / 2 : recordStartNs;
		viewEndNs = viewStartNs + nextSpan;
		clampView();
		update();
	}

	uint64_t majorTickStep(int width) const
	{
		const uint64_t span = viewSpan();
		if (!span)
			return NS_PER_SECOND;
		const unsigned targetTicks = std::max(2, width / 105);
		const uint64_t raw = std::max<uint64_t>(1, span / targetTicks);
		if (minimumDurationNs && span <= 5ULL * NS_PER_SECOND) {
			uint64_t frames = std::max<uint64_t>(1, raw / minimumDurationNs);
			const uint64_t bases[] = {1, 2, 5, 10, 20, 50, 100};
			for (uint64_t base : bases) {
				if (base >= frames)
					return base * minimumDurationNs;
			}
		}
		const uint64_t steps[] = {10000000ULL,   20000000ULL,   50000000ULL,   100000000ULL,
					 200000000ULL,  500000000ULL,  NS_PER_SECOND, 2ULL * NS_PER_SECOND,
					 5ULL * NS_PER_SECOND, 10ULL * NS_PER_SECOND, 15ULL * NS_PER_SECOND,
					 30ULL * NS_PER_SECOND, 60ULL * NS_PER_SECOND, 120ULL * NS_PER_SECOND,
					 300ULL * NS_PER_SECOND, 600ULL * NS_PER_SECOND, 1800ULL * NS_PER_SECOND,
					 3600ULL * NS_PER_SECOND};
		for (uint64_t step : steps) {
			if (step >= raw)
				return step;
		}
		return 3600ULL * NS_PER_SECOND;
	}

	void paintRuler(QPainter &painter, const QRect &timeline)
	{
		const uint64_t major = majorTickStep(timeline.width());
		const uint64_t minor = std::max<uint64_t>(1, major / 5);
		const uint64_t relativeStart = viewStartNs > recordStartNs ? viewStartNs - recordStartNs : 0;
		uint64_t first = (relativeStart / minor) * minor;
		if (first < relativeStart)
			first += minor;
		for (uint64_t relative = first; relative <= recordEndNs - recordStartNs;) {
			const uint64_t timestamp = recordStartNs + relative;
			if (timestamp > viewEndNs)
				break;
			const int x = xFromTimestamp(timestamp);
			const bool majorTick = relative % major < minor;
			painter.setPen(palette().color(majorTick ? QPalette::Text : QPalette::Mid));
			const int top = timeline.top() + (majorTick ? 17 : 22);
			painter.drawLine(x, top, x, timeline.bottom());
			if (majorTick) {
				const QString label = replayClockText(relative);
				painter.drawText(QRect(x - 48, timeline.top(), 96, 15), Qt::AlignHCenter | Qt::AlignTop,
						 label);
			}
			if (relative > UINT64_MAX - minor)
				break;
			relative += minor;
		}
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
		QRect tag(right ? x - 34 : x, timeline.top() + 16, 34, 15);
		painter.fillRect(tag, marker);
		painter.setPen(palette().color(QPalette::HighlightedText));
		painter.drawText(tag, Qt::AlignCenter, label);
	}

	ScrubHandler scrubHandler;
	RangeHandler rangeHandler;
	CommandHandler commandHandler;
	DragTarget dragTarget = DragTarget::None;
	uint64_t recordStartNs = 0;
	uint64_t recordEndNs = 0;
	uint64_t viewStartNs = 0;
	uint64_t viewEndNs = 0;
	uint64_t playheadNs = 0;
	uint64_t selectionInNs = 0;
	uint64_t selectionOutNs = 0;
	uint64_t minimumDurationNs = 33333333ULL;
	bool haveRecording = false;
	bool haveSelection = false;
	bool zoomLocked = false;
};

'''
if 'class SrReplayTimeline : public QWidget' not in dock:
    dock = replace_once(dock, 'class SrEventTable : public QTableWidget {', professional_timeline_class + 'class SrEventTable : public QTableWidget {', 'insert professional timeline class')

old_timeline_ui = r'''
		auto *timelineBar = new QHBoxLayout();
		timelineBar->setSpacing(3);
		timelineBar->addWidget(new QLabel(T("EventDock.Timeline"), this));
		timelineModeLabel = new QLabel(T("EventDock.Timeline.EditMode"), this);
		timelineModeLabel->setAlignment(Qt::AlignCenter);
		timelineModeLabel->setMinimumWidth(58);
		timelineBar->addWidget(timelineModeLabel);
		timelineSlider = new SrRangeSlider(this);
		timelineSlider->setRange(0, TIMELINE_SCALE);
		timelineSlider->setSingleStep(1);
		timelineSlider->setPageStep(TIMELINE_SCALE / 100);
		timelineSlider->setMinimumHeight(30);
		timelineSlider->setEnabled(false);
		timelineSlider->setToolTip(T("EventDock.Timeline.EditTooltip"));
		timelineBar->addWidget(timelineSlider, 1);
		timelineTime = new QLabel(QStringLiteral("--:--.--- / --:--.---"), this);
		timelineTime->setMinimumWidth(275);
		timelineBar->addWidget(timelineTime);
		root->addLayout(timelineBar);
'''
new_timeline_ui = r'''
		auto *timelineBar = new QHBoxLayout();
		timelineBar->setSpacing(3);
		timelineBar->addWidget(new QLabel(T("EventDock.Timeline"), this));
		timelineModeLabel = new QLabel(T("EventDock.Timeline.EditMode"), this);
		timelineModeLabel->setAlignment(Qt::AlignCenter);
		timelineModeLabel->setMinimumWidth(58);
		timelineBar->addWidget(timelineModeLabel);
		timelineStack = new QStackedWidget(this);
		editTimeline = new SrReplayTimeline(timelineStack);
		editTimeline->setEnabled(false);
		timelineSlider = new SrRangeSlider(timelineStack);
		timelineSlider->setRange(0, TIMELINE_SCALE);
		timelineSlider->setSingleStep(1);
		timelineSlider->setPageStep(TIMELINE_SCALE / 100);
		timelineSlider->setMinimumHeight(30);
		timelineSlider->setEnabled(false);
		timelineStack->addWidget(editTimeline);
		timelineStack->addWidget(timelineSlider);
		timelineStack->setCurrentWidget(editTimeline);
		timelineBar->addWidget(timelineStack, 1);
		timelineTime = new QLabel(QStringLiteral("--:--.--- / --:--.---"), this);
		timelineTime->setMinimumWidth(315);
		timelineTime->setWordWrap(true);
		timelineBar->addWidget(timelineTime);
		root->addLayout(timelineBar);

		editTimelineControls = new QWidget(this);
		auto *editTimelineBar = new QHBoxLayout(editTimelineControls);
		editTimelineBar->setContentsMargins(0, 0, 0, 0);
		editTimelineBar->setSpacing(3);
		editSetInButton = new QPushButton(QStringLiteral("SET IN"), editTimelineControls);
		editGotoInButton = new QPushButton(QStringLiteral("|< IN"), editTimelineControls);
		editPreviewButton = new QPushButton(QStringLiteral("▶ IN→OUT"), editTimelineControls);
		editGotoOutButton = new QPushButton(QStringLiteral("OUT >|"), editTimelineControls);
		editSetOutButton = new QPushButton(QStringLiteral("SET OUT"), editTimelineControls);
		editFitButton = new QPushButton(QStringLiteral("FIT"), editTimelineControls);
		editLiveButton = new QPushButton(QStringLiteral("LIVE"), editTimelineControls);
		editTimelineBar->addStretch(1);
		editTimelineBar->addWidget(editSetInButton);
		editTimelineBar->addWidget(editGotoInButton);
		editTimelineBar->addWidget(editPreviewButton);
		editTimelineBar->addWidget(editGotoOutButton);
		editTimelineBar->addWidget(editSetOutButton);
		editTimelineBar->addSpacing(8);
		editTimelineBar->addWidget(editFitButton);
		editTimelineBar->addWidget(editLiveButton);
		auto *editHelp = new QLabel(QStringLiteral("Wheel: zoom · Shift+Wheel: pan · Space: play/pause · I/O: marks · [ ]: goto"),
						 editTimelineControls);
		editHelp->setStyleSheet(QStringLiteral("color: gray;"));
		editTimelineBar->addWidget(editHelp);
		root->addWidget(editTimelineControls);
'''
dock = replace_once(dock, old_timeline_ui, new_timeline_ui, 'timeline UI block')

old_connections = r'''
		connect(timelineSlider, &QSlider::sliderPressed, this, [this]() {
			timelineDragging = true;
			sr_replay_channel_pause(timelineTransportBus(), true);
		});
		connect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });
		connect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
			seekTimeline(timelineSlider->value());
			timelineDragging = false;
			syncTimeline();
		});
		timelineSlider->setClickHandler([this](int value) {
			seekEditTimeline(value);
			syncTimeline();
		});
		timelineSlider->setRangeHandler(
			[this](int rangeIn, int rangeOut) { editSelectedEventRange(rangeIn, rangeOut); });
'''
new_connections = r'''
		connect(timelineSlider, &QSlider::sliderPressed, this, [this]() {
			timelineDragging = true;
			sr_replay_channel_pause(timelineTransportBus(), true);
		});
		connect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });
		connect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
			seekTimeline(timelineSlider->value());
			timelineDragging = false;
			syncTimeline();
		});
		editTimeline->setScrubHandler([this](uint64_t timestampNs, SrReplayTimeline::DragTarget) {
			previewSeekTo(timestampNs);
		});
		editTimeline->setRangeHandler(
			[this](uint64_t inNs, uint64_t outNs) { editSelectedEventRange(inNs, outNs); });
		editTimeline->setCommandHandler([this](SrReplayTimeline::Command command) {
			switch (command) {
			case SrReplayTimeline::Command::TogglePlay:
				toggleEditPreview();
				break;
			case SrReplayTimeline::Command::GotoIn:
				gotoEditMarker(false);
				break;
			case SrReplayTimeline::Command::GotoOut:
				gotoEditMarker(true);
				break;
			case SrReplayTimeline::Command::SetIn:
				setEditMarkerAtPlayhead(false);
				break;
			case SrReplayTimeline::Command::SetOut:
				setEditMarkerAtPlayhead(true);
				break;
			case SrReplayTimeline::Command::StepBack:
				stepEditFrames(-1);
				break;
			case SrReplayTimeline::Command::StepForward:
				stepEditFrames(1);
				break;
			case SrReplayTimeline::Command::StepBackFast:
				stepEditFrames(-10);
				break;
			case SrReplayTimeline::Command::StepForwardFast:
				stepEditFrames(10);
				break;
			}
		});
		connect(editPreviewButton, &QPushButton::clicked, this, [this]() { previewPlayFromIn(); });
		connect(editGotoInButton, &QPushButton::clicked, this, [this]() { gotoEditMarker(false); });
		connect(editGotoOutButton, &QPushButton::clicked, this, [this]() { gotoEditMarker(true); });
		connect(editSetInButton, &QPushButton::clicked, this, [this]() { setEditMarkerAtPlayhead(false); });
		connect(editSetOutButton, &QPushButton::clicked, this, [this]() { setEditMarkerAtPlayhead(true); });
		connect(editFitButton, &QPushButton::clicked, this, [this]() {
			editTimeline->fitView();
			syncEditTimeline();
		});
		connect(editLiveButton, &QPushButton::clicked, this, [this]() {
			editTimeline->centerOnLive();
			previewSeekTo(editTimelineEndNs);
		});
'''
dock = replace_once(dock, old_connections, new_connections, 'timeline connection block')

# Camera changes should preserve the current EDIT playhead while switching angle.
dock = replace_once(
    dock,
    '''\t\tconnect(cameraCombo, &QComboBox::currentIndexChanged, this, [this](int) {\n\t\t\tif (!replayPlayoutActive()) {\n\t\t\t\teditPreviewEventId = 0;\n\t\t\t\tpreviewSelectedEvent(true);\n\t\t\t\tsyncTimeline();\n\t\t\t}\n\t\t});''',
    '''\t\tconnect(cameraCombo, &QComboBox::currentIndexChanged, this, [this](int) {\n\t\t\tif (!replayPlayoutActive()) {\n\t\t\t\tpreviewSelectedEvent(true);\n\t\t\t\tsyncTimeline();\n\t\t\t}\n\t\t});''',
    'camera preview connection',
)

start = dock.index('\tbool replayPlayoutActive() const\n')
end = dock.index('\n\tvoid jogMoved(int value)\n', start)
new_timeline_methods = r'''
	bool replayPlayoutActive() const
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
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.playing && !state.preview_mode)
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
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.playing && !state.preview_mode)
				return bus;
		}
		return transportBus();
	}

	uint64_t editFrameDurationNs() const
	{
		struct obs_video_info video = {};
		if (obs_get_video_info(&video) && video.fps_num && video.fps_den)
			return std::max<uint64_t>(1, (uint64_t)((long double)NS_PER_SECOND * video.fps_den /
										 (long double)video.fps_num));
		return 33333333ULL;
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

	QString editTimelineStatusText(uint64_t cursorNs, uint64_t inNs, uint64_t outNs) const
	{
		const uint64_t total = editTimelineHaveBounds && editTimelineEndNs > editTimelineStartNs
					       ? editTimelineEndNs - editTimelineStartNs
					       : 0;
		auto relative = [this](uint64_t timestamp) {
			return editTimelineHaveBounds && timestamp > editTimelineStartNs ? timestamp - editTimelineStartNs : 0;
		};
		return QStringLiteral("CUR %1  ·  IN %2  ·  OUT %3  ·  DUR %4  ·  REC %5")
			.arg(replayClockText(relative(cursorNs)))
			.arg(replayClockText(relative(inNs)))
			.arg(replayClockText(relative(outNs)))
			.arg(replayClockText(outNs > inNs ? outNs - inNs : 0))
			.arg(replayClockText(total));
	}

	QStringList editPreviewCandidates(const sr_event_record &event) const
	{
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
		return candidates;
	}

	bool cueEditPreviewAt(const QString &camera, uint64_t eventId, uint64_t timestampNs, uint64_t rangeInNs,
			      uint64_t rangeOutNs)
	{
		if (camera.isEmpty() || !eventId || rangeOutNs <= rangeInNs)
			return false;
		const enum sr_replay_bus bus = transportBus();
		const QByteArray cameraUtf8 = camera.toUtf8();
		if (!sr_replay_channel_cue_preview(bus, eventId, cameraUtf8.constData(), rangeInNs, rangeOutNs, timestampNs))
			return false;
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

	bool previewSelectedEvent(bool force)
	{
		if (!controller || replayPlayoutActive())
			return false;
		updateEditTimelineBounds();
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return false;

		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event))
			return false;
		const QStringList candidates = editPreviewCandidates(event);
		uint64_t target = event.in_ns;
		if (editPreviewEventId == eventId && editTimeline && editTimeline->playheadTimestamp())
			target = editTimeline->playheadTimestamp();
		if (target < editTimelineStartNs || target > editTimelineEndNs)
			target = event.in_ns;
		const uint64_t rangeIn = editTimelineHaveBounds ? editTimelineStartNs : event.in_ns;
		const uint64_t rangeOut = editTimelineHaveBounds ? editTimelineEndNs : event.out_ns;
		sr_event_controller_free_event(&event);

		const enum sr_replay_bus bus = transportBus();
		for (const QString &camera : candidates) {
			sr_replay_channel_state current = {};
			if (!force && sr_replay_channel_get_state(bus, &current) && current.cued && current.preview_mode &&
			    current.event_id == eventId && QString::fromUtf8(current.camera_name) == camera &&
			    target >= current.in_ns && target <= current.out_ns) {
				editPreviewEventId = eventId;
				editPreviewBus = bus;
				editPreviewCamera = camera;
				sr_replay_channel_pause(bus, true);
				sr_replay_channel_seek(bus, target);
				return true;
			}
			if (cueEditPreviewAt(camera, eventId, target, rangeIn, rangeOut))
				return true;
		}

		editPreviewEventId = 0;
		editPreviewCamera.clear();
		return false;
	}

	bool previewSeekTo(uint64_t timestampNs)
	{
		if (!controller || replayPlayoutActive() || !editTimelineHaveBounds)
			return false;
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return false;
		timestampNs = qBound(editTimelineStartNs, timestampNs, editTimelineEndNs);

		if (editPreviewEventId != eventId || editPreviewBus != transportBus() || editPreviewCamera.isEmpty()) {
			if (!previewSelectedEvent(false))
				return false;
		}

		const enum sr_replay_bus bus = transportBus();
		sr_replay_channel_state state = {};
		bool ok = sr_replay_channel_get_state(bus, &state) && state.cued && state.preview_mode &&
			  state.event_id == eventId && QString::fromUtf8(state.camera_name) == editPreviewCamera &&
			  timestampNs >= state.in_ns && timestampNs <= state.out_ns;
		if (ok) {
			sr_replay_channel_pause(bus, true);
			ok = sr_replay_channel_seek(bus, timestampNs);
		} else {
			ok = cueEditPreviewAt(editPreviewCamera, eventId, timestampNs, editTimelineStartNs,
						 editTimelineEndNs);
		}
		if (!ok)
			return false;

		sr_replay_channel_get_state(bus, &state);
		const uint64_t actual = state.cued ? state.playhead_ns : timestampNs;
		if (editTimeline)
			editTimeline->setPlayhead(actual);
		uint64_t inNs = editTimeline && editTimeline->hasSelection() ? editTimeline->selectionIn() : actual;
		uint64_t outNs = editTimeline && editTimeline->hasSelection() ? editTimeline->selectionOut() : actual;
		timelineTime->setText(editTimelineStatusText(actual, inNs, outNs));
		return true;
	}

	void syncEditTimeline()
	{
		updateEditTimelineBounds();
		setTimelineModeBadge(true);
		if (timelineStack)
			timelineStack->setCurrentWidget(editTimeline);
		if (editTimelineControls)
			editTimelineControls->setVisible(true);
		if (!editTimeline)
			return;
		editTimeline->setMinimumDuration(editFrameDurationNs());
		if (!editTimelineHaveBounds || editTimelineEndNs <= editTimelineStartNs) {
			timelineEventId = 0;
			editTimeline->setEnabled(false);
			editTimeline->setRecordingBounds(0, 0);
			timelineTime->setText(T("EventDock.Timeline.NoRecording"));
			return;
		}
		editTimeline->setRecordingBounds(editTimelineStartNs, editTimelineEndNs);
		if (editTimeline->isDragging())
			return;

		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (!eventId || !controller || !sr_event_controller_get_event(controller, eventId, &event) ||
		    event.out_ns <= event.in_ns) {
			if (event.id)
				sr_event_controller_free_event(&event);
			timelineEventId = 0;
			editTimeline->clearSelection();
			editTimeline->setEnabled(false);
			editTimeline->setPlayhead(editTimelineEndNs);
			timelineTime->setText(T("EventDock.Timeline.RecordLength")
						      .arg(replayClockText(editTimelineEndNs - editTimelineStartNs)));
			return;
		}

		timelineEventId = event.id;
		editTimeline->setEnabled(true);
		editTimeline->setSelection(event.in_ns, event.out_ns);
		uint64_t playhead = event.in_ns;
		sr_replay_channel_state preview = {};
		if (sr_replay_channel_get_state(transportBus(), &preview) && preview.cued && preview.preview_mode &&
		    preview.event_id == event.id)
			playhead = preview.playhead_ns;
		editTimeline->setPlayhead(playhead);
		timelineTime->setText(editTimelineStatusText(playhead, event.in_ns, event.out_ns));
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
			if (timelineStack)
				timelineStack->setCurrentWidget(timelineSlider);
			if (editTimelineControls)
				editTimelineControls->setVisible(false);
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
		if (timelineStack)
			timelineStack->setCurrentWidget(timelineSlider);
		if (editTimelineControls)
			editTimelineControls->setVisible(false);
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
			const int sliderValue = (int)((long double)position * TIMELINE_SCALE / (long double)duration);
			timelineSlider->setValue(sliderValue);
		}
		timelineTime->setText(replayClockText(position) + QStringLiteral(" / ") + replayClockText(duration));
	}

	void seekTimeline(int value)
	{
		if (!replayPlayoutActive())
			return;

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

	void editSelectedEventRange(uint64_t inNs, uint64_t outNs)
	{
		if (!controller || replayPlayoutActive() || !editTimelineHaveBounds || outNs <= inNs) {
			syncTimeline();
			return;
		}
		inNs = qBound(editTimelineStartNs, inNs, editTimelineEndNs);
		outNs = qBound(editTimelineStartNs, outNs, editTimelineEndNs);
		const uint64_t minimumDuration = editFrameDurationNs();
		if (outNs <= inNs || outNs - inNs < minimumDuration) {
			setStatus("EventDock.Timeline.TooShort");
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
		status->setText(T("EventDock.Timeline.EditSaved").arg((double)(outNs - inNs) / 1e9, 0, 'f', 3));
		refresh(eventId);
		refreshAngleCoverage();
		previewSelectedEvent(true);
		syncTimeline();
	}

	void gotoEditMarker(bool outMarker)
	{
		if (!editTimeline || !editTimeline->hasSelection())
			return;
		previewSeekTo(outMarker ? editTimeline->selectionOut() : editTimeline->selectionIn());
	}

	void setEditMarkerAtPlayhead(bool outMarker)
	{
		if (!editTimeline || !editTimeline->hasSelection())
			return;
		const uint64_t cursor = editTimeline->playheadTimestamp();
		uint64_t inNs = editTimeline->selectionIn();
		uint64_t outNs = editTimeline->selectionOut();
		if (outMarker)
			outNs = cursor;
		else
			inNs = cursor;
		editSelectedEventRange(inNs, outNs);
	}

	void stepEditFrames(int frames)
	{
		if (!editTimelineHaveBounds || !editTimeline || !frames)
			return;
		const uint64_t frame = editFrameDurationNs();
		uint64_t cursor = editTimeline->playheadTimestamp();
		if (frames < 0) {
			const uint64_t magnitude = (uint64_t)(-frames) * frame;
			cursor = magnitude >= cursor - editTimelineStartNs ? editTimelineStartNs : cursor - magnitude;
		} else {
			const uint64_t magnitude = (uint64_t)frames * frame;
			cursor = magnitude >= editTimelineEndNs - cursor ? editTimelineEndNs : cursor + magnitude;
		}
		previewSeekTo(cursor);
	}

	void previewPlayFromIn()
	{
		if (!controller || replayPlayoutActive())
			return;
		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (!eventId || !sr_event_controller_get_event(controller, eventId, &event))
			return;
		const QStringList candidates = editPreviewCandidates(event);
		const uint64_t inNs = event.in_ns;
		const uint64_t outNs = event.out_ns;
		sr_event_controller_free_event(&event);
		for (const QString &camera : candidates) {
			if (!cueEditPreviewAt(camera, eventId, inNs, inNs, outNs))
				continue;
			sr_replay_channel_set_loop(transportBus(), loopButton && loopButton->isChecked());
			sr_replay_channel_play(transportBus());
			if (editTimeline)
				editTimeline->setPlayhead(inNs);
			status->setText(QStringLiteral("EDIT preview · Event %1 · %2").arg(eventId).arg(camera));
			return;
		}
		setStatus("EventDock.CueFailed");
	}

	void toggleEditPreview()
	{
		if (!controller || replayPlayoutActive())
			return;
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		sr_replay_channel_state state = {};
		const enum sr_replay_bus bus = transportBus();
		if (sr_replay_channel_get_state(bus, &state) && state.cued && state.preview_mode &&
		    state.event_id == eventId && state.playing && !state.paused) {
			sr_replay_channel_pause(bus, true);
			return;
		}

		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event))
			return;
		const QStringList candidates = editPreviewCandidates(event);
		uint64_t target = editTimeline ? editTimeline->playheadTimestamp() : event.in_ns;
		target = qBound(event.in_ns, target, event.out_ns);
		const uint64_t inNs = event.in_ns;
		const uint64_t outNs = event.out_ns;
		sr_event_controller_free_event(&event);
		for (const QString &camera : candidates) {
			if (!cueEditPreviewAt(camera, eventId, target, inNs, outNs))
				continue;
			sr_replay_channel_set_loop(bus, loopButton && loopButton->isChecked());
			sr_replay_channel_play(bus);
			return;
		}
	}
'''
dock = dock[:start] + new_timeline_methods + dock[end:]

# Prevent a manual TAKE from putting a full-recording transient preview range on air.
old_take = r'''
	void takeBus(enum sr_replay_bus bus)
	{
		if (!controller || !sr_replay_take_bus(controller, bus)) {
'''
new_take = r'''
	void takeBus(enum sr_replay_bus bus)
	{
		sr_replay_channel_state candidate = {};
		if (sr_replay_channel_get_state(bus, &candidate) && candidate.cued && candidate.preview_mode) {
			if (!cueSelected(bus))
				return;
		}
		if (!controller || !sr_replay_take_bus(controller, bus)) {
'''
dock = replace_once(dock, old_take, new_take, 'normalize preview before take')

old_members = r'''
	SrRangeSlider *timelineSlider = nullptr;
	QSlider *jogSlider = nullptr;
	QSlider *shuttleSlider = nullptr;
	QLabel *timelineTime = nullptr;
	QLabel *timelineModeLabel = nullptr;
'''
new_members = r'''
	QStackedWidget *timelineStack = nullptr;
	SrReplayTimeline *editTimeline = nullptr;
	SrRangeSlider *timelineSlider = nullptr;
	QWidget *editTimelineControls = nullptr;
	QPushButton *editSetInButton = nullptr;
	QPushButton *editGotoInButton = nullptr;
	QPushButton *editPreviewButton = nullptr;
	QPushButton *editGotoOutButton = nullptr;
	QPushButton *editSetOutButton = nullptr;
	QPushButton *editFitButton = nullptr;
	QPushButton *editLiveButton = nullptr;
	QSlider *jogSlider = nullptr;
	QSlider *shuttleSlider = nullptr;
	QLabel *timelineTime = nullptr;
	QLabel *timelineModeLabel = nullptr;
'''
dock = replace_once(dock, old_members, new_members, 'timeline member block')

# The old integer edit mapper must be gone after replacing the method region.
for stale in ('editTimelineValue(', 'editTimelineTimestamp(', 'seekEditTimeline('):
    if stale in dock:
        raise RuntimeError(f'stale old timeline API remains: {stale}')

dock_path.write_text(dock, encoding='utf-8')

# Restore the normal CI workflow in the product commit and delete this helper.
workflow_path.write_text('''name: Replay Development CI\nrun-name: Replay Development CI — ${{ github.ref_name }} — ${{ github.event_name }}\n\non:\n  workflow_dispatch:\n  push:\n    branches:\n      - feature/hardware-zero-copy\n\npermissions:\n  contents: read\n\nconcurrency:\n  group: replay-development-ci-${{ github.ref }}\n  cancel-in-progress: true\n\n# Full validation for the active replay development branch.\n# Every push is built; workflow_dispatch keeps a manual Run workflow button\n# available from the default branch. PR validation is intentionally left to\n# the repository's normal PR workflow when this branch is eventually retargeted\n# to main, avoiding duplicate push + pull_request builds during development.\njobs:\n  check-format:\n    name: Check Formatting 🔍\n    uses: ./.github/workflows/check-format.yaml\n    permissions:\n      contents: read\n\n  build-project:\n    name: Build Project 🧱\n    uses: ./.github/workflows/build-project.yaml\n    secrets: inherit\n    permissions:\n      contents: read\n''', encoding='utf-8')
script_path.unlink()
