/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-dock.h"

#include "sr-camera-list.h"
#include "sr-camera-identity.h"
#include "sr-capture.h"
#include "sr-config.h"
#include "sr-event-controller.h"
#include "sr-dock.h"
#include "sr-event-export.h"
#include "sr-replay-channel.h"
#include "sr-replay-coverage.h"
#include "sr-replay-playlist.h"
#include "sr-program-recorder.h"
#include "sr-replay-setup.h"
#include "sr-replay-take.h"
#include "sr-storage-cleanup.h"
#include "sr-session.h"
#include "sr-thumb.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <algorithm>
#include <cstring>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QRubberBand>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStringList>
#include <QTableWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#define NS_PER_SECOND 1000000000ULL

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString stateText(const sr_event_record &event)
{
	QStringList states;
	if (event.pending)
		states.append(T("EventDock.State.Pending"));
	if (event.protected_event)
		states.append(T("EventDock.State.Protected"));
	if (event.played)
		states.append(T("EventDock.State.Played"));
	if (states.isEmpty())
		states.append(T("EventDock.State.Ready"));
	return states.join(QStringLiteral(" / "));
}

QString durationText(const sr_event_record &event)
{
	if (event.out_ns < event.in_ns)
		return QStringLiteral("-");
	return QString::number((double)(event.out_ns - event.in_ns) / 1e9, 'f', 3) + QStringLiteral(" s");
}

QString replayClockText(uint64_t ns)
{
	const uint64_t totalMs = ns / 1000000ULL;
	const uint64_t minutes = totalMs / 60000ULL;
	const uint64_t seconds = (totalMs / 1000ULL) % 60ULL;
	const uint64_t millis = totalMs % 1000ULL;
	return QStringLiteral("%1:%2.%3")
		.arg(minutes, 2, 10, QChar('0'))
		.arg(seconds, 2, 10, QChar('0'))
		.arg(millis, 3, 10, QChar('0'));
}

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

QStringList captureCameraNames()
{
	QStringList names;
	sr_camera_list cameras = {};
	if (!sr_camera_list_capture(&cameras))
		return names;
	for (size_t i = 0; i < cameras.count; i++)
		names.append(QString::fromUtf8(cameras.names[i]));
	sr_camera_list_free(&cameras);
	return names;
}

QString channelSummary(enum sr_replay_bus bus, const QString &label)
{
	sr_replay_channel_state state = {};
	if (!sr_replay_channel_get_state(bus, &state) || !state.cued)
		return QStringLiteral("%1: —").arg(label);

	const double duration = state.out_ns >= state.in_ns ? (double)(state.out_ns - state.in_ns) / 1e9 : 0.0;
	const double position = state.playhead_ns >= state.in_ns ? (double)(state.playhead_ns - state.in_ns) / 1e9
								 : 0.0;
	QString mode = T("EventDock.Transport.Cued");
	if (state.playing)
		mode = state.paused ? T("EventDock.Transport.Paused") : T("EventDock.Transport.Playing");
	QString flags;
	if (state.backward)
		flags += QStringLiteral(" REV");
	if (state.loop)
		flags += QStringLiteral(" LOOP");
	if (state.partial_coverage)
		flags += QStringLiteral(" PARTIAL");
	flags += state.audio_mode == SR_REPLAY_AUDIO_MASTER   ? QStringLiteral(" MASTER")
		 : state.audio_mode == SR_REPLAY_AUDIO_CAMERA ? QStringLiteral(" CAMERA")
							      : QStringLiteral(" MUTE");

	return QStringLiteral("%1: #%2  %3  %4%  %5/%6 s  %7%8")
		.arg(label)
		.arg(state.event_id)
		.arg(QString::fromUtf8(state.camera_name))
		.arg(state.speed_percent, 0, 'f', 0)
		.arg(position, 0, 'f', 2)
		.arg(duration, 0, 'f', 2)
		.arg(mode)
		.arg(flags);
}

QString encoderShortName(const char *name)
{
	const QString encoder = QString::fromUtf8(name ? name : "");
	if (encoder.contains(QStringLiteral("nvenc"), Qt::CaseInsensitive))
		return QStringLiteral("NVENC");
	if (encoder.contains(QStringLiteral("amf"), Qt::CaseInsensitive))
		return QStringLiteral("AMF");
	if (encoder.contains(QStringLiteral("qsv"), Qt::CaseInsensitive))
		return QStringLiteral("QSV");
	if (encoder.contains(QStringLiteral("x264"), Qt::CaseInsensitive))
		return QStringLiteral("x264");
	return encoder.isEmpty() ? QStringLiteral("—") : encoder;
}

QString capturePerformancePath(const sr_capture_performance_entry &entry)
{
	const QString encoder = encoderShortName(entry.encoder_name);
	switch (entry.path) {
	case SR_CAPTURE_PERF_GPU_D3D11:
		return QStringLiteral("D3D11 → %1").arg(encoder);
	case SR_CAPTURE_PERF_CPU:
		return QStringLiteral("CPU → %1").arg(encoder);
	case SR_CAPTURE_PERF_ERROR:
		return T("EventDock.Performance.Error");
	case SR_CAPTURE_PERF_WAITING:
	default:
		return T("EventDock.Performance.Waiting");
	}
}

QString captureFallbackText(enum sr_capture_gpu_fallback_reason reason)
{
	switch (reason) {
	case SR_CAPTURE_GPU_FALLBACK_CREATE_FAILED:
		return T("EventDock.Performance.FallbackCreate");
	case SR_CAPTURE_GPU_FALLBACK_RUNTIME_FAILED:
		return T("EventDock.Performance.FallbackRuntime");
	case SR_CAPTURE_GPU_FALLBACK_NONE:
	default:
		return QString();
	}
}

QString captureVideoMode(const sr_capture_performance_entry &entry)
{
	if (!entry.width || !entry.height)
		return QStringLiteral("—");
	QString fps = QStringLiteral("—");
	if (entry.fps_num && entry.fps_den) {
		if (entry.fps_num % entry.fps_den == 0)
			fps = QString::number(entry.fps_num / entry.fps_den);
		else
			fps = QString::number((double)entry.fps_num / (double)entry.fps_den, 'f', 2);
	}
	return QStringLiteral("%1×%2 @ %3").arg(entry.width).arg(entry.height).arg(fps);
}

QString captureGopMode(const sr_capture_performance_entry &entry)
{
	const QString gop = entry.gop_ms ? QStringLiteral("%1 ms").arg(entry.gop_ms) : QStringLiteral("All-I");
	return QStringLiteral("%1 / QP %2").arg(gop).arg(entry.qp);
}

QString captureDiskState(const sr_capture_performance_entry &entry)
{
	if (!entry.disk_requested)
		return T("EventDock.Performance.Off");
	if (entry.reserve_blocked)
		return T("EventDock.Performance.Reserve");
	if (entry.writer_failed)
		return T("EventDock.Performance.WriteError");
	if (!entry.writer_active)
		return T("EventDock.Performance.Starting");
	return T("EventDock.Performance.Recording").arg((double)entry.bytes_written / (1024.0 * 1024.0), 0, 'f', 1);
}

QString replayPerformanceSummary(enum sr_replay_bus bus, const QString &label)
{
	sr_replay_channel_state state = {};
	if (!sr_replay_channel_get_state(bus, &state) || !state.cued)
		return T("EventDock.Performance.ReplayEmpty").arg(label);

	QString decoder = T("EventDock.Performance.Waiting");
	if (state.decoder_open)
		decoder = state.hardware_decode ? QStringLiteral("D3D11VA") : QStringLiteral("Software");

	QString video = QStringLiteral("—");
	if (state.width && state.height)
		video = QStringLiteral("%1×%2").arg(state.width).arg(state.height);
	const uint64_t hitPercent = state.decode_requests ? state.decode_cache_hits * 100ULL / state.decode_requests
							  : 0ULL;
	return T("EventDock.Performance.Replay")
		.arg(label)
		.arg(decoder)
		.arg(video)
		.arg(hitPercent)
		.arg(state.decode_cache_hits)
		.arg(state.decode_requests)
		.arg(state.decoded_frames);
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
	return safe.isEmpty() ? QStringLiteral("camera") : safe.left(80);
}

QString unusedAnglePath(const QDir &directory, uint64_t eventId, const QString &camera)
{
	const QString stem = QStringLiteral("Event_%1_%2").arg(eventId, 6, 10, QChar('0')).arg(safeFilePart(camera));
	QString path = directory.filePath(stem + QStringLiteral(".mp4"));
	for (unsigned suffix = 2; QFileInfo::exists(path); suffix++)
		path = directory.filePath(stem + QStringLiteral("-%1.mp4").arg(suffix));
	return path;
}

struct ExportTask {
	std::string sessionDir;
	std::string camera;
	std::string outputPath;
	uint64_t eventInNs = 0;
	uint64_t eventOutNs = 0;
	int64_t syncOffsetNs = 0;
	bool includeMasterAudio = false;
};

struct ExportJob {
	std::vector<ExportTask> tasks;
	std::atomic<bool> cancel{false};
	std::atomic<bool> done{false};
	std::atomic<unsigned> progress{0};
	std::thread worker;
	bool success = false;
	size_t completed = 0;
	std::string failedCamera;
	sr_event_export_result result = {};
};

constexpr int ANGLE_PREVIEW_WIDTH = 176;
constexpr int ANGLE_PREVIEW_HEIGHT = 99;
constexpr int EVENT_THUMB_WIDTH = 192;
constexpr int EVENT_THUMB_HEIGHT = 108;
constexpr int EVENT_THUMB_DISPLAY_WIDTH = 176;
constexpr int EVENT_THUMB_DISPLAY_HEIGHT = 99;
constexpr int EVENT_THUMB_CELL_WIDTH = 184;
constexpr int EVENT_THUMB_CELL_HEIGHT = 142;
constexpr size_t EVENT_THUMB_BATCH = 24;
constexpr size_t ANGLE_PREVIEW_CACHE_EVENTS = 12;

class SrEventThumbnailDelegate : public QStyledItemDelegate {
public:
	explicit SrEventThumbnailDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

	QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
	{
		return QSize(EVENT_THUMB_CELL_WIDTH, EVENT_THUMB_CELL_HEIGHT);
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		QStyleOptionViewItem itemOption(option);
		initStyleOption(&itemOption, index);
		const QIcon icon = itemOption.icon;
		const QString text = itemOption.text;

		itemOption.icon = QIcon();
		itemOption.text.clear();
		QStyle *style = itemOption.widget ? itemOption.widget->style() : QApplication::style();
		style->drawControl(QStyle::CE_ItemViewItem, &itemOption, painter, itemOption.widget);

		const QRect content = option.rect.adjusted(4, 3, -4, -3);
		const QRect imageBox(content.left(), content.top(), content.width(), EVENT_THUMB_DISPLAY_HEIGHT);
		if (!icon.isNull()) {
			const QIcon::Mode mode = !(option.state & QStyle::State_Enabled)   ? QIcon::Disabled
						 : (option.state & QStyle::State_Selected) ? QIcon::Selected
											   : QIcon::Normal;
			QPixmap pixmap = icon.pixmap(QSize(EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT), mode);
			if (!pixmap.isNull()) {
				QSize drawSize = pixmap.size();
				drawSize.scale(QSize(EVENT_THUMB_DISPLAY_WIDTH, EVENT_THUMB_DISPLAY_HEIGHT),
					       Qt::KeepAspectRatio);
				QRect target(QPoint(0, 0), drawSize);
				target.moveCenter(imageBox.center());
				painter->save();
				painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
				painter->drawPixmap(target, pixmap);
				painter->restore();
			}
		}

		const QRect textRect(content.left(), imageBox.bottom() + 4, content.width(),
				     std::max(0, content.bottom() - imageBox.bottom() - 3));
		painter->save();
		painter->setPen((option.state & QStyle::State_Selected) ? option.palette.highlightedText().color()
									: option.palette.text().color());
		painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, text);
		painter->restore();
	}
};

struct AnglePreviewResult {
	std::string camera;
	std::vector<uint8_t> rgba;
};

struct AnglePreviewTask {
	std::string camera;
	uint64_t timestampNs = 0;
};

struct AnglePreviewJob {
	uint64_t eventId = 0;
	std::string sessionDir;
	std::vector<AnglePreviewTask> tasks;
	std::vector<AnglePreviewResult> results;
	std::atomic<bool> done{false};
	std::thread worker;
};

struct EventThumbnailResult {
	uint64_t eventId = 0;
	uint64_t inNs = 0;
	uint64_t outNs = 0;
	std::vector<uint8_t> rgba;
};

struct EventThumbnailTask {
	uint64_t eventId = 0;
	uint64_t inNs = 0;
	uint64_t outNs = 0;
	std::string camera;
	uint64_t timestampNs = 0;
};

struct EventThumbnailJob {
	uint64_t generation = 0;
	std::string sessionDir;
	std::vector<EventThumbnailTask> tasks;
	std::vector<EventThumbnailResult> results;
	std::atomic<bool> done{false};
	std::thread worker;
};

struct CachedEventThumbnail {
	uint64_t inNs = 0;
	uint64_t outNs = 0;
	QIcon icon;
};

void runAnglePreviewJob(AnglePreviewJob *job)
{
	for (const AnglePreviewTask &task : job->tasks) {
		uint8_t *rgba = nullptr;
		AnglePreviewResult result;
		result.camera = task.camera;
		if (sr_disk_thumbnail_rgba(job->sessionDir.c_str(), task.camera.c_str(), task.timestampNs,
					   ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT, &rgba) &&
		    rgba) {
			const size_t bytes = (size_t)ANGLE_PREVIEW_WIDTH * ANGLE_PREVIEW_HEIGHT * 4;
			result.rgba.assign(rgba, rgba + bytes);
		}
		bfree(rgba);
		job->results.emplace_back(std::move(result));
	}
	job->done.store(true, std::memory_order_release);
}

void runEventThumbnailJob(EventThumbnailJob *job)
{
	for (const EventThumbnailTask &task : job->tasks) {
		uint8_t *rgba = nullptr;
		EventThumbnailResult result;
		result.eventId = task.eventId;
		result.inNs = task.inNs;
		result.outNs = task.outNs;
		if (sr_disk_thumbnail_rgba(job->sessionDir.c_str(), task.camera.c_str(), task.timestampNs,
					   EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT, &rgba) &&
		    rgba) {
			const size_t bytes = (size_t)EVENT_THUMB_WIDTH * EVENT_THUMB_HEIGHT * 4;
			result.rgba.assign(rgba, rgba + bytes);
		}
		bfree(rgba);
		job->results.emplace_back(std::move(result));
	}
	job->done.store(true, std::memory_order_release);
}

bool exportCancelled(void *data)
{
	return static_cast<ExportJob *>(data)->cancel.load(std::memory_order_relaxed);
}

void exportProgress(void *data, unsigned percent)
{
	auto *job = static_cast<ExportJob *>(data);
	const unsigned taskCount = (unsigned)job->tasks.size();
	const unsigned completed = (unsigned)job->completed;
	job->progress.store(taskCount ? (completed * 100U + percent) / taskCount : 0, std::memory_order_relaxed);
}

void runExportJob(ExportJob *job)
{
	job->success = true;
	for (size_t i = 0; i < job->tasks.size(); i++) {
		if (job->cancel.load(std::memory_order_relaxed)) {
			job->success = false;
			job->result.error = SR_EVENT_EXPORT_CANCELLED;
			break;
		}

		const ExportTask &task = job->tasks[i];
		sr_event_export_spec spec = {};
		spec.session_dir = task.sessionDir.c_str();
		spec.camera_name = task.camera.c_str();
		spec.output_path = task.outputPath.c_str();
		spec.event_in_ns = task.eventInNs;
		spec.event_out_ns = task.eventOutNs;
		spec.camera_sync_offset_ns = task.syncOffsetNs;
		spec.include_master_audio = task.includeMasterAudio;
		job->failedCamera = task.camera;
		if (!sr_event_export_fast(&spec, exportCancelled, exportProgress, job, &job->result)) {
			job->success = false;
			break;
		}
		job->completed = i + 1;
		job->progress.store((unsigned)(job->completed * 100 / job->tasks.size()), std::memory_order_relaxed);
	}
	job->done.store(true, std::memory_order_release);
}

class SrRangeSlider : public QSlider {
public:
	using RangeHandler = std::function<void(int, int)>;
	using ClickHandler = std::function<void(int)>;

	explicit SrRangeSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent) {}

	void setRangeHandler(RangeHandler handler) { rangeHandler = std::move(handler); }
	void setClickHandler(ClickHandler handler) { clickHandler = std::move(handler); }
	void setSequenceProgress(bool active)
	{
		sequenceProgress = active;
		if (active)
			clearSelection();
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

	void clearSelection()
	{
		selecting = false;
		hasSelection = false;
		update();
	}

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (sequenceProgress) {
			event->accept();
			return;
		}
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

		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
		if (progressTint.isValid()) {
			const int left = pixelForValue(minimum());
			const int right = pixelForValue(value());
			if (right > left) {
				QColor tint = progressTint;
				tint.setAlpha(210);
				QPainter progressPainter(this);
				progressPainter.setPen(Qt::NoPen);
				progressPainter.setBrush(tint);
				progressPainter.drawRoundedRect(QRect(left, groove.center().y() - 4, right - left, 8),
								3, 3);
			}
		}

		if (!selecting && !hasSelection)
			return;

		const int rangeIn = qMin(selectionStart, selectionEnd);
		const int rangeOut = qMax(selectionStart, selectionEnd);
		const int left = pixelForValue(rangeIn);
		const int right = pixelForValue(rangeOut);
		if (right <= left)
			return;

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
	bool sequenceProgress = false;
	QColor progressTint;
};

class SrEventTable : public QTableWidget {
public:
	explicit SrEventTable(QWidget *parent = nullptr)
		: QTableWidget(parent),
		  rubberBand(new QRubberBand(QRubberBand::Rectangle, viewport()))
	{
	}

	bool selectionGestureActive() const { return rubberCandidate; }

protected:
	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && !(event->modifiers() & Qt::AltModifier)) {
			rubberCandidate = true;
			rubberSelecting = false;
			rubberOrigin = event->position().toPoint();
			rubberAdditive = event->modifiers().testFlag(Qt::ControlModifier);
			baseRows.clear();
			if (rubberAdditive && selectionModel()) {
				const QModelIndexList selected = selectionModel()->selectedRows();
				baseRows.reserve(selected.size());
				for (const QModelIndex &index : selected)
					baseRows.push_back(index.row());
			}
			rubberBand->hide();
		} else {
			rubberCandidate = false;
			rubberSelecting = false;
			rubberBand->hide();
		}
		QTableWidget::mousePressEvent(event);
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (!rubberCandidate || !(event->buttons() & Qt::LeftButton)) {
			QTableWidget::mouseMoveEvent(event);
			return;
		}

		const QPoint position = boundedViewportPoint(event->position().toPoint());
		if (!rubberSelecting && (position - rubberOrigin).manhattanLength() < 4) {
			event->accept();
			return;
		}

		rubberSelecting = true;
		const QRect rectangle = QRect(rubberOrigin, position).normalized().intersected(viewport()->rect());
		rubberBand->setGeometry(rectangle);
		rubberBand->show();
		applyRubberSelection(rectangle);
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (!rubberCandidate || event->button() != Qt::LeftButton) {
			QTableWidget::mouseReleaseEvent(event);
			return;
		}

		if (rubberSelecting) {
			const QPoint position = boundedViewportPoint(event->position().toPoint());
			const QRect rectangle =
				QRect(rubberOrigin, position).normalized().intersected(viewport()->rect());
			applyRubberSelection(rectangle);
			rubberBand->hide();
			rubberCandidate = false;
			rubberSelecting = false;
			event->accept();
			return;
		}

		rubberCandidate = false;
		QTableWidget::mouseReleaseEvent(event);
	}

private:
	QPoint boundedViewportPoint(const QPoint &position) const
	{
		const QRect bounds = viewport()->rect();
		return QPoint(qBound(bounds.left(), position.x(), bounds.right()),
			      qBound(bounds.top(), position.y(), bounds.bottom()));
	}

	void applyRubberSelection(const QRect &rectangle)
	{
		if (!selectionModel() || !model())
			return;

		selectionModel()->clearSelection();
		if (rubberAdditive) {
			for (int row : baseRows) {
				if (row >= 0 && row < rowCount())
					selectionModel()->select(model()->index(row, 0),
								 QItemSelectionModel::Select |
									 QItemSelectionModel::Rows);
			}
		}

		for (int row = 0; row < rowCount(); row++) {
			const QModelIndex index = model()->index(row, 0);
			QRect rowRectangle = visualRect(index);
			if (!rowRectangle.isValid())
				continue;
			rowRectangle.setLeft(0);
			rowRectangle.setRight(viewport()->width() - 1);
			if (rowRectangle.intersects(rectangle))
				selectionModel()->select(index,
							 QItemSelectionModel::Select | QItemSelectionModel::Rows);
		}

		const QModelIndexList selected = selectionModel()->selectedRows();
		if (!selected.isEmpty() && !selectionModel()->isSelected(currentIndex()))
			setCurrentIndex(selected.constFirst());
	}

	QRubberBand *rubberBand = nullptr;
	QPoint rubberOrigin;
	QVector<int> baseRows;
	bool rubberCandidate = false;
	bool rubberSelecting = false;
	bool rubberAdditive = false;
};

class SrEventDock : public QWidget {
public:
	explicit SrEventDock(sr_event_controller *eventController, QWidget *parent = nullptr)
		: QWidget(parent),
		  controller(eventController)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(2, 2, 2, 2);
		root->setSpacing(2);
		setStyleSheet(QStringLiteral("QPushButton { padding: 0px 4px; min-height: 18px; }"
					     "QToolButton { padding: 0px 4px; min-height: 18px; }"
					     "QComboBox { padding: 0px 3px; min-height: 18px; }"
					     "QTableWidget::item { padding: 0px 2px; }"
					     "QHeaderView::section { padding: 1px 4px; }"
					     "QTabBar::tab { padding: 2px 9px; min-width: 24px; min-height: 18px; }"));

		auto *operatorHint = new QLabel(T("EventDock.OperatorHint"), this);
		operatorHint->setWordWrap(true);
		operatorHint->setStyleSheet(
			QStringLiteral("QLabel { color: palette(text); background: transparent; "
				       "border: 1px solid palette(mid); border-radius: 3px; padding: 2px; }"));
		root->addWidget(operatorHint);

		auto *recordBar = new QHBoxLayout();
		recordBar->setSpacing(3);
		recordToggle = new QToolButton(this);
		recordToggle->setText(QStringLiteral("● REC"));
		recordToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
		recordToggle->setMinimumSize(116, 36);
		auto recordFont = recordToggle->font();
		recordFont.setPointSizeF(recordFont.pointSizeF() * 1.35);
		recordFont.setBold(true);
		recordToggle->setFont(recordFont);
		recordToggle->setAutoRaise(false);
		auto *settingsGear = new QToolButton(this);
		settingsGear->setText(QString::fromUtf8("\xE2\x9A\x99"));
		settingsGear->setToolTip(T("Dock.Settings"));
		settingsGear->setAutoRaise(true);
		settingsGear->setFixedWidth(28);
		recordStatus = new QLabel(this);
		recordStatus->setWordWrap(true);
		setupSourceStatus = new QLabel(this);
		setupSourceStatus->setAlignment(Qt::AlignCenter);
		setupSourceStatus->setMinimumWidth(76);
		setupSourceStatus->setStyleSheet(QStringLiteral("color: gray;"));
		setupButton = new QToolButton(this);
		setupButton->setText(T("EventDock.Setup.Button"));
		setupButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
		setupButton->setAutoRaise(true);
		setupButton->setMinimumWidth(82);
		auto *repairABButton = new QToolButton(this);
		repairABButton->setText(T("EventDock.Setup.CreateAB"));
		repairABButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
		repairABButton->setAutoRaise(true);
		repairABButton->setToolTip(T("EventDock.Setup.CreateAB"));
		recordBar->addWidget(recordToggle);
		recordBar->addWidget(repairABButton);
		recordBar->addWidget(settingsGear);
		recordBar->addWidget(setupSourceStatus);
		recordBar->addWidget(setupButton);
		recordBar->addWidget(recordStatus, 1);
		root->addLayout(recordBar);

		performancePanel = new QWidget(this);
		auto *performanceLayout = new QVBoxLayout(performancePanel);
		performanceLayout->setContentsMargins(0, 0, 0, 0);
		performanceLayout->setSpacing(1);
		performanceSummary = new QLabel(performancePanel);
		performanceSummary->setWordWrap(true);
		performanceSummary->setStyleSheet(QStringLiteral("color: gray;"));
		performanceLayout->addWidget(performanceSummary);
		performanceTable = new QTableWidget(performancePanel);
		performanceTable->setColumnCount(8);
		performanceTable->setHorizontalHeaderLabels(
			{T("EventDock.Performance.Camera"), T("EventDock.Performance.Path"),
			 T("EventDock.Performance.Video"), T("EventDock.Performance.Gop"),
			 T("EventDock.Performance.Queue"), T("EventDock.Performance.Packets"),
			 T("EventDock.Performance.Drops"), T("EventDock.Performance.Disk")});
		performanceTable->setSelectionMode(QAbstractItemView::NoSelection);
		performanceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
		performanceTable->setFocusPolicy(Qt::NoFocus);
		performanceTable->setAlternatingRowColors(true);
		performanceTable->verticalHeader()->setVisible(false);
		performanceTable->verticalHeader()->setMinimumSectionSize(16);
		performanceTable->horizontalHeader()->setStretchLastSection(false);
		performanceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		for (int column = 1; column < performanceTable->columnCount(); column++)
			performanceTable->horizontalHeader()->setSectionResizeMode(column,
										   QHeaderView::ResizeToContents);
		performanceTable->verticalHeader()->setDefaultSectionSize(18);
		performanceTable->setMinimumHeight(58);
		performanceTable->setMaximumHeight(108);
		performanceLayout->addWidget(performanceTable);
		performancePanel->setVisible(false);

		auto *programBar = new QHBoxLayout();
		programBar->setSpacing(3);
		programStatus = new QLabel(this);
		cueAStatus = new QLabel(this);
		cueBStatus = new QLabel(this);
		for (QLabel *label : {programStatus, cueAStatus, cueBStatus}) {
			label->setAlignment(Qt::AlignCenter);
			label->setMinimumHeight(20);
			programBar->addWidget(label, 1);
		}
		root->addLayout(programBar);

		listTabs = new QTabBar(this);
		listTabs->setExpanding(false);
		listTabs->setUsesScrollButtons(true);
		listTabs->setElideMode(Qt::ElideNone);
		for (unsigned i = 1; i <= SR_EVENT_LIST_COUNT; i++) {
			const int index = listTabs->addTab(QString::number(i));
			listTabs->setTabData(index, i);
		}
		listTabs->setCurrentIndex(0);
		root->addWidget(listTabs);

		auto *markBar = new QHBoxLayout();
		markBar->setSpacing(3);

		auto *markIn = new QPushButton(QStringLiteral("IN"), this);
		auto *markOut = new QPushButton(QStringLiteral("OUT"), this);
		auto *mark5 = new QPushButton(QStringLiteral("-5"), this);
		auto *mark10 = new QPushButton(QStringLiteral("-10"), this);
		auto *mark20 = new QPushButton(QStringLiteral("-20"), this);
		markIn->setToolTip(T("Hotkey.EventIn"));
		markOut->setToolTip(T("Hotkey.EventOut"));
		mark5->setToolTip(T("Hotkey.EventLast5"));
		mark10->setToolTip(T("Hotkey.EventLast10"));
		mark20->setToolTip(T("Hotkey.EventLast20"));
		markBar->addWidget(markIn);
		markBar->addWidget(markOut);
		markBar->addWidget(mark5);
		markBar->addWidget(mark10);
		markBar->addWidget(mark20);
		markBar->addStretch(1);
		root->addLayout(markBar);

		auto *eventViewBar = new QHBoxLayout();
		eventViewBar->setSpacing(3);
		eventViewBar->addStretch(1);
		auto *viewListButton = new QToolButton(this);
		viewListButton->setText(QStringLiteral("☷ ") + T("EventDock.ViewList"));
		viewListButton->setCheckable(true);
		viewListButton->setChecked(true);
		auto *viewThumbButton = new QToolButton(this);
		viewThumbButton->setText(QStringLiteral("▦ ") + T("EventDock.ViewThumbnails"));
		viewThumbButton->setCheckable(true);
		eventViewBar->addWidget(viewListButton);
		eventViewBar->addWidget(viewThumbButton);
		root->addLayout(eventViewBar);

		eventViewStack = new QStackedWidget(this);
		table = new SrEventTable(eventViewStack);
		table->setColumnCount(6);
		table->setHorizontalHeaderLabels({T("EventDock.Column.Id"), T("EventDock.Column.Duration"),
						  T("EventDock.Column.Speed"), T("EventDock.Column.State"),
						  T("EventDock.Column.Name"), T("EventDock.Column.Tag")});
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::ExtendedSelection);
		table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
		table->verticalHeader()->setVisible(false);
		table->verticalHeader()->setMinimumSectionSize(16);
		table->verticalHeader()->setDefaultSectionSize(18);
		table->horizontalHeader()->setFixedHeight(21);
		table->horizontalHeader()->setStretchLastSection(true);
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
		eventViewStack->addWidget(table);

		thumbnailList = new QListWidget(eventViewStack);
		thumbnailList->setViewMode(QListView::IconMode);
		thumbnailList->setResizeMode(QListView::Adjust);
		thumbnailList->setMovement(QListView::Static);
		thumbnailList->setWrapping(true);
		thumbnailList->setWordWrap(true);
		thumbnailList->setSelectionMode(QAbstractItemView::ExtendedSelection);
		thumbnailList->setSelectionRectVisible(true);
		thumbnailList->setIconSize(QSize(EVENT_THUMB_DISPLAY_WIDTH, EVENT_THUMB_DISPLAY_HEIGHT));
		thumbnailList->setGridSize(QSize(EVENT_THUMB_CELL_WIDTH, EVENT_THUMB_CELL_HEIGHT));
		thumbnailList->setSpacing(1);
		thumbnailList->setUniformItemSizes(true);
		thumbnailList->setItemDelegate(new SrEventThumbnailDelegate(thumbnailList));
		thumbnailList->setToolTip(T("EventDock.ViewThumbnails.Tooltip"));
		eventViewStack->addWidget(thumbnailList);
		eventViewStack->setCurrentWidget(table);
		root->addWidget(eventViewStack, 1);

		auto *actionBar = new QHBoxLayout();
		actionBar->setSpacing(3);
		auto *up = new QPushButton(QStringLiteral("↑"), this);
		auto *down = new QPushButton(QStringLiteral("↓"), this);
		auto *played = new QPushButton(T("EventDock.Played"), this);
		auto *protect = new QPushButton(T("EventDock.Protect"), this);
		protect->setToolTip(T("EventDock.Protect.Tooltip"));
		auto *remove = new QPushButton(T("EventDock.Delete"), this);
		auto *removeMedia = new QPushButton(T("EventDock.DeleteMedia"), this);
		actionBar->addWidget(up);
		actionBar->addWidget(down);
		actionBar->addWidget(played);
		actionBar->addWidget(protect);
		actionBar->addWidget(remove);
		actionBar->addWidget(removeMedia);
		actionBar->addStretch(1);
		actionBar->addWidget(new QLabel(T("EventDock.TargetList"), this));
		targetCombo = new QComboBox(this);
		for (unsigned i = 1; i <= SR_EVENT_LIST_COUNT; i++)
			targetCombo->addItem(QString::number(i), i);
		targetCombo->setCurrentIndex(1);
		actionBar->addWidget(targetCombo);
		auto *copy = new QPushButton(T("EventDock.Copy"), this);
		auto *move = new QPushButton(T("EventDock.Move"), this);
		auto *duplicate = new QPushButton(T("EventDock.Duplicate"), this);
		actionBar->addWidget(copy);
		actionBar->addWidget(move);
		actionBar->addWidget(duplicate);
		root->addLayout(actionBar);

		auto *cueBar = new QHBoxLayout();
		cueBar->setSpacing(3);
		cueBar->addWidget(new QLabel(T("EventDock.Camera"), this));
		cameraCombo = new QComboBox(this);
		cameraCombo->setMinimumContentsLength(18);
		cueBar->addWidget(cameraCombo, 1);
		auto *setPreferred = new QPushButton(T("EventDock.SetPreferred"), this);
		auto *clearPreferred = new QPushButton(T("EventDock.ClearPreferred"), this);
		auto *cueA = new QPushButton(T("EventDock.CueA"), this);
		auto *cueB = new QPushButton(T("EventDock.CueB"), this);
		cueBar->addWidget(setPreferred);
		cueBar->addWidget(clearPreferred);
		cueBar->addWidget(cueA);
		cueBar->addWidget(cueB);
		cueBar->addSpacing(6);
		cueBar->addWidget(new QLabel(T("EventDock.TransportBus"), this));
		busCombo = new QComboBox(this);
		busCombo->addItem(QStringLiteral("A"), SR_REPLAY_BUS_A);
		busCombo->addItem(QStringLiteral("B"), SR_REPLAY_BUS_B);
		cueBar->addWidget(busCombo);
		auto *playPause = new QPushButton(T("EventDock.PlayPause"), this);
		auto *stop = new QPushButton(T("EventDock.Stop"), this);
		auto *restart = new QPushButton(T("EventDock.Restart"), this);
		auto *prevFrame = new QPushButton(T("EventDock.PrevFrame"), this);
		auto *nextFrame = new QPushButton(T("EventDock.NextFrame"), this);
		prevFrame->setToolTip(T("EventDock.PrevFrame.Tooltip"));
		nextFrame->setToolTip(T("EventDock.NextFrame.Tooltip"));
		reverseButton = new QPushButton(T("EventDock.Reverse"), this);
		reverseButton->setCheckable(true);
		loopButton = new QPushButton(T("EventDock.Loop"), this);
		loopButton->setCheckable(true);
		cueBar->addWidget(playPause);
		cueBar->addWidget(stop);
		cueBar->addWidget(restart);
		cueBar->addWidget(prevFrame);
		cueBar->addWidget(nextFrame);
		cueBar->addWidget(reverseButton);
		cueBar->addWidget(loopButton);
		speedCombo = new QComboBox(this);
		const int speeds[] = {25, 33, 50, 75, 100};
		for (int speed : speeds)
			speedCombo->addItem(QStringLiteral("%1%").arg(speed), speed);
		speedCombo->setCurrentIndex(speedCombo->findData(100));
		speedCombo->setToolTip(T("EventDock.Speed.Tooltip"));
		cueBar->addWidget(speedCombo);
		cueBar->addWidget(new QLabel(T("EventDock.Audio"), this));
		audioCombo = new QComboBox(this);
		audioCombo->addItem(T("EventDock.AudioMaster"), SR_REPLAY_AUDIO_MASTER);
		audioCombo->addItem(T("EventDock.AudioCamera"), SR_REPLAY_AUDIO_CAMERA);
		audioCombo->addItem(T("EventDock.AudioOff"), SR_REPLAY_AUDIO_OFF);
		cueBar->addWidget(audioCombo);
		root->addLayout(cueBar);

		auto *angleHeader = new QHBoxLayout();
		angleHeader->setSpacing(3);
		angleHeader->addWidget(new QLabel(T("EventDock.Angles"), this));
		angleHeader->addStretch(1);
		auto *angleLegend = new QLabel(T("EventDock.AnglesLegend"), this);
		angleLegend->setStyleSheet(QStringLiteral("color: gray;"));
		angleHeader->addWidget(angleLegend);
		root->addLayout(angleHeader);
		angleGrid = new QGridLayout();
		angleGrid->setHorizontalSpacing(4);
		angleGrid->setVerticalSpacing(4);
		root->addLayout(angleGrid);

		auto *timelineBar = new QHBoxLayout();
		timelineBar->setSpacing(3);
		timelineBar->addWidget(new QLabel(T("EventDock.Timeline"), this));
		timelineSlider = new SrRangeSlider(this);
		timelineSlider->setRange(0, 10000);
		timelineSlider->setSingleStep(1);
		timelineSlider->setPageStep(100);
		timelineSlider->setEnabled(false);
		timelineSlider->setToolTip(T("EventDock.Timeline.Tooltip"));
		timelineBar->addWidget(timelineSlider, 1);
		timelineTime = new QLabel(QStringLiteral("--:--.--- / --:--.---"), this);
		timelineTime->setMinimumWidth(130);
		timelineBar->addWidget(timelineTime);
		root->addLayout(timelineBar);

		auto *jogShuttleBar = new QHBoxLayout();
		jogShuttleBar->setSpacing(3);
		jogShuttleBar->addWidget(new QLabel(T("EventDock.Jog"), this));
		jogSlider = new QSlider(Qt::Horizontal, this);
		jogSlider->setRange(-24, 24);
		jogSlider->setValue(0);
		jogSlider->setSingleStep(1);
		jogSlider->setPageStep(1);
		jogSlider->setToolTip(T("EventDock.Jog.Tooltip"));
		jogShuttleBar->addWidget(jogSlider, 1);
		jogShuttleBar->addSpacing(4);
		jogShuttleBar->addWidget(new QLabel(T("EventDock.Shuttle"), this));
		shuttleSlider = new QSlider(Qt::Horizontal, this);
		shuttleSlider->setRange(-5, 5);
		shuttleSlider->setValue(0);
		shuttleSlider->setSingleStep(1);
		shuttleSlider->setPageStep(1);
		shuttleSlider->setToolTip(T("EventDock.Shuttle.Tooltip"));
		jogShuttleBar->addWidget(shuttleSlider, 1);
		shuttleValue = new QLabel(QStringLiteral("0"), this);
		shuttleValue->setMinimumWidth(42);
		jogShuttleBar->addWidget(shuttleValue);
		root->addLayout(jogShuttleBar);

		auto *takeBar = new QHBoxLayout();
		takeBar->setSpacing(3);
		auto *playSelected = new QPushButton(T("EventDock.PlaySelected"), this);
		auto *playAll = new QPushButton(T("EventDock.PlayAll"), this);
		auto *playEachAngleButton = new QPushButton(T("EventDock.PlayEachAngle"), this);
		auto *playLast = new QPushButton(T("EventDock.PlayLast"), this);
		auto *playByIdButton = new QPushButton(T("EventDock.PlayById"), this);
		auto *playlistA = new QPushButton(T("EventDock.PlaylistA"), this);
		auto *playlistB = new QPushButton(T("EventDock.PlaylistB"), this);
		auto *playlistNext = new QPushButton(T("EventDock.PlaylistNext"), this);
		auto *playlistStop = new QPushButton(T("EventDock.PlaylistStop"), this);
		auto *takeA = new QPushButton(T("EventDock.TakeA"), this);
		auto *takeB = new QPushButton(T("EventDock.TakeB"), this);
		auto *takeToggle = new QPushButton(T("EventDock.TakeToggle"), this);
		auto *returnLive = new QPushButton(T("EventDock.ReturnLive"), this);
		playSelected->setStyleSheet(QStringLiteral("font-weight: bold;"));
		for (QPushButton *button :
		     {playSelected, playAll, playEachAngleButton, playLast, playByIdButton, playlistA, playlistB,
		      playlistNext, playlistStop, takeA, takeB, takeToggle, returnLive})
			button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
		takeBar->addWidget(playSelected);
		takeBar->addWidget(playAll);
		takeBar->addWidget(playEachAngleButton);
		takeBar->addWidget(playLast);
		takeBar->addWidget(playByIdButton);
		takeBar->addSpacing(4);
		takeBar->addWidget(playlistA);
		takeBar->addWidget(playlistB);
		takeBar->addWidget(playlistNext);
		takeBar->addWidget(playlistStop);
		takeBar->addSpacing(4);
		takeBar->addWidget(takeA);
		takeBar->addWidget(takeB);
		takeBar->addWidget(takeToggle);
		takeBar->addWidget(returnLive);
		root->addLayout(takeBar);

		auto *exportBar = new QHBoxLayout();
		exportBar->setSpacing(3);
		exportBar->addWidget(new QLabel(T("EventDock.Export"), this));
		exportModeCombo = new QComboBox(this);
		exportModeCombo->addItem(T("EventDock.ExportPreferred"), 0);
		exportModeCombo->addItem(T("EventDock.ExportAll"), 1);
		exportBar->addWidget(exportModeCombo);
		auto *exportFast = new QPushButton(T("EventDock.ExportFast"), this);
		exportCancelButton = new QPushButton(T("EventDock.ExportCancel"), this);
		exportCancelButton->setEnabled(false);
		exportProgressBar = new QProgressBar(this);
		exportProgressBar->setRange(0, 100);
		exportProgressBar->setValue(0);
		exportProgressBar->setMinimumWidth(120);
		exportBar->addWidget(exportFast);
		exportBar->addWidget(exportCancelButton);
		exportBar->addWidget(exportProgressBar, 1);
		root->addLayout(exportBar);

		transportStatus = new QLabel(this);
		transportStatus->setStyleSheet(QStringLiteral("color: gray;"));
		transportStatus->setWordWrap(true);
		root->addWidget(transportStatus);

		status = new QLabel(T("EventDock.Ready"), this);
		status->setStyleSheet(QStringLiteral("color: gray;"));
		root->addWidget(status);

		performanceToggle = new QToolButton(this);
		performanceToggle->setText(T("EventDock.Performance.Title"));
		performanceToggle->setCheckable(true);
		performanceToggle->setChecked(false);
		performanceToggle->setArrowType(Qt::RightArrow);
		performanceToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		performanceToggle->setAutoRaise(true);
		root->addWidget(performanceToggle);
		root->addWidget(performancePanel);

		connect(listTabs, &QTabBar::currentChanged, this, [this](int index) {
			if (!controller || index < 0)
				return;
			sr_event_controller_set_current_list(controller, currentList());
			refresh();
		});
		connect(markIn, &QPushButton::clicked, this, [this]() { setMarkIn(); });
		connect(markOut, &QPushButton::clicked, this, [this]() { setMarkOut(); });
		connect(mark5, &QPushButton::clicked, this, [this]() { quickMark(5); });
		connect(mark10, &QPushButton::clicked, this, [this]() { quickMark(10); });
		connect(mark20, &QPushButton::clicked, this, [this]() { quickMark(20); });
		connect(recordToggle, &QToolButton::clicked, this, [this]() { toggleAllRecording(); });
		connect(repairABButton, &QToolButton::clicked, this, [this]() {
			sr_replay_setup_result result = {};
			if (!sr_replay_setup_ensure_event_scenes(&result)) {
				QMessageBox::warning(this, T("EventDock.Setup.Title"), T("EventDock.Setup.ABFailed"));
			} else {
				status->setText(T("EventDock.Setup.ABReady")
							.arg(QString::fromUtf8(result.scene_a))
							.arg(QString::fromUtf8(result.scene_b)));
			}
			refreshSetupStatus();
		});
		connect(settingsGear, &QToolButton::clicked, this, []() { sr_dock_open_settings(); });
		connect(setupButton, &QToolButton::clicked, this, [this]() { openReplaySetup(); });
		connect(up, &QPushButton::clicked, this, [this]() { moveRow(-1); });
		connect(down, &QPushButton::clicked, this, [this]() { moveRow(1); });
		connect(played, &QPushButton::clicked, this, [this]() { togglePlayed(); });
		connect(protect, &QPushButton::clicked, this, [this]() { toggleProtected(); });
		connect(remove, &QPushButton::clicked, this, [this]() { deleteSelected(false); });
		connect(removeMedia, &QPushButton::clicked, this, [this]() { deleteSelected(true); });
		connect(copy, &QPushButton::clicked, this, [this]() { copySelected(); });
		connect(move, &QPushButton::clicked, this, [this]() { moveSelected(); });
		connect(duplicate, &QPushButton::clicked, this, [this]() { duplicateSelected(); });
		connect(exportFast, &QPushButton::clicked, this, [this]() { startExport(); });
		connect(exportCancelButton, &QPushButton::clicked, this, [this]() { cancelExport(); });
		connect(setPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(false); });
		connect(clearPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(true); });
		connect(cueA, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_A); });
		connect(cueB, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_B); });
		connect(busCombo, &QComboBox::currentIndexChanged, this, [this](int) { syncTransportControls(); });
		connect(playPause, &QPushButton::clicked, this, [this]() { togglePlayPause(); });
		connect(stop, &QPushButton::clicked, this, [this]() { stopTransport(); });
		connect(restart, &QPushButton::clicked, this, [this]() { restartTransport(); });
		connect(prevFrame, &QPushButton::clicked, this, [this]() { stepFrame(-1); });
		connect(nextFrame, &QPushButton::clicked, this, [this]() { stepFrame(1); });
		connect(reverseButton, &QPushButton::clicked, this,
			[this](bool checked) { sr_replay_channel_set_backward(transportBus(), checked); });
		connect(loopButton, &QPushButton::clicked, this,
			[this](bool checked) { sr_replay_channel_set_loop(transportBus(), checked); });
		connect(speedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (index >= 0)
				setOperatorSpeed(speedCombo->itemData(index).toDouble());
		});
		connect(audioCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (index >= 0)
				sr_replay_channel_set_audio_mode(
					transportBus(),
					static_cast<sr_replay_audio_mode>(audioCombo->itemData(index).toInt()));
			refreshTransportStatus();
		});
		connect(timelineSlider, &QSlider::sliderPressed, this, [this]() {
			timelineDragging = true;
			sr_replay_channel_pause(transportBus(), true);
		});
		connect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });
		connect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
			seekTimeline(timelineSlider->value());
			timelineDragging = false;
			syncTimeline();
		});
		timelineSlider->setClickHandler([this](int value) {
			seekTimeline(value);
			syncTimeline();
		});
		timelineSlider->setRangeHandler(
			[this](int rangeIn, int rangeOut) { createRangeEvent(rangeIn, rangeOut); });
		connect(jogSlider, &QSlider::sliderPressed, this, [this]() { jogLastValue = jogSlider->value(); });
		connect(jogSlider, &QSlider::sliderMoved, this, [this](int value) { jogMoved(value); });
		connect(jogSlider, &QSlider::sliderReleased, this, [this]() {
			const QSignalBlocker blocker(jogSlider);
			jogSlider->setValue(0);
			jogLastValue = 0;
		});
		connect(shuttleSlider, &QSlider::valueChanged, this, [this](int value) { applyShuttle(value); });
		connect(playSelected, &QPushButton::clicked, this, [this]() { playSelectedEvent(); });
		connect(playAll, &QPushButton::clicked, this, [this]() { startPlaylist(transportBus()); });
		connect(playEachAngleButton, &QPushButton::clicked, this, [this]() { playEachAngle(); });
		connect(playLast, &QPushButton::clicked, this, [this]() { playLastEvent(); });
		connect(playByIdButton, &QPushButton::clicked, this, [this]() { playById(); });
		connect(playlistA, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_A); });
		connect(playlistB, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_B); });
		connect(playlistNext, &QPushButton::clicked, this, [this]() { nextPlaylist(); });
		connect(playlistStop, &QPushButton::clicked, this, [this]() { stopPlaylist(); });
		connect(performanceToggle, &QToolButton::toggled, this, [this](bool expanded) {
			performanceToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
			performancePanel->setVisible(expanded);
			if (expanded)
				refreshHardwareStatus();
		});
		connect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });
		connect(takeB, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_B); });
		connect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });
		connect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });
		connect(viewListButton, &QToolButton::clicked, this, [this, viewListButton, viewThumbButton]() {
			viewListButton->setChecked(true);
			viewThumbButton->setChecked(false);
			eventViewStack->setCurrentWidget(table);
			syncGallerySelectionFromTable();
		});
		connect(viewThumbButton, &QToolButton::clicked, this, [this, viewListButton, viewThumbButton]() {
			viewListButton->setChecked(false);
			viewThumbButton->setChecked(true);
			eventViewStack->setCurrentWidget(thumbnailList);
			refreshEventGallery();
			syncGallerySelectionFromTable();
			requestEventThumbnails();
		});
		connect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
			if (!syncingEventViews)
				syncGallerySelectionFromTable();
			refreshAngleCoverage();
		});
		connect(thumbnailList, &QListWidget::itemSelectionChanged, this, [this]() {
			if (!syncingEventViews)
				syncTableSelectionFromGallery();
		});
		connect(thumbnailList, &QListWidget::itemDoubleClicked, this,
			[this](QListWidgetItem *) { playSelectedEvent(); });
		connect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) { editEvent(item); });
		connect(table->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
			[this]() { tableEditing = false; });
		connect(table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
			if (item && (item->column() == 2 || item->column() == 4 || item->column() == 5))
				tableEditing = true;
			else if (item)
				playSelectedEvent();
		});

		refreshTimer = new QTimer(this);
		refreshTimer->setInterval(750);
		connect(refreshTimer, &QTimer::timeout, this, [this]() {
			refresh();
			refreshTransportStatus();
			refreshRecordingStatus();
			refreshHardwareStatus();
			if (++cameraRefreshTicks >= 4) {
				cameraRefreshTicks = 0;
				refreshCameras();
				refreshSetupStatus();
			}
		});
		refreshTimer->start();

		transportTimer = new QTimer(this);
		transportTimer->setInterval(100);
		connect(transportTimer, &QTimer::timeout, this, [this]() {
			refreshTransportStatus();
			refreshProgramState();
			syncTimeline();
			syncAngleButtonState();
			pollExport();
			pollAnglePreviews();
			pollEventThumbnails();
		});
		transportTimer->start();

		if (controller)
			sr_event_controller_set_current_list(controller, currentList());
		refreshCameras();
		refresh();
		refreshTransportStatus();
		refreshProgramState();
		refreshRecordingStatus();
		refreshSetupStatus();
		refreshHardwareStatus();
		syncTransportControls();
	}

	~SrEventDock() override
	{
		if (exportJob) {
			exportJob->cancel.store(true, std::memory_order_relaxed);
			if (exportJob->worker.joinable())
				exportJob->worker.join();
		}
		if (anglePreviewJob && anglePreviewJob->worker.joinable())
			anglePreviewJob->worker.join();
		if (eventThumbnailJob && eventThumbnailJob->worker.joinable())
			eventThumbnailJob->worker.join();
	}

private:
	unsigned currentList() const
	{
		return listTabs && listTabs->currentIndex() >= 0 ? listTabs->tabData(listTabs->currentIndex()).toUInt()
								 : 1;
	}

	unsigned targetList() const { return targetCombo ? targetCombo->currentData().toUInt() : 1; }

	enum sr_replay_bus transportBus() const
	{
		return busCombo && busCombo->currentData().toInt() == SR_REPLAY_BUS_B ? SR_REPLAY_BUS_B
										      : SR_REPLAY_BUS_A;
	}

	QString selectedCamera() const { return cameraCombo ? cameraCombo->currentData().toString() : QString(); }

	bool thumbnailViewActive() const

	{
		return eventViewStack && thumbnailList && eventViewStack->currentWidget() == thumbnailList;
	}

	QListWidgetItem *galleryItemById(uint64_t eventId) const

	{
		if (!thumbnailList || !eventId)
			return nullptr;
		for (int i = 0; i < thumbnailList->count(); i++) {
			QListWidgetItem *item = thumbnailList->item(i);
			if (item && item->data(Qt::UserRole).toULongLong() == eventId)
				return item;
		}
		return nullptr;
	}

	void syncGallerySelectionFromTable()

	{
		if (!table || !thumbnailList || syncingEventViews)
			return;
		syncingEventViews = true;
		const QSignalBlocker blocker(thumbnailList);
		thumbnailList->clearSelection();
		const std::vector<uint64_t> ids = selectedEventIds();
		const uint64_t currentId = selectedEventId();
		for (uint64_t id : ids) {
			if (QListWidgetItem *item = galleryItemById(id))
				item->setSelected(true);
		}
		if (QListWidgetItem *current = galleryItemById(currentId))
			thumbnailList->setCurrentItem(current, QItemSelectionModel::NoUpdate);
		syncingEventViews = false;
	}

	void syncTableSelectionFromGallery()

	{
		if (!table || !thumbnailList || syncingEventViews)
			return;
		syncingEventViews = true;
		const QSignalBlocker blocker(table);
		table->clearSelection();
		uint64_t currentId = 0;
		if (QListWidgetItem *current = thumbnailList->currentItem())
			currentId = current->data(Qt::UserRole).toULongLong();
		int currentRow = -1;
		for (int row = 0; row < table->rowCount(); row++) {
			QTableWidgetItem *idItem = table->item(row, 0);
			const uint64_t id = idItem ? idItem->data(Qt::UserRole).toULongLong() : 0;
			QListWidgetItem *galleryItem = galleryItemById(id);
			if (galleryItem && galleryItem->isSelected())
				table->selectionModel()->select(table->model()->index(row, 0),
								QItemSelectionModel::Select |
									QItemSelectionModel::Rows);
			if (id && id == currentId)
				currentRow = row;
		}
		if (currentRow >= 0)
			table->setCurrentCell(currentRow, 0, QItemSelectionModel::NoUpdate);
		syncingEventViews = false;
		refreshAngleCoverage();
	}

	QString eventGalleryText(const sr_event_record &event) const

	{
		const QString name = QString::fromUtf8(event.name ? event.name : "").trimmed();
		const QString tag = QString::fromUtf8(event.tag ? event.tag : "").trimmed();
		QString first = QStringLiteral("#%1").arg(event.id);
		if (!name.isEmpty())
			first += QStringLiteral("  ") + name;
		const bool inherited = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL &&
				       !event.speed_override;
		const QString speed = inherited ? QStringLiteral("--")
						: QStringLiteral("%1%").arg(event.speed_percent, 0, 'f', 0);
		QString second = QStringLiteral("%1  ·  %2").arg(durationText(event)).arg(speed);
		if (!tag.isEmpty())
			second += QStringLiteral("  ·  ") + tag;
		return first + QStringLiteral("\n") + second;
	}

	bool makeEventThumbnailTask(const sr_event_record &event, const QStringList &cameras, EventThumbnailTask *task)

	{
		if (!task || cameras.isEmpty() || event.out_ns <= event.in_ns)
			return false;

		QString preferred;
		if (event.preferred_camera_id) {
			char *name = nullptr;
			if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &name) && name)
				preferred = QString::fromUtf8(name);
			bfree(name);
		}

		auto prepare = [&](const QString &camera, bool fullOnly) -> bool {
			if (camera.isEmpty())
				return false;
			sr_replay_coverage_info coverage = {};
			const QByteArray cameraUtf8 = camera.toUtf8();
			if (!sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns, &coverage) ||
			    coverage.coverage == SR_REPLAY_COVERAGE_NONE ||
			    (fullOnly && coverage.coverage != SR_REPLAY_COVERAGE_FULL))
				return false;
			uint64_t timestamp = event.in_ns + (event.out_ns - event.in_ns) / 2;
			if (coverage.coverage == SR_REPLAY_COVERAGE_PARTIAL &&
			    coverage.playable_out_ns > coverage.playable_in_ns)
				timestamp = coverage.playable_in_ns +
					    (coverage.playable_out_ns - coverage.playable_in_ns) / 2;
			const int64_t offset = coverage.sync_offset_ns;
			if (offset >= 0 && (uint64_t)offset <= UINT64_MAX - timestamp)
				timestamp += (uint64_t)offset;
			else if (offset < 0 && (uint64_t)(-offset) < timestamp)
				timestamp -= (uint64_t)(-offset);
			task->eventId = event.id;
			task->inNs = event.in_ns;
			task->outNs = event.out_ns;
			task->camera = cameraUtf8.constData();
			task->timestampNs = timestamp;
			return true;
		};

		if (!preferred.isEmpty() && prepare(preferred, false))
			return true;
		for (const QString &camera : cameras) {
			if (camera != preferred && prepare(camera, true))
				return true;
		}
		for (const QString &camera : cameras) {
			if (camera != preferred && prepare(camera, false))
				return true;
		}
		return false;
	}

	void refreshEventGallery()

	{
		if (!thumbnailList || !table || !controller)
			return;

		std::vector<uint64_t> ids;
		ids.reserve((size_t)table->rowCount());
		for (int row = 0; row < table->rowCount(); row++) {
			QTableWidgetItem *item = table->item(row, 0);
			const uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
			if (id)
				ids.push_back(id);
		}

		const bool structureChanged = galleryListId != currentList() || ids != galleryEventIds;
		if (structureChanged) {
			galleryListId = currentList();
			galleryEventIds = ids;
			galleryGeneration++;
			const QSignalBlocker blocker(thumbnailList);
			thumbnailList->clear();
			for (uint64_t id : galleryEventIds) {
				auto *item = new QListWidgetItem(thumbnailList);
				item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
				item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
			}
		}

		for (int i = 0; i < thumbnailList->count(); i++) {
			QListWidgetItem *item = thumbnailList->item(i);
			const uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
			sr_event_record event = {};
			if (!id || !sr_event_controller_get_event(controller, id, &event))
				continue;
			item->setText(eventGalleryText(event));
			item->setToolTip(QStringLiteral("#%1 · %2 · %3")
						 .arg(event.id)
						 .arg(stateText(event))
						 .arg(QString::fromUtf8(event.tag ? event.tag : "")));
			item->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(event.in_ns));
			item->setData(Qt::UserRole + 2, QVariant::fromValue<qulonglong>(event.out_ns));
			auto cached = eventThumbnailCache.find(id);
			if (cached != eventThumbnailCache.end() && cached->second.inNs == event.in_ns &&
			    cached->second.outNs == event.out_ns)
				item->setIcon(cached->second.icon);
			else
				item->setIcon(QIcon());
			sr_event_controller_free_event(&event);
		}

		if (structureChanged)
			syncGallerySelectionFromTable();
		if (thumbnailViewActive())
			requestEventThumbnails();
	}

	void requestEventThumbnails()

	{
		if (!thumbnailViewActive() || eventThumbnailJob || !controller || !thumbnailList)
			return;
		const QStringList cameras = captureCameraNames();
		if (cameras.isEmpty())
			return;

		auto job = std::make_unique<EventThumbnailJob>();
		job->generation = galleryGeneration;
		for (int i = 0; i < thumbnailList->count() && job->tasks.size() < EVENT_THUMB_BATCH; i++) {
			QListWidgetItem *item = thumbnailList->item(i);
			const uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
			const uint64_t inNs = item ? item->data(Qt::UserRole + 1).toULongLong() : 0;
			const uint64_t outNs = item ? item->data(Qt::UserRole + 2).toULongLong() : 0;
			auto cached = eventThumbnailCache.find(id);
			if (cached != eventThumbnailCache.end() && cached->second.inNs == inNs &&
			    cached->second.outNs == outNs && !cached->second.icon.isNull())
				continue;
			sr_event_record event = {};
			if (!id || !sr_event_controller_get_event(controller, id, &event))
				continue;
			EventThumbnailTask task;
			if (makeEventThumbnailTask(event, cameras, &task))
				job->tasks.emplace_back(std::move(task));
			sr_event_controller_free_event(&event);
		}
		if (job->tasks.empty())
			return;
		char *sessionPath = sr_session_get_or_create_path();
		if (!sessionPath)
			return;
		job->sessionDir = sessionPath;
		bfree(sessionPath);
		eventThumbnailJob = std::move(job);
		EventThumbnailJob *workerJob = eventThumbnailJob.get();
		workerJob->worker = std::thread([workerJob]() { runEventThumbnailJob(workerJob); });
	}

	void pollEventThumbnails()

	{
		if (!eventThumbnailJob || !eventThumbnailJob->done.load(std::memory_order_acquire))
			return;
		if (eventThumbnailJob->worker.joinable())
			eventThumbnailJob->worker.join();
		if (eventThumbnailJob->generation == galleryGeneration) {
			for (const EventThumbnailResult &result : eventThumbnailJob->results) {
				if (result.rgba.empty())
					continue;
				QListWidgetItem *item = galleryItemById(result.eventId);
				if (!item || item->data(Qt::UserRole + 1).toULongLong() != result.inNs ||
				    item->data(Qt::UserRole + 2).toULongLong() != result.outNs)
					continue;
				const QImage image(result.rgba.data(), EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT,
						   EVENT_THUMB_WIDTH * 4, QImage::Format_RGBA8888);
				CachedEventThumbnail cached;
				cached.inNs = result.inNs;
				cached.outNs = result.outNs;
				cached.icon = QIcon(QPixmap::fromImage(image.copy()));
				eventThumbnailCache[result.eventId] = cached;
				item->setIcon(cached.icon);
			}
		}
		eventThumbnailJob.reset();
		requestEventThumbnails();
	}

	uint64_t selectedEventId() const
	{
		const int row = table ? table->currentRow() : -1;
		if (row < 0)
			return 0;
		QTableWidgetItem *item = table->item(row, 0);
		return item ? item->data(Qt::UserRole).toULongLong() : 0;
	}

	std::vector<uint64_t> selectedEventIds() const
	{
		std::vector<uint64_t> ids;
		if (!table || !table->selectionModel())
			return ids;
		for (int row = 0; row < table->rowCount(); row++) {
			if (!table->selectionModel()->isRowSelected(row, QModelIndex()))
				continue;
			QTableWidgetItem *item = table->item(row, 0);
			const uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
			if (id)
				ids.push_back(id);
		}
		return ids;
	}

	void setStatus(const char *key) { status->setText(T(key)); }

	void setCreatedStatus(uint64_t eventId) { status->setText(T("EventDock.Created").arg(eventId)); }

	bool eventTransitionConfigured() const
	{
		char *raw = sr_config_get_event_transition();
		const bool configured = raw && *raw;
		bfree(raw);
		return configured;
	}

	void refreshSetupStatus()
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
		setupSourceStatus->setStyleSheet(selectedSources
							 ? QStringLiteral("color: #30c85a; font-weight: bold;")
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

	bool openReplaySetup()
	{
		class ToggleSwitch final : public QCheckBox {
		public:
			explicit ToggleSwitch(QWidget *parent = nullptr) : QCheckBox(parent)
			{
				setCursor(Qt::PointingHandCursor);
				setFixedSize(44, 24);
				setFocusPolicy(Qt::StrongFocus);
			}

		protected:
			bool hitButton(const QPoint &position) const override { return rect().contains(position); }

			void paintEvent(QPaintEvent *event) override
			{
				Q_UNUSED(event);
				QPainter painter(this);
				painter.setRenderHint(QPainter::Antialiasing, true);
				const QColor off = isEnabled() ? QColor(92, 92, 92) : QColor(68, 68, 68);
				const QColor on = isEnabled() ? QColor(39, 174, 96) : QColor(76, 110, 88);
				const QRectF track(1.0, 3.0, width() - 2.0, height() - 6.0);
				painter.setPen(Qt::NoPen);
				painter.setBrush(isChecked() ? on : off);
				painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

				const qreal diameter = track.height() - 4.0;
				const qreal x = isChecked() ? track.right() - diameter - 2.0 : track.left() + 2.0;
				painter.setBrush(isEnabled() ? QColor(250, 250, 250) : QColor(170, 170, 170));
				painter.drawEllipse(QRectF(x, track.top() + 2.0, diameter, diameter));

				if (hasFocus()) {
					QPen focusPen(palette().highlight().color());
					focusPen.setWidth(1);
					painter.setPen(focusPen);
					painter.setBrush(Qt::NoBrush);
					painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 5, 5);
				}
			}
		};

		QDialog dialog(this);
		dialog.setWindowTitle(T("EventDock.Setup.Title"));
		dialog.resize(820, 500);
		auto *layout = new QVBoxLayout(&dialog);
		layout->setContentsMargins(8, 8, 8, 8);
		layout->setSpacing(5);

		auto *summary = new QLabel(&dialog);
		summary->setWordWrap(true);
		layout->addWidget(summary);
		auto *topologyBar = new QHBoxLayout();
		auto *ensureAB = new QPushButton(T("EventDock.Setup.CreateAB"), &dialog);
		topologyBar->addWidget(ensureAB);
		topologyBar->addStretch(1);
		layout->addLayout(topologyBar);

		auto *hint = new QLabel(T("EventDock.Setup.CameraHint"), &dialog);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: gray;"));
		layout->addWidget(hint);

		auto *sources = new QTableWidget(&dialog);
		sources->setColumnCount(4);
		sources->setHorizontalHeaderLabels({T("EventDock.Setup.Use"), T("EventDock.Setup.Source"),
						    T("EventDock.Setup.Compatible"), T("EventDock.Setup.Type")});
		sources->setEditTriggers(QAbstractItemView::NoEditTriggers);
		sources->setSelectionBehavior(QAbstractItemView::SelectRows);
		sources->setSelectionMode(QAbstractItemView::NoSelection);
		sources->setAlternatingRowColors(true);
		sources->verticalHeader()->setVisible(false);
		sources->verticalHeader()->setDefaultSectionSize(30);
		sources->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		sources->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		for (int column = 2; column < 4; column++)
			sources->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
		layout->addWidget(sources, 1);

		auto refreshSummary = [&]() {
			sr_replay_setup_snapshot snapshot = {};
			if (!sr_replay_setup_get_snapshot(&snapshot)) {
				summary->setText(T("EventDock.Setup.Unavailable"));
				return;
			}
			const QString sceneA = snapshot.bus_a_ready ? QString::fromUtf8(snapshot.scene_a)
								    : T("EventDock.Setup.Missing");
			const QString sceneB = snapshot.bus_b_ready ? QString::fromUtf8(snapshot.scene_b)
								    : T("EventDock.Setup.Missing");
			const QString programState =
				!snapshot.program_output_supported ? T("EventDock.Setup.ProgramUnsupportedShort")
				: snapshot.program_output_enabled  ? T("EventDock.Setup.ProgramOn")
								   : T("EventDock.Setup.ProgramOff");
			summary->setText(T("EventDock.Setup.Summary")
						 .arg(snapshot.enabled_capture_source_count)
						 .arg(snapshot.compatible_source_count)
						 .arg(sceneA)
						 .arg(sceneB)
						 .arg(snapshot.event_transition_ready ? T("EventDock.Setup.ReadyAB")
										      : T("EventDock.Setup.CutOnly")) +
					 QStringLiteral(" · PROGRAM: ") + programState);
			sr_replay_setup_free_snapshot(&snapshot);
		};

		auto makeSwitchCell = [sources](ToggleSwitch *toggle) {
			auto *container = new QWidget(sources);
			auto *cellLayout = new QHBoxLayout(container);
			cellLayout->setContentsMargins(5, 0, 5, 0);
			cellLayout->setSpacing(0);
			cellLayout->addWidget(toggle);
			cellLayout->addStretch(1);
			return container;
		};

		auto applyToggle = [&, this](ToggleSwitch *toggle, bool program, const QString &sourceName,
					     bool enabled) {
			sr_capture_recording_summary recording = {};
			if (sr_capture_get_recording_summary(&recording) && recording.requested_count) {
				if (QMessageBox::question(&dialog, T("EventDock.Setup.Title"),
							  T("EventDock.Setup.StopRecordingFirst")) !=
				    QMessageBox::Yes) {
					QSignalBlocker blocker(toggle);
					toggle->setChecked(!enabled);
					return;
				}
				setAllRecording(false);
			}

			const QByteArray sourceUtf8 = sourceName.toUtf8();
			const bool ok = program ? sr_replay_setup_set_program_output(enabled)
						: sr_replay_setup_set_capture(sourceUtf8.constData(), enabled);
			if (!ok) {
				QSignalBlocker blocker(toggle);
				toggle->setChecked(!enabled);
				QMessageBox::warning(&dialog, T("EventDock.Setup.Title"),
						     T("EventDock.Setup.ToggleFailed"));
				return;
			}

			refreshSummary();
			refreshSetupStatus();
			refreshRecordingStatus();
			refreshCameras();
			status->setText(T("EventDock.Setup.ToggleApplied")
						.arg(sourceName)
						.arg(enabled ? T("EventDock.Setup.ProgramOn")
							     : T("EventDock.Setup.ProgramOff")));
		};

		auto populateSources = [&]() {
			sr_replay_setup_snapshot snapshot = {};
			if (!sr_replay_setup_get_snapshot(&snapshot)) {
				sources->setRowCount(0);
				return;
			}

			sources->setRowCount((int)snapshot.source_count + 1);

			// PROGRAM is a permanent pseudo-camera row. It is always visible even
			// when the current platform cannot record it.
			auto *programToggle = new ToggleSwitch(sources);
			programToggle->setChecked(snapshot.program_output_enabled);
			programToggle->setEnabled(snapshot.program_output_supported);
			programToggle->setToolTip(snapshot.program_output_supported
							  ? T("EventDock.Setup.ProgramOutput.Tooltip")
							  : T("EventDock.Setup.ProgramOutput.Unsupported"));
			sources->setCellWidget(0, 0, makeSwitchCell(programToggle));
			sources->setItem(0, 1, new QTableWidgetItem(T("EventDock.Setup.ProgramOutputName")));
			sources->setItem(0, 2,
					 new QTableWidgetItem(snapshot.program_output_supported
								      ? T("EventDock.Setup.Yes")
								      : T("EventDock.Setup.No")));
			sources->setItem(0, 3, new QTableWidgetItem(T("EventDock.Setup.ProgramOutputType")));
			connect(programToggle, &QCheckBox::toggled, &dialog, [&, programToggle](bool enabled) {
				applyToggle(programToggle, true, T("EventDock.Setup.ProgramOutputName"), enabled);
			});

			for (size_t i = 0; i < snapshot.source_count; i++) {
				const sr_replay_setup_source &entry = snapshot.sources[i];
				const int row = (int)i + 1;
				auto *toggle = new ToggleSwitch(sources);
				const bool configured = entry.has_capture && entry.capture_enabled;
				toggle->setChecked(configured);
				toggle->setEnabled(entry.compatible || entry.has_capture);
				const QString sourceName = QString::fromUtf8(entry.name);
				toggle->setToolTip(entry.compatible
							   ? T("EventDock.Setup.ToggleCameraTooltip").arg(sourceName)
							   : T("EventDock.Setup.IncompatibleTooltip").arg(sourceName));
				sources->setCellWidget(row, 0, makeSwitchCell(toggle));
				sources->setItem(row, 1, new QTableWidgetItem(sourceName));
				sources->setItem(row, 2,
						 new QTableWidgetItem(entry.compatible ? T("EventDock.Setup.Yes")
										       : T("EventDock.Setup.No")));
				sources->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(entry.type_id)));
				connect(toggle, &QCheckBox::toggled, &dialog, [&, toggle, sourceName](bool enabled) {
					applyToggle(toggle, false, sourceName, enabled);
				});
			}
			sr_replay_setup_free_snapshot(&snapshot);
		};

		refreshSummary();
		populateSources();

		auto *cameraBar = new QHBoxLayout();
		auto *selectAll = new QPushButton(T("EventDock.Setup.SelectAll"), &dialog);
		auto *selectNone = new QPushButton(T("EventDock.Setup.SelectNone"), &dialog);
		cameraBar->addWidget(selectAll);
		cameraBar->addWidget(selectNone);
		cameraBar->addStretch(1);
		layout->addLayout(cameraBar);

		connect(selectAll, &QPushButton::clicked, &dialog, [sources]() {
			for (int row = 0; row < sources->rowCount(); row++) {
				QWidget *cell = sources->cellWidget(row, 0);
				auto *toggle = cell ? cell->findChild<ToggleSwitch *>() : nullptr;
				if (toggle && toggle->isEnabled())
					toggle->setChecked(true);
			}
		});
		connect(selectNone, &QPushButton::clicked, &dialog, [sources]() {
			for (int row = 0; row < sources->rowCount(); row++) {
				QWidget *cell = sources->cellWidget(row, 0);
				auto *toggle = cell ? cell->findChild<ToggleSwitch *>() : nullptr;
				if (toggle && toggle->isEnabled())
					toggle->setChecked(false);
			}
		});
		connect(ensureAB, &QPushButton::clicked, &dialog, [&, this]() {
			sr_replay_setup_result result = {};
			if (!sr_replay_setup_ensure_event_scenes(&result)) {
				QMessageBox::warning(&dialog, T("EventDock.Setup.Title"),
						     T("EventDock.Setup.ABFailed"));
			} else {
				status->setText(T("EventDock.Setup.ABReady")
							.arg(QString::fromUtf8(result.scene_a))
							.arg(QString::fromUtf8(result.scene_b)));
			}
			refreshSummary();
			refreshSetupStatus();
		});

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		layout->addWidget(buttons);
		dialog.exec();

		refreshSetupStatus();
		refreshRecordingStatus();
		sr_replay_setup_snapshot finalSnapshot = {};
		const bool ready =
			sr_replay_setup_get_snapshot(&finalSnapshot) &&
			(finalSnapshot.enabled_capture_source_count > 0 || finalSnapshot.program_output_enabled);
		sr_replay_setup_free_snapshot(&finalSnapshot);
		return ready;
	}

	bool recordingPreflight()
	{
		sr_replay_setup_snapshot snapshot = {};
		if (!sr_replay_setup_get_snapshot(&snapshot))
			return false;
		bool haveCapture = snapshot.enabled_capture_source_count > 0 || snapshot.program_output_enabled;
		bool abReady = snapshot.event_transition_ready;
		sr_replay_setup_free_snapshot(&snapshot);

		if (!haveCapture) {
			QMessageBox prompt(QMessageBox::Warning, T("EventDock.Setup.PreflightTitle"),
					   T("EventDock.Setup.NoCapturePreflight"), QMessageBox::NoButton, this);
			QPushButton *openSetup = prompt.addButton(T("EventDock.Setup.Open"), QMessageBox::AcceptRole);
			prompt.addButton(QMessageBox::Cancel);
			prompt.exec();
			if (prompt.clickedButton() != openSetup || !openReplaySetup())
				return false;
			sr_replay_setup_snapshot configured = {};
			haveCapture =
				sr_replay_setup_get_snapshot(&configured) &&
				(configured.enabled_capture_source_count > 0 || configured.program_output_enabled);
			sr_replay_setup_free_snapshot(&configured);
		}

		if (!haveCapture)
			return false;

		/* Replay A/B is part of the normal recording topology, not only an
		 * Event-Transition feature. Prepare it automatically before REC so a
		 * fresh OBS scene collection is one-click ready for replay. */
		sr_replay_setup_snapshot current = {};
		if (sr_replay_setup_get_snapshot(&current))
			abReady = current.event_transition_ready;
		sr_replay_setup_free_snapshot(&current);

		if (!abReady) {
			sr_replay_setup_result result = {};
			if (sr_replay_setup_ensure_event_scenes(&result)) {
				abReady = result.event_transition_ready;
				refreshSetupStatus();
				status->setText(T("EventDock.Setup.ABReady")
							.arg(QString::fromUtf8(result.scene_a))
							.arg(QString::fromUtf8(result.scene_b)));
			} else {
				QMessageBox prompt(QMessageBox::Warning, T("EventDock.Setup.PreflightTitle"),
						   T("EventDock.Setup.ABFailed"), QMessageBox::NoButton, this);
				QPushButton *openSetup =
					prompt.addButton(T("EventDock.Setup.Open"), QMessageBox::AcceptRole);
				QPushButton *cuts =
					prompt.addButton(T("EventDock.Setup.StartCuts"), QMessageBox::ActionRole);
				prompt.addButton(QMessageBox::Cancel);
				prompt.exec();
				if (prompt.clickedButton() == openSetup) {
					if (!openReplaySetup())
						return false;
					sr_replay_setup_snapshot repaired = {};
					const bool repairedReady = sr_replay_setup_get_snapshot(&repaired) &&
								   repaired.event_transition_ready;
					sr_replay_setup_free_snapshot(&repaired);
					if (!repairedReady)
						return false;
				} else if (prompt.clickedButton() != cuts) {
					return false;
				}
			}
		}
		return haveCapture;
	}

	void updateRecordToggle(const sr_capture_recording_summary *summary)
	{
		if (!recordToggle)
			return;
		const bool requested = summary && summary->requested_count > 0;
		const bool active = summary && summary->active_count > 0;
		recordToggle->setToolTip(T(requested ? "EventDock.RecordStop" : "EventDock.RecordStart"));
		if (active) {
			recordToggle->setStyleSheet(QStringLiteral(
				"QToolButton { color: #ff4040; font-weight: bold; border: 1px solid #7f3030; border-radius: 3px; }"));
		} else if (requested) {
			recordToggle->setStyleSheet(QStringLiteral(
				"QToolButton { color: #d8a000; font-weight: bold; border: 1px solid #806b2a; border-radius: 3px; }"));
		} else {
			recordToggle->setStyleSheet(QStringLiteral(
				"QToolButton { color: #8a8a8a; font-weight: bold; border: 1px solid #5a5a5a; border-radius: 3px; }"));
		}
	}

	void toggleAllRecording()
	{
		sr_capture_recording_summary summary = {};
		const bool requested = sr_capture_get_recording_summary(&summary) && summary.requested_count > 0;
		if (requested) {
			setAllRecording(false);
			return;
		}
		if (recordingPreflight())
			setAllRecording(true);
	}

	void setAllRecording(bool enabled)
	{
		size_t cameras = 0;
		if (!sr_capture_set_all_disk_recording(enabled, &cameras) || !cameras) {
			setStatus("EventDock.RecordNoCameras");
			refreshRecordingStatus();
			return;
		}
		status->setText(T(enabled ? "EventDock.RecordStartRequested" : "EventDock.RecordStopped").arg(cameras));
		refreshRecordingStatus();
	}

	void refreshRecordingStatus()
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
			recordStatus->setText(
				T("EventDock.RecordActive")
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

	void refreshHardwareStatus()
	{
		if (!performanceSummary || !performanceTable || (performancePanel && !performancePanel->isVisible()))
			return;

		sr_capture_performance_snapshot snapshot = {};
		if (!sr_capture_get_performance_snapshot(&snapshot)) {
			performanceSummary->setText(T("EventDock.Performance.Unavailable"));
			performanceTable->setRowCount(0);
			return;
		}

		performanceTable->setUpdatesEnabled(false);
		performanceTable->setRowCount((int)snapshot.count);
		size_t gpuCount = 0;
		size_t cpuCount = 0;
		size_t waitingCount = 0;
		size_t errorCount = 0;
		size_t fallbackCount = 0;
		uint64_t writtenPackets = 0;
		uint64_t droppedPackets = 0;

		for (size_t i = 0; i < snapshot.count; i++) {
			const sr_capture_performance_entry &entry = snapshot.entries[i];
			switch (entry.path) {
			case SR_CAPTURE_PERF_GPU_D3D11:
				gpuCount++;
				break;
			case SR_CAPTURE_PERF_CPU:
				cpuCount++;
				break;
			case SR_CAPTURE_PERF_ERROR:
				errorCount++;
				break;
			case SR_CAPTURE_PERF_WAITING:
			default:
				waitingCount++;
				break;
			}
			if (entry.gpu_fallback_reason != SR_CAPTURE_GPU_FALLBACK_NONE)
				fallbackCount++;
			writtenPackets += entry.packets_written;
			droppedPackets += entry.packets_dropped;

			const int row = (int)i;
			const QString path = capturePerformancePath(entry);
			const QString video = captureVideoMode(entry);
			const QString gop = captureGopMode(entry);
			const QString queue =
				QStringLiteral("%1 / peak %2").arg(entry.queue_depth).arg(entry.queue_high_watermark);
			const QString packets = QString::number(entry.packets_written);
			const QString drops = QString::number(entry.packets_dropped);
			const QString disk = captureDiskState(entry);
			const QString fallback = captureFallbackText(entry.gpu_fallback_reason);
			const double averageSubmitMs = entry.encode_calls ? (double)entry.encode_time_ns_total /
										    (double)entry.encode_calls / 1e6
									  : 0.0;
			const double lastSubmitMs = (double)entry.encode_time_ns_last / 1e6;
			QString tooltip = T("EventDock.Performance.Tooltip")
						  .arg(averageSubmitMs, 0, 'f', 3)
						  .arg(lastSubmitMs, 0, 'f', 3)
						  .arg((double)entry.ram_bytes / (1024.0 * 1024.0), 0, 'f', 1)
						  .arg(entry.segments_finalized)
						  .arg(entry.queue_high_watermark);
			if (!fallback.isEmpty())
				tooltip = fallback + QStringLiteral("\n") + tooltip;

			const QString values[] = {
				QString::fromUtf8(entry.camera_name), path, video, gop, queue, packets, drops, disk};
			for (int column = 0; column < performanceTable->columnCount(); column++) {
				auto *item = new QTableWidgetItem(values[column]);
				item->setToolTip(tooltip);
				performanceTable->setItem(row, column, item);
			}
		}

		performanceTable->setUpdatesEnabled(true);
		performanceSummary->setText(
			T("EventDock.Performance.Summary")
				.arg(gpuCount)
				.arg(cpuCount)
				.arg(waitingCount)
				.arg(errorCount)
				.arg(fallbackCount)
				.arg(writtenPackets)
				.arg(droppedPackets) +
			QStringLiteral("\n") + replayPerformanceSummary(SR_REPLAY_BUS_A, QStringLiteral("A")) +
			QStringLiteral("    ") + replayPerformanceSummary(SR_REPLAY_BUS_B, QStringLiteral("B")));
		sr_capture_free_performance_snapshot(&snapshot);
	}

	bool recordingHasMedia()
	{
		sr_capture_recording_summary summary = {};
		return sr_capture_get_recording_summary(&summary) && summary.active_count && summary.packets_written &&
		       summary.reserve_blocked_count < summary.active_count;
	}

	void rebuildAngleButtons(const QStringList &names)
	{
		if (!angleGrid)
			return;

		while (QLayoutItem *item = angleGrid->takeAt(0)) {
			if (QWidget *widget = item->widget())
				widget->deleteLater();
			delete item;
		}
		angleButtons.clear();

		for (int i = 0; i < names.size(); i++) {
			const QString camera = names.at(i);
			auto *button = new QToolButton(this);
			button->setText(camera);
			button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			button->setCheckable(true);
			button->setAutoRaise(false);
			button->setMinimumWidth(184);
			button->setFixedHeight(132);
			button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			button->setIconSize(QSize(ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT));
			button->setProperty("cameraName", camera);
			button->setProperty("coverage", (int)SR_REPLAY_COVERAGE_NONE);
			button->setProperty("playableInNs", QVariant::fromValue<qulonglong>(0));
			button->setProperty("playableOutNs", QVariant::fromValue<qulonglong>(0));
			connect(button, &QToolButton::clicked, this, [this, camera]() { selectAngle(camera); });
			angleGrid->addWidget(button, i / 4, i % 4);
			angleButtons.append(button);
		}
	}

	void refreshCameras()
	{
		if (!cameraCombo)
			return;
		const QString previous = selectedCamera();
		const QStringList names = captureCameraNames();
		QStringList current;
		for (int i = 0; i < cameraCombo->count(); i++) {
			const QString value = cameraCombo->itemData(i).toString();
			if (!value.isEmpty())
				current.append(value);
		}

		if (current != names) {
			cameraCombo->clear();
			if (names.isEmpty()) {
				cameraCombo->addItem(T("EventDock.NoCamera"), QString());
			} else {
				for (const QString &name : names)
					cameraCombo->addItem(name, name);
				const int previousIndex = cameraCombo->findData(previous);
				if (previousIndex >= 0)
					cameraCombo->setCurrentIndex(previousIndex);
			}
			rebuildAngleButtons(names);
		}

		refreshAngleCoverage();
	}

	uint64_t angleEventId() const
	{
		const uint64_t selected = selectedEventId();
		if (selected)
			return selected;

		sr_replay_channel_state state = {};
		return sr_replay_channel_get_state(transportBus(), &state) && state.cued ? state.event_id : 0;
	}

	void refreshAngleCoverage()
	{
		const uint64_t eventId = angleEventId();
		sr_event_record event = {};
		const bool haveEvent = controller && eventId &&
				       sr_event_controller_get_event(controller, eventId, &event);
		if (!haveEvent) {
			previewTargetEventId = 0;
			previewLoadedEventId = 0;
			for (QToolButton *button : angleButtons)
				button->setIcon(QIcon());
		}

		QString preferredCamera;
		if (haveEvent && event.preferred_camera_id) {
			char *preferredName = nullptr;
			if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id,
								&preferredName) &&
			    preferredName)
				preferredCamera = QString::fromUtf8(preferredName);
			bfree(preferredName);
		}

		for (QToolButton *button : angleButtons) {
			const QString camera = button->property("cameraName").toString();
			sr_replay_coverage_info coverage = {};
			if (haveEvent) {
				const QByteArray cameraUtf8 = camera.toUtf8();
				if (!sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns,
							      &coverage))
					coverage.coverage = SR_REPLAY_COVERAGE_NONE;
			}

			button->setProperty("coverage", (int)coverage.coverage);
			button->setProperty("playableInNs", QVariant::fromValue<qulonglong>(coverage.playable_in_ns));
			button->setProperty("playableOutNs", QVariant::fromValue<qulonglong>(coverage.playable_out_ns));
			button->setProperty("syncOffsetNs", QVariant::fromValue<qlonglong>(coverage.sync_offset_ns));

			QString marker = QStringLiteral("○");
			QString tooltip = haveEvent ? T("EventDock.AngleNone").arg(camera)
						    : T("EventDock.NoEventSelected");
			if (coverage.coverage == SR_REPLAY_COVERAGE_FULL) {
				marker = QStringLiteral("●");
				tooltip = T("EventDock.AngleFull").arg(camera);
			} else if (coverage.coverage == SR_REPLAY_COVERAGE_PARTIAL) {
				marker = QStringLiteral("◐");
				const double eventSeconds =
					event.out_ns >= event.in_ns ? (double)(event.out_ns - event.in_ns) / 1e9 : 0.0;
				const double playableSeconds =
					coverage.playable_out_ns >= coverage.playable_in_ns
						? (double)(coverage.playable_out_ns - coverage.playable_in_ns) / 1e9
						: 0.0;
				tooltip = T("EventDock.AnglePartial")
						  .arg(camera)
						  .arg(playableSeconds, 0, 'f', 2)
						  .arg(eventSeconds, 0, 'f', 2);
			}
			const QString preferredMarker = camera == preferredCamera ? QStringLiteral("★ ") : QString();
			button->setText(QStringLiteral("%1%2 %3").arg(preferredMarker, marker, camera));
			if (camera == preferredCamera)
				tooltip += QStringLiteral(" — ") + T("EventDock.Preferred");
			button->setProperty("coverageTooltip", tooltip);
			button->setToolTip(tooltip);
		}

		if (haveEvent)
			requestAnglePreviews(event.id, event.in_ns + (event.out_ns - event.in_ns) / 2);

		if (haveEvent)
			sr_event_controller_free_event(&event);
		syncAngleButtonState();
	}

	void requestAnglePreviews(uint64_t eventId, uint64_t timestampNs)
	{
		if (previewTargetEventId != eventId) {
			previewTargetEventId = eventId;
			previewLoadedEventId = 0;
		}
		if (!eventId)
			return;

		auto cacheIt = anglePreviewCache.find(eventId);
		bool allCached = true;
		for (QToolButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE)
				continue;
			const std::string camera = button->property("cameraName").toString().toUtf8().constData();
			if (cacheIt != anglePreviewCache.end()) {
				auto iconIt = cacheIt->second.find(camera);
				if (iconIt != cacheIt->second.end() && !iconIt->second.isNull()) {
					button->setIcon(iconIt->second);
					continue;
				}
			}
			allCached = false;
		}
		if (allCached) {
			previewLoadedEventId = eventId;
			return;
		}
		if (previewLoadedEventId == eventId || anglePreviewJob)
			return;

		char *sessionPath = sr_session_get_or_create_path();
		if (!sessionPath)
			return;
		auto job = std::make_unique<AnglePreviewJob>();
		job->eventId = eventId;
		job->sessionDir = sessionPath;
		bfree(sessionPath);
		for (QToolButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE)
				continue;
			const std::string camera = button->property("cameraName").toString().toUtf8().constData();
			cacheIt = anglePreviewCache.find(eventId);
			if (cacheIt != anglePreviewCache.end()) {
				auto iconIt = cacheIt->second.find(camera);
				if (iconIt != cacheIt->second.end() && !iconIt->second.isNull())
					continue;
			}
			const qint64 offset = button->property("syncOffsetNs").toLongLong();
			uint64_t cameraTimestamp = timestampNs;
			if (offset >= 0 && (uint64_t)offset <= UINT64_MAX - cameraTimestamp)
				cameraTimestamp += (uint64_t)offset;
			else if (offset < 0 && (uint64_t)(-offset) < cameraTimestamp)
				cameraTimestamp -= (uint64_t)(-offset);
			AnglePreviewTask task;
			task.camera = camera;
			task.timestampNs = cameraTimestamp;
			job->tasks.emplace_back(std::move(task));
		}
		if (job->tasks.empty()) {
			previewLoadedEventId = eventId;
			return;
		}
		anglePreviewJob = std::move(job);
		AnglePreviewJob *workerJob = anglePreviewJob.get();
		workerJob->worker = std::thread([workerJob]() { runAnglePreviewJob(workerJob); });
	}

	void pollAnglePreviews()
	{
		if (!anglePreviewJob || !anglePreviewJob->done.load(std::memory_order_acquire))
			return;
		if (anglePreviewJob->worker.joinable())
			anglePreviewJob->worker.join();

		const uint64_t completedEventId = anglePreviewJob->eventId;
		sr_event_record completedEvent = {};
		const bool completedEventExists =
			controller && completedEventId &&
			sr_event_controller_get_event(controller, completedEventId, &completedEvent);
		if (completedEventExists) {
			sr_event_controller_free_event(&completedEvent);
			auto &cache = anglePreviewCache[completedEventId];
			for (const AnglePreviewResult &result : anglePreviewJob->results) {
				if (result.rgba.empty())
					continue;
				const QImage image(result.rgba.data(), ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT,
						   ANGLE_PREVIEW_WIDTH * 4, QImage::Format_RGBA8888);
				QIcon icon(QPixmap::fromImage(image.copy()));
				cache[result.camera] = icon;
				if (completedEventId == previewTargetEventId &&
				    angleEventId() == previewTargetEventId) {
					if (QToolButton *button = angleButton(QString::fromUtf8(result.camera.c_str())))
						button->setIcon(icon);
				}
			}
		}
		while (anglePreviewCache.size() > ANGLE_PREVIEW_CACHE_EVENTS)
			anglePreviewCache.erase(anglePreviewCache.begin());

		if (completedEventId == previewTargetEventId && angleEventId() == previewTargetEventId)
			previewLoadedEventId = completedEventId;
		anglePreviewJob.reset();

		if (previewTargetEventId && previewLoadedEventId != previewTargetEventId)
			refreshAngleCoverage();
	}

	void syncAngleButtonState()
	{
		const uint64_t eventId = angleEventId();
		sr_replay_channel_state state = {};
		const bool haveState = sr_replay_channel_get_state(transportBus(), &state);
		const bool sameEvent = haveState && state.cued && eventId && state.event_id == eventId;
		const QString activeCamera = sameEvent ? QString::fromUtf8(state.camera_name) : QString();

		for (QToolButton *button : angleButtons) {
			const auto coverage = static_cast<sr_replay_coverage>(button->property("coverage").toInt());
			const uint64_t playableIn = button->property("playableInNs").toULongLong();
			const uint64_t playableOut = button->property("playableOutNs").toULongLong();
			const QString camera = button->property("cameraName").toString();
			const bool atPlayhead = !sameEvent ||
						(state.playhead_ns >= playableIn && state.playhead_ns <= playableOut);
			button->setEnabled(eventId && coverage != SR_REPLAY_COVERAGE_NONE && atPlayhead);
			button->setChecked(sameEvent && activeCamera == camera);
			button->setToolTip(button->property("coverageTooltip").toString());
			if (sameEvent && coverage != SR_REPLAY_COVERAGE_NONE && !atPlayhead)
				button->setToolTip(T("EventDock.AngleUnavailable").arg(camera));
		}
	}

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

	void setPreferredCamera(bool clear)
	{
		const uint64_t eventId = selectedEventId();
		if (!controller || !eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}

		const QString camera = selectedCamera();
		if (!clear && camera.isEmpty()) {
			setStatus("EventDock.NoCameraSelected");
			return;
		}
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *name = clear ? nullptr : cameraUtf8.constData();
		if (!sr_event_controller_set_preferred_camera(controller, eventId, name)) {
			setStatus("EventDock.PreferredFailed");
			return;
		}

		setStatus(clear ? "EventDock.PreferredCleared" : "EventDock.PreferredSet");
		refreshAngleCoverage();
	}

	QString playlistSummary(enum sr_replay_bus bus) const
	{
		sr_replay_playlist_state state = {};
		if (!sr_replay_playlist_get_state(bus, &state) || !state.active)
			return QString();
		if (state.angle_sequence)
			return T("EventDock.AngleSequenceState")
				.arg(state.event_id)
				.arg(state.position + 1)
				.arg(state.count);
		return T("EventDock.PlaylistState").arg(state.list_id).arg(state.position + 1).arg(state.count);
	}

	enum sr_replay_bus activePlaylistBus() const
	{
		enum sr_replay_bus program;
		sr_replay_playlist_state state = {};
		if (sr_replay_take_program_bus(&program) && sr_replay_playlist_get_state(program, &state) &&
		    state.active)
			return program;
		if (sr_replay_playlist_get_state(SR_REPLAY_BUS_A, &state) && state.active)
			return SR_REPLAY_BUS_A;
		if (sr_replay_playlist_get_state(SR_REPLAY_BUS_B, &state) && state.active)
			return SR_REPLAY_BUS_B;
		return transportBus();
	}

	bool eventTransitionCrossBus(bool *requested = nullptr) const
	{
		char *configured = sr_config_get_event_transition();
		const bool wanted = configured && *configured;
		bfree(configured);
		if (requested)
			*requested = wanted;
		return wanted && sr_replay_take_event_transition_ready();
	}

	void refreshTransportStatus()
	{
		if (!transportStatus)
			return;
		QString a = channelSummary(SR_REPLAY_BUS_A, QStringLiteral("A"));
		QString b = channelSummary(SR_REPLAY_BUS_B, QStringLiteral("B"));
		const QString pa = playlistSummary(SR_REPLAY_BUS_A);
		const QString pb = playlistSummary(SR_REPLAY_BUS_B);
		if (!pa.isEmpty())
			a += QStringLiteral("  [") + pa + QStringLiteral("]");
		if (!pb.isEmpty())
			b += QStringLiteral("  [") + pb + QStringLiteral("]");
		transportStatus->setText(a + QStringLiteral("    ") + b);
	}

	QString cueStatus(enum sr_replay_bus bus, const QString &label) const
	{
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(bus, &state) || !state.cued)
			return T("EventDock.CueStateEmpty").arg(label);
		return T("EventDock.CueStateEvent")
			.arg(label)
			.arg(state.event_id)
			.arg(QString::fromUtf8(state.camera_name));
	}

	void refreshProgramState()
	{
		if (!programStatus || !cueAStatus || !cueBStatus)
			return;
		enum sr_replay_bus programBus;
		if (sr_replay_take_program_bus(&programBus)) {
			programStatus->setText(T("EventDock.ProgramReplay")
						       .arg(programBus == SR_REPLAY_BUS_A ? QStringLiteral("A")
											  : QStringLiteral("B")));
			programStatus->setStyleSheet(QStringLiteral(
				"font-weight: bold; color: white; background: #b52b2b; border-radius: 3px; padding: 2px;"));
		} else {
			programStatus->setText(T("EventDock.ProgramLive"));
			programStatus->setStyleSheet(QStringLiteral(
				"font-weight: bold; color: white; background: #247a3b; border-radius: 3px; padding: 2px;"));
		}
		cueAStatus->setText(cueStatus(SR_REPLAY_BUS_A, QStringLiteral("A")));
		cueBStatus->setText(cueStatus(SR_REPLAY_BUS_B, QStringLiteral("B")));
		cueAStatus->setStyleSheet(QStringLiteral(
			"font-weight: bold; color: palette(text); background: palette(alternate-base); border: 1px solid #3f78c5; border-radius: 3px; padding: 2px;"));
		cueBStatus->setStyleSheet(QStringLiteral(
			"font-weight: bold; color: palette(text); background: palette(alternate-base); border: 1px solid #d49a2a; border-radius: 3px; padding: 2px;"));
	}

	void setOperatorSpeed(double speed)
	{
		if (!sr_replay_channel_set_controller_speed(speed))
			return;
		if (sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_EVENT)
			sr_replay_channel_set_speed(transportBus(), speed);
		syncTimeline();
	}

	void syncTransportControls()
	{
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state))
			return;
		reverseButton->setChecked(state.backward);
		loopButton->setChecked(state.loop);
		const double displayedSpeed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL
						      ? sr_replay_channel_get_controller_speed()
						      : state.speed_percent;
		const int speedIndex = speedCombo->findData((int)displayedSpeed);
		if (speedIndex >= 0) {
			const QSignalBlocker blocker(speedCombo);
			speedCombo->setCurrentIndex(speedIndex);
		}
		const int audioIndex = audioCombo->findData((int)state.audio_mode);
		if (audioIndex >= 0)
			audioCombo->setCurrentIndex(audioIndex);

		int shuttlePosition = 0;
		if (state.cued && state.playing && !state.paused) {
			const int speed = (int)state.speed_percent;
			if (speed == 25)
				shuttlePosition = 1;
			else if (speed == 50)
				shuttlePosition = 2;
			else if (speed == 100)
				shuttlePosition = 3;
			else if (speed == 200)
				shuttlePosition = 4;
			else if (speed == 400)
				shuttlePosition = 5;
			if (state.backward)
				shuttlePosition = -shuttlePosition;
		}
		if (shuttleSlider) {
			const QSignalBlocker blocker(shuttleSlider);
			shuttleSlider->setValue(shuttlePosition);
		}
		if (shuttleValue) {
			const int speed = shuttleSpeed(shuttlePosition);
			shuttleValue->setText(shuttlePosition
						      ? QStringLiteral("%1%").arg(shuttlePosition < 0 ? -speed : speed)
						      : QStringLiteral("0"));
		}
		syncTimeline();
		refreshTransportStatus();
	}

	uint64_t playbackRuntimeNs(uint64_t mediaNs, double speedPercent) const
	{
		const double speed = speedPercent > 0.0 ? speedPercent : 100.0;
		const long double runtime = (long double)mediaNs * 100.0L / (long double)speed;
		return runtime >= (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)runtime;
	}

	bool playlistTimelineProgress(uint64_t *elapsedNs, uint64_t *totalNs, uint64_t *remainingNs) const
	{
		if (!elapsedNs || !totalNs || !remainingNs || !controller)
			return false;
		*elapsedNs = 0;
		*totalNs = 0;
		*remainingNs = 0;

		const enum sr_replay_bus bus = activePlaylistBus();
		sr_replay_playlist_state playlist = {};
		if (!sr_replay_playlist_get_state(bus, &playlist) || !playlist.active)
			return false;

		uint64_t *eventIds = nullptr;
		size_t count = 0;
		size_t position = 0;
		bool angleSequence = false;
		if (!sr_replay_playlist_snapshot_items(bus, &eventIds, &count, &position, &angleSequence) ||
		    !eventIds || !count) {
			bfree(eventIds);
			return false;
		}

		sr_replay_channel_state current = {};
		const bool haveCurrent = sr_replay_channel_get_state(bus, &current) && current.cued;
		uint64_t total = 0;
		uint64_t elapsed = 0;
		for (size_t i = 0; i < count; i++) {
			uint64_t runtime = 0;
			sr_event_record event = {};
			if (sr_event_controller_get_event(controller, eventIds[i], &event)) {
				if (!event.pending && event.out_ns > event.in_ns) {
					const double plannedSpeed = sr_config_get_replay_speed_policy() ==
											    SR_REPLAY_SPEED_GLOBAL &&
										    !event.speed_override
									    ? sr_replay_channel_get_controller_speed()
									    : event.speed_percent;
					runtime = playbackRuntimeNs(event.out_ns - event.in_ns, plannedSpeed);
				}
				sr_event_controller_free_event(&event);
			}

			uint64_t currentElapsed = 0;
			if (i == position && haveCurrent && current.event_id == eventIds[i] &&
			    current.out_ns > current.in_ns) {
				runtime = playbackRuntimeNs(current.out_ns - current.in_ns, current.speed_percent);
				const uint64_t mediaPosition = current.playhead_ns <= current.in_ns ? 0
							       : current.playhead_ns >= current.out_ns
								       ? current.out_ns - current.in_ns
								       : current.playhead_ns - current.in_ns;
				currentElapsed = playbackRuntimeNs(mediaPosition, current.speed_percent);
				if (currentElapsed > runtime)
					currentElapsed = runtime;
			}

			if (UINT64_MAX - total < runtime)
				total = UINT64_MAX;
			else
				total += runtime;
			if (i < position) {
				if (UINT64_MAX - elapsed < runtime)
					elapsed = UINT64_MAX;
				else
					elapsed += runtime;
			} else if (i == position) {
				if (UINT64_MAX - elapsed < currentElapsed)
					elapsed = UINT64_MAX;
				else
					elapsed += currentElapsed;
			}
		}
		bfree(eventIds);
		if (!total)
			return false;
		if (elapsed > total)
			elapsed = total;
		*elapsedNs = elapsed;
		*totalNs = total;
		*remainingNs = total - elapsed;
		return true;
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
			timelineSlider->setSequenceProgress(true);
			timelineSlider->setEnabled(true);
			timelineSlider->setToolTip(T("EventDock.Timeline.PlaylistTooltip"));
			const int value = (int)((long double)sequenceElapsed * 10000.0L / (long double)sequenceTotal);
			timelineSlider->setValue(qBound(0, value, 10000));
			if (sequenceRemaining > 15ULL * NS_PER_SECOND)
				timelineSlider->setProgressTint(QColor(QStringLiteral("#2fb34a")));
			else if (sequenceRemaining > 10ULL * NS_PER_SECOND)
				timelineSlider->setProgressTint(QColor(QStringLiteral("#d2a216")));
			else
				timelineSlider->setProgressTint(QColor(QStringLiteral("#d33b3b")));
			timelineTime->setText(replayClockText(sequenceElapsed) + QStringLiteral(" / ") +
					      replayClockText(sequenceTotal) + QStringLiteral("   −") +
					      replayClockText(sequenceRemaining));
			return;
		}

		timelineSlider->setSequenceProgress(false);
		timelineSlider->clearProgressTint();
		timelineSlider->setToolTip(T("EventDock.Timeline.Tooltip"));
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued ||
		    state.out_ns <= state.in_ns) {
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
			const int sliderValue = (int)((long double)position * 10000.0L / (long double)duration);
			timelineSlider->setValue(sliderValue);
		}
		timelineTime->setText(replayClockText(position) + QStringLiteral(" / ") + replayClockText(duration));
	}

	void seekTimeline(int value)
	{
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued || state.out_ns <= state.in_ns)
			return;

		if (value < 0)
			value = 0;
		if (value > 10000)
			value = 10000;
		const uint64_t duration = state.out_ns - state.in_ns;
		const uint64_t offset = (uint64_t)((long double)duration * (long double)value / 10000.0L);
		const uint64_t target = offset >= duration ? state.out_ns : state.in_ns + offset;
		sr_replay_channel_pause(transportBus(), true);
		sr_replay_channel_seek(transportBus(), target);
		timelineTime->setText(replayClockText(offset > duration ? duration : offset) + QStringLiteral(" / ") +
				      replayClockText(duration));
	}

	void createRangeEvent(int rangeIn, int rangeOut)
	{
		if (!controller || rangeOut <= rangeIn)
			return;

		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued ||
		    state.out_ns <= state.in_ns) {
			setStatus("EventDock.NoCue");
			return;
		}

		rangeIn = qBound(0, rangeIn, 10000);
		rangeOut = qBound(0, rangeOut, 10000);
		const uint64_t duration = state.out_ns - state.in_ns;
		const uint64_t inOffset = (uint64_t)((long double)duration * (long double)rangeIn / 10000.0L);
		const uint64_t outOffset = (uint64_t)((long double)duration * (long double)rangeOut / 10000.0L);
		const uint64_t inNs = state.in_ns + inOffset;
		const uint64_t outNs = rangeOut >= 10000 ? state.out_ns : state.in_ns + outOffset;
		if (outNs <= inNs || !sr_event_controller_mark_in(controller, inNs)) {
			setStatus("EventDock.Failed");
			return;
		}

		uint64_t eventId = 0;
		if (!sr_event_controller_mark_out(controller, outNs, &eventId)) {
			sr_event_controller_cancel_mark_in(controller);
			setStatus("EventDock.Failed");
			return;
		}
		setCreatedStatus(eventId);
		refresh(eventId);
	}

	void jogMoved(int value)
	{
		const int delta = value - jogLastValue;
		jogLastValue = value;
		if (!delta)
			return;

		const int direction = delta > 0 ? 1 : -1;
		if (!sr_replay_channel_step_frames(transportBus(), direction))
			setStatus("EventDock.FrameStepFailed");
		refreshTransportStatus();
		syncTimeline();
	}

	static int shuttleSpeed(int position)
	{
		switch (position < 0 ? -position : position) {
		case 1:
			return 25;
		case 2:
			return 50;
		case 3:
			return 100;
		case 4:
			return 200;
		case 5:
			return 400;
		default:
			return 0;
		}
	}

	void applyShuttle(int position)
	{
		if (!shuttleSlider || !shuttleValue)
			return;

		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued) {
			if (position != 0) {
				const QSignalBlocker blocker(shuttleSlider);
				shuttleSlider->setValue(0);
			}
			shuttleValue->setText(QStringLiteral("0"));
			setStatus("EventDock.NoCue");
			return;
		}

		if (!position) {
			sr_replay_channel_pause(transportBus(), true);
			shuttleValue->setText(QStringLiteral("0"));
			refreshTransportStatus();
			return;
		}

		const int speed = shuttleSpeed(position);
		if (!speed)
			return;
		sr_replay_channel_set_backward(transportBus(), position < 0);
		setOperatorSpeed(speed);
		if (state.paused)
			sr_replay_channel_pause(transportBus(), false);
		else if (!state.playing)
			sr_replay_channel_play(transportBus());
		shuttleValue->setText(QStringLiteral("%1%").arg(position < 0 ? -speed : speed));
		refreshTransportStatus();
	}

	bool cueSelected(enum sr_replay_bus bus)
	{
		const uint64_t eventId = selectedEventId();
		const QString camera = selectedCamera();
		if (!eventId) {
			setStatus("EventDock.NoEventSelected");
			return false;
		}
		if (camera.isEmpty()) {
			setStatus("EventDock.NoCameraSelected");
			return false;
		}
		QToolButton *angle = angleButton(camera);
		if (angle && angle->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE) {
			status->setText(T("EventDock.CueNoCoverage").arg(camera));
			return false;
		}
		const QByteArray cameraUtf8 = camera.toUtf8();
		sr_replay_playlist_stop(bus);
		if (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData())) {
			setStatus("EventDock.CueFailed");
			return false;
		}
		status->setText(T("EventDock.Cued")
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
					.arg(eventId));
		if (transportBus() == bus)
			syncTransportControls();
		refreshTransportStatus();
		return true;
	}

	void playSelectedEvent()
	{
		const std::vector<uint64_t> eventIds = selectedEventIds();
		if (eventIds.empty()) {
			setStatus("EventDock.NoEventSelected");
			return;
		}

		const enum sr_replay_bus bus = transportBus();
		if (eventIds.size() == 1) {
			if (!cueSelected(bus))
				return;
			takeBus(bus);
			return;
		}

		const QString camera = selectedCamera();
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
		bool transitionRequested = false;
		const bool crossBus = eventTransitionCrossBus(&transitionRequested);
		if (!controller || !sr_replay_playlist_start_events_with_transitions(
					   bus, currentList(), eventIds.data(), eventIds.size(), preferred, crossBus)) {
			setStatus("EventDock.PlaylistFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}

		QString message = T("EventDock.PlaySelected") +
				  QStringLiteral(": %1 Events · %2")
					  .arg(eventIds.size())
					  .arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"));
		if (transitionRequested && !crossBus)
			message += QStringLiteral(" · ") + T("EventDock.EventTransitionFallback");
		status->setText(message);
		refreshTransportStatus();
	}

	void playEachAngle()
	{
		const uint64_t eventId = selectedEventId();
		if (!controller || !eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}
		bool transitionRequested = false;
		const bool crossBus = eventTransitionCrossBus(&transitionRequested);
		const enum sr_replay_bus bus = transportBus();
		if (!sr_replay_playlist_start_event_angles(bus, eventId, crossBus)) {
			setStatus("EventDock.AngleSequenceFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}
		sr_replay_playlist_state sequence = {};
		sr_replay_playlist_get_state(bus, &sequence);
		QString message = T("EventDock.AngleSequenceStarted").arg(eventId).arg(sequence.count);
		if (transitionRequested && !crossBus)
			message += QStringLiteral(" · ") + T("EventDock.EventTransitionFallback");
		status->setText(message);
		refreshTransportStatus();
	}

	bool selectEventRowById(uint64_t eventId)
	{
		if (!table || !eventId)
			return false;
		for (int row = 0; row < table->rowCount(); row++) {
			QTableWidgetItem *item = table->item(row, 0);
			if (item && item->data(Qt::UserRole).toULongLong() == eventId) {
				table->setCurrentCell(row, 0);
				table->selectRow(row);
				return true;
			}
		}
		return false;
	}

	void playLastEvent()
	{
		if (!table || table->rowCount() <= 0) {
			setStatus("EventDock.NoEventSelected");
			return;
		}
		table->setCurrentCell(table->rowCount() - 1, 0);
		table->selectRow(table->rowCount() - 1);
		playSelectedEvent();
	}

	void playById()
	{
		bool accepted = false;
		const QString text = QInputDialog::getText(this, T("EventDock.PlayByIdTitle"),
							   T("EventDock.PlayByIdPrompt"), QLineEdit::Normal, QString(),
							   &accepted);
		if (!accepted)
			return;
		bool valid = false;
		const uint64_t eventId = text.trimmed().toULongLong(&valid);
		if (!valid || !eventId || !selectEventRowById(eventId)) {
			status->setText(T("EventDock.PlayByIdNotFound").arg(text.trimmed()).arg(currentList()));
			return;
		}
		playSelectedEvent();
	}

	void startPlaylist(enum sr_replay_bus bus)
	{
		const QString camera = selectedCamera();
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
		bool transitionRequested = false;
		const bool crossBus = eventTransitionCrossBus(&transitionRequested);
		if (!controller ||
		    !sr_replay_playlist_start_with_transitions(bus, currentList(), preferred, crossBus)) {
			setStatus("EventDock.PlaylistFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}
		QString message = T("EventDock.PlaylistStarted")
					  .arg(currentList())
					  .arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"));
		if (transitionRequested && !crossBus)
			message += QStringLiteral(" · ") + T("EventDock.EventTransitionFallback");
		status->setText(message);
		refresh();
		refreshTransportStatus();
	}

	void nextPlaylist()
	{
		if (!sr_replay_playlist_next(activePlaylistBus())) {
			setStatus("EventDock.PlaylistFinished");
			refreshTransportStatus();
			return;
		}
		setStatus("EventDock.PlaylistAdvanced");
		refresh();
		refreshTransportStatus();
	}

	void stopPlaylist()
	{
		sr_replay_playlist_stop(activePlaylistBus());
		setStatus("EventDock.PlaylistStopped");
		refreshTransportStatus();
	}

	void takeBus(enum sr_replay_bus bus)
	{
		if (!controller || !sr_replay_take_bus(controller, bus)) {
			setStatus("EventDock.TakeFailed");
			return;
		}
		status->setText(
			T("EventDock.Taken").arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B")));
		refresh();
		refreshTransportStatus();
	}

	void takeToggleBus()
	{
		if (!controller || !sr_replay_take_toggle(controller)) {
			setStatus("EventDock.TakeFailed");
			return;
		}
		setStatus("EventDock.ToggleTaken");
		refresh();
		refreshTransportStatus();
	}

	void returnLiveBus()
	{
		if (!controller || !sr_replay_take_return(controller)) {
			setStatus("EventDock.ReturnFailed");
			return;
		}
		setStatus("EventDock.Returned");
		refreshTransportStatus();
	}

	void togglePlayPause()
	{
		const enum sr_replay_bus bus = transportBus();
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(bus, &state) || !state.cued) {
			setStatus("EventDock.NoCue");
			return;
		}
		const bool ok = state.playing && !state.paused ? sr_replay_channel_pause(bus, true)
				: state.paused                 ? sr_replay_channel_pause(bus, false)
							       : sr_replay_channel_play(bus);
		if (!ok)
			setStatus("EventDock.TransportFailed");
		refreshTransportStatus();
	}

	void stopTransport()
	{
		sr_replay_playlist_stop(transportBus());
		sr_replay_channel_stop(transportBus());
		refreshTransportStatus();
	}

	void restartTransport()
	{
		sr_replay_channel_restart(transportBus());
		refreshTransportStatus();
	}

	void stepFrame(int direction)
	{
		if (!sr_replay_channel_step_frames(transportBus(), direction)) {
			setStatus("EventDock.FrameStepFailed");
			return;
		}
		refreshTransportStatus();
	}

	QToolButton *angleButton(const QString &camera) const
	{
		for (QToolButton *button : angleButtons) {
			if (button->property("cameraName").toString() == camera)
				return button;
		}
		return nullptr;
	}

	bool addExportTask(std::vector<ExportTask> &tasks, const std::string &sessionDir, const QString &camera,
			   const QString &outputPath, const sr_event_record &event)
	{
		QToolButton *button = angleButton(camera);
		if (!button || button->property("coverage").toInt() != SR_REPLAY_COVERAGE_FULL)
			return false;

		ExportTask task;
		task.sessionDir = sessionDir;
		task.camera = camera.toUtf8().constData();
		task.outputPath = outputPath.toUtf8().constData();
		task.eventInNs = event.in_ns;
		task.eventOutNs = event.out_ns;
		task.syncOffsetNs = button->property("syncOffsetNs").toLongLong();
		task.includeMasterAudio = event.audio_mode == SR_EVENT_AUDIO_MASTER ||
					  sr_camera_is_program_name(task.camera.c_str());
		tasks.emplace_back(std::move(task));
		return true;
	}

	void startExport()
	{
		if (exportJob) {
			setStatus("EventDock.ExportBusy");
			return;
		}

		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (!controller || !eventId || !sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.NoEventSelected");
			return;
		}
		if (event.pending || event.out_ns <= event.in_ns) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ExportPending");
			return;
		}

		QString preferred;
		if (event.preferred_camera_id) {
			char *name = nullptr;
			if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &name) && name)
				preferred = QString::fromUtf8(name);
			bfree(name);
		}

		QStringList fullAngles;
		for (QToolButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_FULL)
				fullAngles.append(button->property("cameraName").toString());
		}
		if (fullAngles.isEmpty()) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ExportNoCoverage");
			return;
		}

		char *sessionPath = sr_session_get_or_create_path();
		if (!sessionPath) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ExportFailed");
			return;
		}
		const std::string sessionDir(sessionPath);
		bfree(sessionPath);

		std::vector<ExportTask> tasks;
		const bool allAngles = exportModeCombo && exportModeCombo->currentData().toInt() == 1;
		if (allAngles) {
			const QString directoryPath =
				QFileDialog::getExistingDirectory(this, T("EventDock.ExportFolder"));
			if (directoryPath.isEmpty()) {
				sr_event_controller_free_event(&event);
				return;
			}
			const QDir directory(directoryPath);
			for (const QString &camera : fullAngles)
				addExportTask(tasks, sessionDir, camera, unusedAnglePath(directory, eventId, camera),
					      event);
		} else {
			QString camera = preferred;
			if (!fullAngles.contains(camera))
				camera = selectedCamera();
			if (!fullAngles.contains(camera))
				camera = fullAngles.first();

			QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
			if (baseDirectory.isEmpty())
				baseDirectory = QDir::homePath();
			const QString suggested = QDir(baseDirectory)
							  .filePath(QStringLiteral("Event_%1_%2.mp4")
									    .arg(eventId, 6, 10, QChar('0'))
									    .arg(safeFilePart(camera)));
			QString outputPath = QFileDialog::getSaveFileName(this, T("EventDock.ExportFile"), suggested,
									  T("EventDock.ExportFilter"));
			if (outputPath.isEmpty()) {
				sr_event_controller_free_event(&event);
				return;
			}
			if (!outputPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive))
				outputPath += QStringLiteral(".mp4");
			if (QFileInfo::exists(outputPath)) {
				sr_event_controller_free_event(&event);
				setStatus("EventDock.ExportExists");
				return;
			}
			addExportTask(tasks, sessionDir, camera, outputPath, event);
		}
		sr_event_controller_free_event(&event);

		if (tasks.empty()) {
			setStatus("EventDock.ExportNoCoverage");
			return;
		}

		exportJob = std::make_unique<ExportJob>();
		exportJob->tasks = std::move(tasks);
		exportProgressBar->setValue(0);
		exportCancelButton->setEnabled(true);
		exportModeCombo->setEnabled(false);
		status->setText(T("EventDock.ExportStarted").arg(exportJob->tasks.size()));
		ExportJob *job = exportJob.get();
		job->worker = std::thread([job]() { runExportJob(job); });
	}

	void cancelExport()
	{
		if (!exportJob)
			return;
		exportJob->cancel.store(true, std::memory_order_relaxed);
		exportCancelButton->setEnabled(false);
		setStatus("EventDock.ExportCancelling");
	}

	void pollExport()
	{
		if (!exportJob)
			return;
		exportProgressBar->setValue((int)exportJob->progress.load(std::memory_order_relaxed));
		if (!exportJob->done.load(std::memory_order_acquire))
			return;

		if (exportJob->worker.joinable())
			exportJob->worker.join();
		exportProgressBar->setValue(exportJob->success ? 100 : (int)exportJob->progress.load());
		if (exportJob->success) {
			status->setText(T("EventDock.ExportComplete").arg(exportJob->completed));
		} else if (exportJob->result.error == SR_EVENT_EXPORT_CANCELLED) {
			setStatus("EventDock.ExportCancelled");
		} else {
			status->setText(
				T("EventDock.ExportError")
					.arg(QString::fromUtf8(exportJob->failedCamera.c_str()),
					     QString::fromUtf8(sr_event_export_error_text(exportJob->result.error))));
		}
		exportCancelButton->setEnabled(false);
		exportModeCombo->setEnabled(true);
		exportJob.reset();
	}

	void setMarkIn()
	{
		if (!recordingHasMedia()) {
			setStatus("EventDock.RecordBeforeMark");
			return;
		}
		if (!controller || !sr_event_controller_mark_in(controller, obs_get_video_frame_time())) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.InSet");
	}

	void setMarkOut()
	{
		uint64_t eventId = 0;
		if (!controller || !sr_event_controller_mark_out(controller, obs_get_video_frame_time(), &eventId)) {
			setStatus("EventDock.NeedIn");
			return;
		}
		setCreatedStatus(eventId);
		refresh(eventId);
	}

	void quickMark(unsigned seconds)
	{
		if (!recordingHasMedia()) {
			setStatus("EventDock.RecordBeforeMark");
			return;
		}
		uint64_t eventId = 0;
		if (!controller || !sr_event_controller_quick_mark(controller, obs_get_video_frame_time(),
								   seconds * NS_PER_SECOND, 0, &eventId)) {
			setStatus("EventDock.Failed");
			return;
		}
		setCreatedStatus(eventId);
		refresh(eventId);
	}

	void refresh(uint64_t selectEventId = 0)
	{
		if (!controller || !table || tableEditing || table->selectionGestureActive() ||
		    (thumbnailViewActive() && (QApplication::mouseButtons() & Qt::LeftButton)))
			return;
		std::vector<uint64_t> preserveSelection;
		if (selectEventId)
			preserveSelection.push_back(selectEventId);
		else
			preserveSelection = selectedEventIds();
		const uint64_t currentEventId = selectEventId ? selectEventId : selectedEventId();

		uint64_t *eventIds = nullptr;
		size_t count = 0;
		if (!sr_event_controller_get_list_events(controller, currentList(), &eventIds, &count)) {
			setStatus("EventDock.Failed");
			return;
		}

		tableRefreshing = true;
		const QSignalBlocker tableSignals(table);
		table->setUpdatesEnabled(false);
		table->setRowCount((int)count);
		std::vector<int> selectedRows;
		int currentRow = -1;
		for (size_t i = 0; i < count; i++) {
			sr_event_record event = {};
			if (!sr_event_controller_get_event(controller, eventIds[i], &event))
				continue;

			auto *id = new QTableWidgetItem(QString::number(event.id));
			id->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(event.id));
			id->setFlags(id->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 0, id);
			auto *duration = new QTableWidgetItem(durationText(event));
			duration->setFlags(duration->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 1, duration);
			const bool inheritedSpeed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL &&
						    !event.speed_override;
			auto *speedItem = new QTableWidgetItem(
				inheritedSpeed ? QStringLiteral("--")
					       : QString::number(event.speed_percent, 'f', 0) + QStringLiteral("%"));
			speedItem->setToolTip(T("EventDock.EventSpeed.Tooltip"));
			table->setItem((int)i, 2, speedItem);
			auto *state = new QTableWidgetItem(stateText(event));
			state->setFlags(state->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 3, state);
			table->setItem((int)i, 4,
				       new QTableWidgetItem(QString::fromUtf8(event.name ? event.name : "")));
			table->setItem((int)i, 5, new QTableWidgetItem(QString::fromUtf8(event.tag ? event.tag : "")));
			if (std::find(preserveSelection.begin(), preserveSelection.end(), event.id) !=
			    preserveSelection.end())
				selectedRows.push_back((int)i);
			if (event.id == currentEventId)
				currentRow = (int)i;
			sr_event_controller_free_event(&event);
		}
		bfree(eventIds);
		table->clearSelection();
		for (int row : selectedRows)
			table->selectionModel()->select(table->model()->index(row, 0),
							QItemSelectionModel::Select | QItemSelectionModel::Rows);
		if (currentRow >= 0)
			table->setCurrentCell(currentRow, 0, QItemSelectionModel::NoUpdate);
		table->setUpdatesEnabled(true);
		tableRefreshing = false;
		refreshEventGallery();
		refreshAngleCoverage();
	}

	void editEvent(QTableWidgetItem *item)
	{
		if (tableRefreshing || !controller || !item ||
		    (item->column() != 2 && item->column() != 4 && item->column() != 5))
			return;
		tableEditing = false;
		QTableWidgetItem *idItem = table->item(item->row(), 0);
		const uint64_t eventId = idItem ? idItem->data(Qt::UserRole).toULongLong() : 0;
		sr_event_record event = {};
		if (!eventId || !sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			return;
		}

		QString speedText = table->item(item->row(), 2)->text().trimmed();
		const bool speedOverride = !speedText.isEmpty() && speedText != QStringLiteral("--");
		double speed = event.speed_percent;
		bool speedOk = true;
		if (speedOverride) {
			speedText.remove(QChar('%'));
			speed = speedText.trimmed().toDouble(&speedOk);
		}
		const QByteArray name = table->item(item->row(), 4)->text().trimmed().toUtf8();
		const QByteArray tag = table->item(item->row(), 5)->text().trimmed().toUtf8();
		if (!speedOk || (speedOverride && (speed < 10.0 || speed > 400.0))) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.InvalidSpeed");
			refresh(eventId);
			return;
		}

		sr_event_write update = {};
		update.in_ns = event.in_ns;
		update.out_ns = event.out_ns;
		update.preferred_camera_id = event.preferred_camera_id;
		update.speed_percent = speed;
		update.speed_override = speedOverride;
		update.audio_mode = event.audio_mode;
		update.protected_event = event.protected_event;
		update.played = event.played;
		update.pending = event.pending;
		update.name = name.constData();
		update.tag = tag.constData();
		const bool ok = sr_event_controller_update_event(controller, eventId, &update);
		sr_event_controller_free_event(&event);
		if (!ok)
			setStatus("EventDock.Failed");
		else
			setStatus("EventDock.Edited");
		refresh(eventId);
	}

	void moveRow(int delta)
	{
		const int row = table->currentRow();
		const uint64_t eventId = selectedEventId();
		if (!eventId || row < 0)
			return;
		const int target = row + delta;
		if (target < 0 || target >= table->rowCount())
			return;
		if (!sr_event_controller_reorder(controller, eventId, currentList(), target)) {
			setStatus("EventDock.Failed");
			return;
		}
		refresh(eventId);
	}

	void copySelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		if (!sr_event_controller_copy_to_list(controller, eventId, targetList(), -1)) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.Copied");
	}

	void moveSelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		const unsigned source = currentList();
		const unsigned target = targetList();
		if (!sr_event_controller_move_to_list(controller, eventId, source, target, -1)) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.Moved");
		refresh();
	}

	void duplicateSelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		uint64_t newEventId = 0;
		if (!sr_event_controller_duplicate(controller, eventId, targetList(), -1, &newEventId)) {
			setStatus("EventDock.Failed");
			return;
		}
		setCreatedStatus(newEventId);
		if (targetList() == currentList())
			refresh(newEventId);
	}

	void togglePlayed()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			return;
		}
		const bool value = !event.played;
		sr_event_controller_free_event(&event);
		if (!sr_event_controller_set_played(controller, eventId, value)) {
			setStatus("EventDock.Failed");
			return;
		}
		refresh(eventId);
	}

	void toggleProtected()
	{
		const std::vector<uint64_t> ids = selectedEventIds();
		if (ids.empty())
			return;
		sr_event_record first = {};
		if (!sr_event_controller_get_event(controller, ids.front(), &first)) {
			setStatus("EventDock.Failed");
			return;
		}
		const bool value = !first.protected_event;
		sr_event_controller_free_event(&first);
		bool ok = true;
		for (uint64_t id : ids)
			ok = sr_event_controller_set_protected(controller, id, value) && ok;
		if (!ok)
			setStatus("EventDock.Failed");
		refresh();
	}

	void clearDeletedEventUi(uint64_t eventId)
	{
		eventThumbnailCache.erase(eventId);
		anglePreviewCache.erase(eventId);
		if (previewTargetEventId == eventId) {
			previewTargetEventId = 0;
			previewLoadedEventId = 0;
		}
		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			const auto bus = static_cast<sr_replay_bus>(i);
			sr_replay_playlist_state playlist = {};
			if (sr_replay_playlist_get_state(bus, &playlist) && playlist.active &&
			    playlist.event_id == eventId)
				sr_replay_playlist_stop(bus);
			sr_replay_channel_state state = {};
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.event_id == eventId)
				sr_replay_channel_clear(bus);
		}
	}

	void deleteSelected(bool deleteMedia)
	{
		const std::vector<uint64_t> ids = selectedEventIds();
		if (ids.empty())
			return;

		size_t deleted = 0;
		size_t protectedSkipped = 0;
		size_t errors = 0;
		sr_storage_cleanup_result cleanupTotal = {};
		for (uint64_t eventId : ids) {
			sr_event_record event = {};
			if (!sr_event_controller_get_event(controller, eventId, &event)) {
				errors++;
				continue;
			}
			const bool protectedEvent = event.protected_event;
			sr_event_controller_free_event(&event);
			if (protectedEvent) {
				protectedSkipped++;
				continue;
			}

			bool ok = false;
			if (!deleteMedia) {
				ok = sr_event_controller_delete_event(controller, eventId);
			} else {
				sr_storage_cleanup_result cleanup = {};
				ok = sr_event_controller_delete_event_with_media(controller, eventId, &cleanup);
				cleanupTotal.segments_examined += cleanup.segments_examined;
				cleanupTotal.segments_deleted += cleanup.segments_deleted;
				cleanupTotal.segments_pinned += cleanup.segments_pinned;
				cleanupTotal.camera_dirs_scanned += cleanup.camera_dirs_scanned;
				cleanupTotal.errors += cleanup.errors;
			}
			if (!ok) {
				errors++;
				continue;
			}
			deleted++;
			clearDeletedEventUi(eventId);
		}

		errors += cleanupTotal.errors;
		status->setText(T(deleteMedia ? "EventDock.DeleteMediaSummary" : "EventDock.DeleteSummary")
					.arg(deleted)
					.arg(protectedSkipped)
					.arg(errors)
					.arg(cleanupTotal.segments_deleted)
					.arg(cleanupTotal.segments_pinned));
		refresh();
	}

	sr_event_controller *controller = nullptr;
	QTabBar *listTabs = nullptr;
	QComboBox *targetCombo = nullptr;
	QComboBox *cameraCombo = nullptr;
	QComboBox *busCombo = nullptr;
	QComboBox *speedCombo = nullptr;
	QComboBox *audioCombo = nullptr;
	QComboBox *exportModeCombo = nullptr;
	QGridLayout *angleGrid = nullptr;
	QVector<QToolButton *> angleButtons;
	QStackedWidget *eventViewStack = nullptr;
	QListWidget *thumbnailList = nullptr;
	SrRangeSlider *timelineSlider = nullptr;
	QSlider *jogSlider = nullptr;
	QSlider *shuttleSlider = nullptr;
	QLabel *timelineTime = nullptr;
	QLabel *shuttleValue = nullptr;
	QPushButton *reverseButton = nullptr;
	QPushButton *loopButton = nullptr;
	QPushButton *exportCancelButton = nullptr;
	QProgressBar *exportProgressBar = nullptr;
	SrEventTable *table = nullptr;
	QTableWidget *performanceTable = nullptr;
	QWidget *performancePanel = nullptr;
	QToolButton *performanceToggle = nullptr;
	QToolButton *recordToggle = nullptr;
	QToolButton *setupButton = nullptr;
	QLabel *setupSourceStatus = nullptr;
	QLabel *recordStatus = nullptr;
	QLabel *performanceSummary = nullptr;
	QLabel *programStatus = nullptr;
	QLabel *cueAStatus = nullptr;
	QLabel *cueBStatus = nullptr;
	QLabel *transportStatus = nullptr;
	QLabel *status = nullptr;
	QTimer *refreshTimer = nullptr;
	QTimer *transportTimer = nullptr;
	bool timelineDragging = false;
	bool tableRefreshing = false;
	bool tableEditing = false;
	int jogLastValue = 0;
	unsigned cameraRefreshTicks = 0;
	std::unique_ptr<ExportJob> exportJob;
	std::unique_ptr<AnglePreviewJob> anglePreviewJob;
	std::unique_ptr<EventThumbnailJob> eventThumbnailJob;
	std::map<uint64_t, std::map<std::string, QIcon>> anglePreviewCache;
	std::map<uint64_t, CachedEventThumbnail> eventThumbnailCache;
	std::vector<uint64_t> galleryEventIds;
	unsigned galleryListId = 0;
	uint64_t galleryGeneration = 1;
	bool syncingEventViews = false;
	uint64_t timelineEventId = 0;
	uint64_t previewTargetEventId = 0;
	uint64_t previewLoadedEventId = 0;
};

} // namespace

QWidget *sr_event_dock_create(struct sr_event_controller *controller, QWidget *parent)
{
	return new SrEventDock(controller, parent);
}
