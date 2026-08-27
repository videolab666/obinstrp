/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-multiview-dock.h"

#include "sr-camera-list.h"
#include "sr-disk-player.h"
#include "sr-event-dock.h"
#include "sr-replay-coverage.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSet>
#include <QSizePolicy>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#define NS_PER_SECOND 1000000000ULL

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString clockText(uint64_t ns)
{
	const uint64_t totalMs = ns / 1000000ULL;
	const uint64_t hours = totalMs / 3600000ULL;
	const uint64_t minutes = (totalMs / 60000ULL) % 60ULL;
	const uint64_t seconds = (totalMs / 1000ULL) % 60ULL;
	const uint64_t millis = totalMs % 1000ULL;
	if (hours)
		return QStringLiteral("%1:%2:%3.%4")
			.arg(hours, 2, 10, QChar('0'))
			.arg(minutes, 2, 10, QChar('0'))
			.arg(seconds, 2, 10, QChar('0'))
			.arg(millis, 3, 10, QChar('0'));
	return QStringLiteral("%1:%2.%3")
		.arg(minutes, 2, 10, QChar('0'))
		.arg(seconds, 2, 10, QChar('0'))
		.arg(millis, 3, 10, QChar('0'));
}

uint64_t addSignedOffset(uint64_t timestamp, int64_t offset)
{
	if (offset >= 0) {
		const uint64_t value = (uint64_t)offset;
		return value <= UINT64_MAX - timestamp ? timestamp + value : UINT64_MAX;
	}
	const uint64_t value = (uint64_t)(-(offset + 1)) + 1ULL;
	return value < timestamp ? timestamp - value : 0;
}

class SrMultiviewDecoder {
public:
	SrMultiviewDecoder()
	{
		worker = std::thread([this]() { run(); });
	}

	~SrMultiviewDecoder()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
			condition.notify_all();
		}
		if (worker.joinable())
			worker.join();
	}

	void setSource(const QString &session, const QString &camera)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const std::string nextSession = session.toUtf8().constData();
		const std::string nextCamera = camera.toUtf8().constData();
		if (nextSession == requestedSession && nextCamera == requestedCamera)
			return;
		requestedSession = nextSession;
		requestedCamera = nextCamera;
		requestSerial++;
		condition.notify_all();
	}

	void request(uint64_t timestampNs, int targetHeight)
	{
		std::lock_guard<std::mutex> lock(mutex);
		requestedTimestampNs = timestampNs;
		requestedHeight = targetHeight;
		requestSerial++;
		condition.notify_all();
	}

	bool takeImage(QImage *image, uint64_t *actualTimestampNs, bool *success)
	{
		if (!image)
			return false;
		std::lock_guard<std::mutex> lock(mutex);
		if (!publishedSerial || publishedSerial == consumedSerial)
			return false;
		consumedSerial = publishedSerial;
		*image = publishedImage;
		if (actualTimestampNs)
			*actualTimestampNs = publishedTimestampNs;
		if (success)
			*success = publishedSuccess;
		return true;
	}

private:
	void publish(uint64_t serial, bool success, uint64_t timestampNs, const QImage &image)
	{
		std::lock_guard<std::mutex> lock(mutex);
		/* A slow random seek is obsolete as soon as the operator requested a
		 * newer timestamp. Never let stale decode work visually catch up later. */
		if (serial != requestSerial)
			return;
		publishedSerial = serial;
		publishedSuccess = success;
		publishedTimestampNs = timestampNs;
		publishedImage = image;
	}

	static bool softwareFrame(const AVFrame *input, AVFrame **owned, const AVFrame **source)
	{
		if (!input || !owned || !source)
			return false;
		*owned = nullptr;
		*source = input;
		if (!input->hw_frames_ctx)
			return true;

		AVFrame *transfer = av_frame_alloc();
		if (!transfer)
			return false;
		if (av_hwframe_transfer_data(transfer, input, 0) < 0) {
			av_frame_free(&transfer);
			return false;
		}
		*owned = transfer;
		*source = transfer;
		return true;
	}

	bool convertFrame(const AVFrame *frame, int targetHeight, QImage *image)
	{
		if (!frame || !image)
			return false;
		AVFrame *owned = nullptr;
		const AVFrame *source = nullptr;
		if (!softwareFrame(frame, &owned, &source) || !source || source->width <= 0 || source->height <= 0) {
			av_frame_free(&owned);
			return false;
		}

		int height = targetHeight > 0 ? std::min(targetHeight, source->height) : source->height;
		height = std::max(2, height);
		int width = (int)std::llround((long double)source->width * height / source->height);
		width = std::max(2, width);
		if (width & 1)
			width++;
		if (height & 1)
			height++;

		sws = sws_getCachedContext(sws, source->width, source->height, (AVPixelFormat)source->format, width,
					   height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws) {
			av_frame_free(&owned);
			return false;
		}

		QImage converted(width, height, QImage::Format_RGBA8888);
		if (converted.isNull()) {
			av_frame_free(&owned);
			return false;
		}
		uint8_t *dstData[4] = {converted.bits(), nullptr, nullptr, nullptr};
		int dstLinesize[4] = {converted.bytesPerLine(), 0, 0, 0};
		const int rows =
			sws_scale(sws, source->data, source->linesize, 0, source->height, dstData, dstLinesize);
		av_frame_free(&owned);
		if (rows <= 0)
			return false;
		*image = std::move(converted);
		return true;
	}

	void run()
	{
		struct sr_disk_player *player = nullptr;
		std::string openedSession;
		std::string openedCamera;
		uint64_t handledSerial = 0;

		for (;;) {
			uint64_t serial = 0;
			uint64_t timestampNs = 0;
			int targetHeight = 360;
			std::string session;
			std::string camera;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this, handledSerial]() {
					return stopping || requestSerial != handledSerial;
				});
				if (stopping)
					break;
				serial = requestSerial;
				timestampNs = requestedTimestampNs;
				targetHeight = requestedHeight;
				session = requestedSession;
				camera = requestedCamera;
			}

			if (session != openedSession || camera != openedCamera) {
				sr_disk_player_destroy(player);
				player = nullptr;
				openedSession = session;
				openedCamera = camera;
				if (!session.empty() && !camera.empty())
					player = sr_disk_player_create_with_cache(session.c_str(), camera.c_str(),
										  12ULL * 1024ULL * 1024ULL);
			}

			bool ok = false;
			QImage image;
			uint64_t actualNs = timestampNs;
			if (player && timestampNs) {
				AVFrame *frame = nullptr;
				ok = sr_disk_player_decode_at(player, timestampNs, &frame, &actualNs);
				if (!ok) {
					sr_disk_player_refresh(player);
					ok = sr_disk_player_decode_at(player, timestampNs, &frame, &actualNs);
				}
				if (ok)
					ok = convertFrame(frame, targetHeight, &image);
				av_frame_free(&frame);
			}
			publish(serial, ok, actualNs, image);
			handledSerial = serial;
		}

		sr_disk_player_destroy(player);
		sws_freeContext(sws);
		sws = nullptr;
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::thread worker;
	bool stopping = false;
	uint64_t requestSerial = 0;
	uint64_t requestedTimestampNs = 0;
	int requestedHeight = 360;
	std::string requestedSession;
	std::string requestedCamera;

	uint64_t publishedSerial = 0;
	uint64_t consumedSerial = 0;
	uint64_t publishedTimestampNs = 0;
	bool publishedSuccess = false;
	QImage publishedImage;
	SwsContext *sws = nullptr;
};

class SrMultiviewTile : public QFrame {
public:
	using Callback = std::function<void()>;

	explicit SrMultiviewTile(const QString &camera, QWidget *parent = nullptr) : QFrame(parent), cameraName(camera)
	{
		setFrameShape(QFrame::Box);
		setLineWidth(2);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		setMinimumSize(190, 130);
		setCursor(Qt::PointingHandCursor);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 3, 4, 3);
		layout->setSpacing(2);
		title = new QLabel(cameraName, this);
		title->setStyleSheet(QStringLiteral("font-weight: 600;"));
		title->setAttribute(Qt::WA_TransparentForMouseEvents);
		layout->addWidget(title);
		picture = new QLabel(this);
		picture->setAlignment(Qt::AlignCenter);
		picture->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
		picture->setMinimumSize(160, 90);
		picture->setStyleSheet(QStringLiteral("background: #080808; color: #aaa;"));
		picture->setText(T("Multiview.Waiting"));
		picture->setAttribute(Qt::WA_TransparentForMouseEvents);
		layout->addWidget(picture, 1);
		footer = new QLabel(this);
		footer->setStyleSheet(QStringLiteral("color: gray; font-size: 10px;"));
		footer->setAttribute(Qt::WA_TransparentForMouseEvents);
		layout->addWidget(footer);
		updateStyle();
	}

	const QString &camera() const { return cameraName; }
	SrMultiviewDecoder &decoder() { return previewDecoder; }

	void setCallbacks(Callback click, Callback doubleClick)
	{
		clickHandler = std::move(click);
		doubleClickHandler = std::move(doubleClick);
	}

	void setSelected(bool value)
	{
		if (selected == value)
			return;
		selected = value;
		updateStyle();
	}

	void setPreview(bool value)
	{
		if (preview == value)
			return;
		preview = value;
		updateTitle();
		updateStyle();
	}

	void setCoverage(enum sr_replay_coverage coverage, bool atPlayhead)
	{
		this->coverage = coverage;
		this->atPlayhead = atPlayhead;
		updateTitle();
		if (coverage == SR_REPLAY_COVERAGE_NONE)
			setMessage(T("Multiview.NoCoverage"));
		else if (!atPlayhead)
			setMessage(T("Multiview.NoMediaAtCursor"));
	}

	void setImage(const QImage &image, uint64_t relativeTimestampNs)
	{
		if (image.isNull())
			return;
		lastImage = image;
		footer->setText(clockText(relativeTimestampNs));
		refreshPixmap();
	}

	void setDecodeFailed() { setMessage(T("Multiview.DecodeWaiting")); }

	void setMessage(const QString &message)
	{
		if (lastImage.isNull())
			picture->setText(message);
		footer->setText(message);
	}

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QFrame::resizeEvent(event);
		refreshPixmap();
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && clickHandler) {
			clickHandler();
			event->accept();
			return;
		}
		QFrame::mousePressEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton && doubleClickHandler) {
			doubleClickHandler();
			event->accept();
			return;
		}
		QFrame::mouseDoubleClickEvent(event);
	}

private:
	void refreshPixmap()
	{
		if (lastImage.isNull() || !picture || picture->width() <= 0 || picture->height() <= 0)
			return;
		picture->setPixmap(QPixmap::fromImage(lastImage).scaled(picture->size(), Qt::KeepAspectRatio,
									Qt::SmoothTransformation));
	}

	void updateTitle()
	{
		QString marker = coverage == SR_REPLAY_COVERAGE_FULL      ? QStringLiteral("●")
				 : coverage == SR_REPLAY_COVERAGE_PARTIAL ? QStringLiteral("◐")
									  : QStringLiteral("○");
		QString prefix;
		if (selected)
			prefix += QStringLiteral("✓ ");
		if (preview)
			prefix += QStringLiteral("▶ ");
		title->setText(QStringLiteral("%1%2 %3").arg(prefix, marker, cameraName));
	}

	void updateStyle()
	{
		QString border = selected  ? QStringLiteral("#2f83ff")
				 : preview ? QStringLiteral("#d3a11f")
					   : QStringLiteral("#555");
		setStyleSheet(
			QStringLiteral("SrMultiviewTile { border: 2px solid %1; border-radius: 4px; }").arg(border));
		updateTitle();
	}

	QString cameraName;
	QLabel *title = nullptr;
	QLabel *picture = nullptr;
	QLabel *footer = nullptr;
	QImage lastImage;
	bool selected = false;
	bool preview = false;
	bool atPlayhead = false;
	enum sr_replay_coverage coverage = SR_REPLAY_COVERAGE_NONE;
	Callback clickHandler;
	Callback doubleClickHandler;
	SrMultiviewDecoder previewDecoder;
};

class SrMultiviewTimeline : public QWidget {
public:
	using SeekHandler = std::function<void(uint64_t)>;
	using RangeHandler = std::function<void(uint64_t, uint64_t)>;

	explicit SrMultiviewTimeline(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(72);
		setFocusPolicy(Qt::StrongFocus);
		setMouseTracking(true);
	}

	void setHandlers(SeekHandler seek, RangeHandler range)
	{
		seekHandler = std::move(seek);
		rangeHandler = std::move(range);
	}

	void setState(const sr_event_editor_snapshot &state)
	{
		const bool first = !haveRecording;
		haveRecording = state.record_end_ns > state.record_start_ns;
		if (!haveRecording) {
			update();
			return;
		}
		recordStartNs = state.record_start_ns;
		recordEndNs = state.record_end_ns;
		if (first || !zoomLocked) {
			viewStartNs = recordStartNs;
			viewEndNs = recordEndNs;
		} else {
			clampView();
		}
		if (drag == Drag::None) {
			playheadNs = clamp(state.playhead_ns);
			inNs = clamp(state.in_ns);
			outNs = clamp(state.out_ns);
			haveRange = state.out_ns > state.in_ns;
		}
		update();
	}

	void fit()
	{
		if (!haveRecording)
			return;
		zoomLocked = false;
		viewStartNs = recordStartNs;
		viewEndNs = recordEndNs;
		update();
	}

	void live()
	{
		if (!haveRecording)
			return;
		playheadNs = recordEndNs;
		if (zoomLocked) {
			const uint64_t span = viewSpan();
			viewEndNs = recordEndNs;
			viewStartNs = span < recordEndNs - recordStartNs ? recordEndNs - span : recordStartNs;
		}
		if (seekHandler)
			seekHandler(playheadNs);
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), palette().window());
		if (!haveRecording || viewEndNs <= viewStartNs) {
			painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
			painter.drawText(rect(), Qt::AlignCenter, T("Multiview.NoTimeline"));
			return;
		}
		const QRect area = timelineRect();
		painter.fillRect(area, palette().base());
		painter.setPen(palette().mid().color());
		painter.drawRect(area.adjusted(0, 0, -1, -1));
		paintRuler(painter, area);

		if (haveRange) {
			const int left = xFromTimestamp(inNs);
			const int right = xFromTimestamp(outNs);
			QColor rangeColor = palette().highlight().color();
			rangeColor.setAlpha(90);
			painter.fillRect(QRect(std::max(area.left(), left), area.top() + 18,
					       std::max(0, std::min(area.right(), right) - std::max(area.left(), left)),
					       area.height() - 19),
					 rangeColor);
			paintMarker(painter, inNs, QStringLiteral("IN"), false, area);
			paintMarker(painter, outNs, QStringLiteral("OUT"), true, area);
		}

		if (playheadNs >= viewStartNs && playheadNs <= viewEndNs) {
			const int x = xFromTimestamp(playheadNs);
			const QColor cursor(QStringLiteral("#e24a4a"));
			painter.setPen(QPen(cursor, 2));
			painter.drawLine(x, area.top(), x, area.bottom());
			painter.setBrush(cursor);
			painter.setPen(Qt::NoPen);
			QPolygon triangle;
			triangle << QPoint(x - 5, area.top()) << QPoint(x + 5, area.top()) << QPoint(x, area.top() + 7);
			painter.drawPolygon(triangle);
		}
		const QString zoom = zoomLocked ? QStringLiteral("%1×").arg(
							  (double)(recordEndNs - recordStartNs) / viewSpan(), 0, 'f', 1)
						: QStringLiteral("FIT");
		painter.setPen(palette().text().color());
		painter.drawText(QRect(area.right() - 80, 1, 76, 16), Qt::AlignRight | Qt::AlignVCenter, zoom);
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		if (!haveRecording || event->button() != Qt::LeftButton) {
			QWidget::mousePressEvent(event);
			return;
		}
		const QPoint point = event->position().toPoint();
		const QRect area = timelineRect();
		if (haveRange && markerRect(inNs, false, area).contains(point))
			drag = Drag::In;
		else if (haveRange && markerRect(outNs, true, area).contains(point))
			drag = Drag::Out;
		else
			drag = Drag::Playhead;
		apply(timestampFromX(point.x()));
		event->accept();
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (drag == Drag::None || !(event->buttons() & Qt::LeftButton)) {
			const QRect area = timelineRect();
			const QPoint point = event->position().toPoint();
			const bool marker = haveRange && (markerRect(inNs, false, area).contains(point) ||
							  markerRect(outNs, true, area).contains(point));
			setCursor(marker ? Qt::SizeHorCursor : Qt::ArrowCursor);
			return;
		}
		apply(timestampFromX(event->position().toPoint().x()));
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton || drag == Drag::None) {
			QWidget::mouseReleaseEvent(event);
			return;
		}
		const Drag released = drag;
		drag = Drag::None;
		if ((released == Drag::In || released == Drag::Out) && rangeHandler && outNs > inNs)
			rangeHandler(inNs, outNs);
		event->accept();
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() == Qt::LeftButton) {
			fit();
			event->accept();
			return;
		}
		QWidget::mouseDoubleClickEvent(event);
	}

	void wheelEvent(QWheelEvent *event) override
	{
		if (!haveRecording)
			return;
		int delta = event->angleDelta().y();
		if (!delta)
			delta = event->angleDelta().x();
		if (!delta)
			return;
		const double steps = (double)delta / 120.0;
		if (event->modifiers().testFlag(Qt::ShiftModifier)) {
			if (zoomLocked)
				pan(steps > 0.0 ? -1 : 1, (uint64_t)(viewSpan() * 0.12 * std::abs(steps)));
		} else {
			zoom(steps);
		}
		event->accept();
	}

private:
	enum class Drag { None, Playhead, In, Out };

	QRect timelineRect() const { return rect().adjusted(8, 18, -8, -5); }
	uint64_t viewSpan() const { return viewEndNs > viewStartNs ? viewEndNs - viewStartNs : 0; }

	uint64_t clamp(uint64_t timestamp) const { return std::min(recordEndNs, std::max(recordStartNs, timestamp)); }

	int xFromTimestamp(uint64_t timestamp) const
	{
		const QRect area = timelineRect();
		if (!viewSpan())
			return area.left();
		const long double ratio = ((long double)timestamp - viewStartNs) / (long double)viewSpan();
		return area.left() + (int)std::llround(ratio * area.width());
	}

	uint64_t timestampFromX(int x) const
	{
		const QRect area = timelineRect();
		const int bounded = std::max(area.left(), std::min(area.right(), x));
		const long double ratio = (long double)(bounded - area.left()) / std::max(1, area.width());
		return clamp(viewStartNs + (uint64_t)((long double)viewSpan() * ratio));
	}

	QRect markerRect(uint64_t timestamp, bool right, const QRect &area) const
	{
		const int x = xFromTimestamp(timestamp);
		const int width = 34;
		return right ? QRect(x - width, area.top(), width, 18) : QRect(x, area.top(), width, 18);
	}

	void paintMarker(QPainter &painter, uint64_t timestamp, const QString &text, bool right, const QRect &area)
	{
		const int x = xFromTimestamp(timestamp);
		const QRect label = markerRect(timestamp, right, area);
		QColor blue(QStringLiteral("#3277cc"));
		painter.fillRect(label, blue);
		painter.setPen(Qt::white);
		painter.drawText(label, Qt::AlignCenter, text);
		painter.setPen(QPen(blue, 2));
		painter.drawLine(x, area.top() + 18, x, area.bottom());
	}

	void paintRuler(QPainter &painter, const QRect &area)
	{
		const uint64_t span = viewSpan();
		if (!span)
			return;
		const uint64_t candidates[] = {100000000ULL,   250000000ULL,    500000000ULL,   1000000000ULL,
					       2000000000ULL,  5000000000ULL,   10000000000ULL, 30000000000ULL,
					       60000000000ULL, 300000000000ULL, 600000000000ULL};
		uint64_t step = candidates[0];
		for (uint64_t candidate : candidates) {
			step = candidate;
			if (span / candidate <= 8)
				break;
		}
		const uint64_t first = ((viewStartNs + step - 1) / step) * step;
		painter.setPen(palette().mid().color());
		for (uint64_t t = first; t <= viewEndNs && t <= UINT64_MAX - step; t += step) {
			const int x = xFromTimestamp(t);
			painter.drawLine(x, area.top(), x, area.top() + 6);
			const uint64_t relative = t > recordStartNs ? t - recordStartNs : 0;
			painter.drawText(QRect(x - 48, 1, 96, 15), Qt::AlignCenter, clockText(relative));
		}
	}

	void apply(uint64_t timestamp)
	{
		timestamp = clamp(timestamp);
		if (drag == Drag::In && haveRange) {
			inNs = std::min(timestamp, outNs > 1 ? outNs - 1 : outNs);
			playheadNs = inNs;
		} else if (drag == Drag::Out && haveRange) {
			outNs = std::max(timestamp, inNs < UINT64_MAX ? inNs + 1 : inNs);
			playheadNs = outNs;
		} else {
			playheadNs = timestamp;
		}
		if (seekHandler)
			seekHandler(playheadNs);
		update();
	}

	void clampView()
	{
		if (!zoomLocked || !haveRecording)
			return;
		const uint64_t total = recordEndNs - recordStartNs;
		const uint64_t span = viewSpan();
		if (!span || span >= total) {
			fit();
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

	void pan(int direction, uint64_t amount)
	{
		if (!zoomLocked || !amount)
			return;
		if (direction < 0) {
			const uint64_t shift = std::min(amount, viewStartNs - recordStartNs);
			viewStartNs -= shift;
			viewEndNs -= shift;
		} else {
			const uint64_t shift = std::min(amount, recordEndNs - viewEndNs);
			viewStartNs += shift;
			viewEndNs += shift;
		}
		update();
	}

	void zoom(double steps)
	{
		const uint64_t total = recordEndNs - recordStartNs;
		if (!total)
			return;
		const uint64_t current = zoomLocked ? viewSpan() : total;
		const long double factor = std::pow(1.25L, (long double)steps);
		uint64_t next = (uint64_t)((long double)current / factor);
		next = std::max<uint64_t>(250000000ULL, std::min(next, total));
		if (next >= total - std::min<uint64_t>(1000000ULL, total / 100)) {
			fit();
			return;
		}
		const uint64_t center = zoomLocked ? viewStartNs + current / 2 : playheadNs;
		zoomLocked = true;
		viewStartNs = center > next / 2 ? center - next / 2 : recordStartNs;
		viewEndNs = viewStartNs + next;
		clampView();
		update();
	}

	bool haveRecording = false;
	bool haveRange = false;
	bool zoomLocked = false;
	uint64_t recordStartNs = 0;
	uint64_t recordEndNs = 0;
	uint64_t viewStartNs = 0;
	uint64_t viewEndNs = 0;
	uint64_t playheadNs = 0;
	uint64_t inNs = 0;
	uint64_t outNs = 0;
	Drag drag = Drag::None;
	SeekHandler seekHandler;
	RangeHandler rangeHandler;
};

class SrMultiviewDock : public QWidget {
public:
	explicit SrMultiviewDock(sr_event_controller *eventController, QWidget *parent = nullptr) : QWidget(parent)
	{
		(void)eventController;
		setObjectName(QStringLiteral("PitelInstantReplayMultiview"));
		setFocusPolicy(Qt::StrongFocus);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(4, 4, 4, 4);
		root->setSpacing(4);

		auto *toolbar = new QHBoxLayout();
		cameraMenuButton = new QToolButton(this);
		cameraMenuButton->setText(T("Multiview.Cameras"));
		cameraMenuButton->setPopupMode(QToolButton::InstantPopup);
		cameraMenu = new QMenu(cameraMenuButton);
		cameraMenuButton->setMenu(cameraMenu);
		toolbar->addWidget(cameraMenuButton);
		toolbar->addWidget(new QLabel(T("Multiview.Quality"), this));
		quality = new QComboBox(this);
		quality->addItem(T("Multiview.QualityAuto"), 0);
		quality->addItem(QStringLiteral("360p"), 360);
		quality->addItem(QStringLiteral("540p"), 540);
		quality->addItem(QStringLiteral("720p"), 720);
		quality->addItem(T("Multiview.QualitySource"), -1);
		toolbar->addWidget(quality);
		toolbar->addWidget(new QLabel(T("Multiview.Fps"), this));
		fps = new QComboBox(this);
		fps->addItem(T("Multiview.FpsAuto"), 0);
		fps->addItem(QStringLiteral("15"), 15);
		fps->addItem(QStringLiteral("20"), 20);
		fps->addItem(QStringLiteral("25"), 25);
		fps->addItem(QStringLiteral("30"), 30);
		toolbar->addWidget(fps);
		autoAngle = new QPushButton(T("Multiview.AutoAngle"), this);
		toolbar->addWidget(autoAngle);
		toolbar->addStretch(1);
		stateLabel = new QLabel(T("Multiview.SelectEvent"), this);
		toolbar->addWidget(stateLabel);
		root->addLayout(toolbar);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		gridHost = new QWidget(scroll);
		grid = new QGridLayout(gridHost);
		grid->setContentsMargins(0, 0, 0, 0);
		grid->setSpacing(4);
		scroll->setWidget(gridHost);
		root->addWidget(scroll, 1);

		timeline = new SrMultiviewTimeline(this);
		timeline->setHandlers([this](uint64_t timestamp) { sr_event_dock_editor_seek(timestamp); },
				      [this](uint64_t inNs, uint64_t outNs) {
					      sr_event_dock_editor_set_range(inNs, outNs);
				      });
		root->addWidget(timeline);

		auto *controls = new QHBoxLayout();
		playPause = new QPushButton(T("Multiview.PlayPause"), this);
		playFromIn = new QPushButton(T("Multiview.PlayFromIn"), this);
		gotoIn = new QPushButton(QStringLiteral("|< IN"), this);
		setIn = new QPushButton(T("Multiview.SetIn"), this);
		setOut = new QPushButton(T("Multiview.SetOut"), this);
		gotoOut = new QPushButton(QStringLiteral("OUT >|"), this);
		prevFrame = new QPushButton(QStringLiteral("◀ 1f"), this);
		nextFrame = new QPushButton(QStringLiteral("1f ▶"), this);
		loop = new QPushButton(T("Multiview.Loop"), this);
		loop->setCheckable(true);
		fit = new QPushButton(QStringLiteral("FIT"), this);
		live = new QPushButton(QStringLiteral("LIVE"), this);
		for (QPushButton *button :
		     {playPause, playFromIn, gotoIn, setIn, setOut, gotoOut, prevFrame, nextFrame, loop, fit, live})
			controls->addWidget(button);
		controls->addStretch(1);
		timeLabel = new QLabel(this);
		controls->addWidget(timeLabel);
		root->addLayout(controls);

		connect(autoAngle, &QPushButton::clicked, this, []() { sr_event_dock_editor_select_camera(nullptr); });
		connect(playPause, &QPushButton::clicked, this, []() { sr_event_dock_editor_toggle_play(); });
		connect(playFromIn, &QPushButton::clicked, this, []() { sr_event_dock_editor_play_from_in(); });
		connect(gotoIn, &QPushButton::clicked, this, []() { sr_event_dock_editor_goto_marker(false); });
		connect(gotoOut, &QPushButton::clicked, this, []() { sr_event_dock_editor_goto_marker(true); });
		connect(setIn, &QPushButton::clicked, this, []() { sr_event_dock_editor_set_marker(false); });
		connect(setOut, &QPushButton::clicked, this, []() { sr_event_dock_editor_set_marker(true); });
		connect(prevFrame, &QPushButton::clicked, this, []() { sr_event_dock_editor_step_frames(-1); });
		connect(nextFrame, &QPushButton::clicked, this, []() { sr_event_dock_editor_step_frames(1); });
		connect(loop, &QPushButton::toggled, this,
			[](bool checked) { sr_event_dock_editor_set_loop(checked); });
		connect(fit, &QPushButton::clicked, timeline, [this]() { timeline->fit(); });
		connect(live, &QPushButton::clicked, timeline, [this]() { timeline->live(); });
		connect(quality, &QComboBox::currentIndexChanged, this, [this](int) { forceDecode = true; });
		connect(fps, &QComboBox::currentIndexChanged, this, [this](int) { forceDecode = true; });

		clock.start();
		cameraTimer = new QTimer(this);
		cameraTimer->setInterval(1500);
		connect(cameraTimer, &QTimer::timeout, this, [this]() { refreshCameraList(); });
		cameraTimer->start();
		uiTimer = new QTimer(this);
		uiTimer->setInterval(33);
		connect(uiTimer, &QTimer::timeout, this, [this]() { tick(); });
		uiTimer->start();
		refreshCameraList();
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		forceDecode = true;
		refreshCameraList();
	}

	void hideEvent(QHideEvent *event) override
	{
		QWidget::hideEvent(event);
		/* Release all readers/decoders while the dock is hidden. The worker
		 * threads remain asleep, so a closed Multiview costs no decoder or
		 * segment-reader resources. */
		for (const auto &tile : tiles)
			tile->decoder().setSource(QString(), QString());
	}

	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
			const int index = event->key() - Qt::Key_1;
			const auto visible = visibleTiles();
			if (index >= 0 && index < (int)visible.size())
				selectCamera(visible[index]->camera());
			event->accept();
			return;
		}
		switch (event->key()) {
		case Qt::Key_Space:
			sr_event_dock_editor_toggle_play();
			event->accept();
			return;
		case Qt::Key_I:
			sr_event_dock_editor_set_marker(false);
			event->accept();
			return;
		case Qt::Key_O:
			sr_event_dock_editor_set_marker(true);
			event->accept();
			return;
		case Qt::Key_BracketLeft:
			sr_event_dock_editor_goto_marker(false);
			event->accept();
			return;
		case Qt::Key_BracketRight:
			sr_event_dock_editor_goto_marker(true);
			event->accept();
			return;
		case Qt::Key_Left:
			sr_event_dock_editor_step_frames(event->modifiers().testFlag(Qt::ShiftModifier) ? -10 : -1);
			event->accept();
			return;
		case Qt::Key_Right:
			sr_event_dock_editor_step_frames(event->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1);
			event->accept();
			return;
		default:
			break;
		}
		QWidget::keyPressEvent(event);
	}

private:
	std::vector<SrMultiviewTile *> visibleTiles() const
	{
		std::vector<SrMultiviewTile *> result;
		for (const auto &tile : tiles) {
			if (!hiddenCameras.contains(tile->camera()) &&
			    (soloCamera.isEmpty() || tile->camera() == soloCamera))
				result.push_back(tile.get());
		}
		return result;
	}

	void clearGrid()
	{
		while (QLayoutItem *item = grid->takeAt(0))
			delete item;
	}

	void rebuildGrid()
	{
		clearGrid();
		const auto visible = visibleTiles();
		const int count = (int)visible.size();
		const int columns = soloCamera.isEmpty() ? (count <= 1 ? 1 : count <= 4 ? 2 : 3) : 1;
		for (int i = 0; i < count; i++) {
			grid->addWidget(visible[i], i / columns, i % columns);
			visible[i]->setVisible(true);
		}
		for (const auto &tile : tiles) {
			if (std::find(visible.begin(), visible.end(), tile.get()) == visible.end()) {
				tile->setVisible(false);
				tile->decoder().setSource(QString(), QString());
			}
		}
		for (int column = 0; column < std::max(1, columns); column++)
			grid->setColumnStretch(column, 1);
		for (int row = 0; row < std::max(1, (count + columns - 1) / columns); row++)
			grid->setRowStretch(row, 1);
		forceDecode = true;
	}

	void rebuildCameraMenu(const QStringList &names)
	{
		cameraMenu->clear();
		for (const QString &camera : names) {
			QAction *action = cameraMenu->addAction(camera);
			action->setCheckable(true);
			action->setChecked(!hiddenCameras.contains(camera));
			connect(action, &QAction::toggled, this, [this, camera](bool checked) {
				if (checked)
					hiddenCameras.remove(camera);
				else
					hiddenCameras.insert(camera);
				if (!soloCamera.isEmpty() && hiddenCameras.contains(soloCamera))
					soloCamera.clear();
				rebuildGrid();
			});
		}
	}

	void refreshCameraList()
	{
		sr_camera_list list = {};
		if (!sr_camera_list_capture(&list))
			return;
		QStringList names;
		for (size_t i = 0; i < list.count; i++)
			names.append(QString::fromUtf8(list.names[i]));
		sr_camera_list_free(&list);
		if (names == cameraNames)
			return;

		cameraNames = names;
		if (cameraNames.size() > 9)
			cameraNames = cameraNames.mid(0, 9);
		QSet<QString> availableCameras;
		for (const QString &camera : cameraNames)
			availableCameras.insert(camera);
		hiddenCameras.intersect(availableCameras);
		tiles.clear();
		for (const QString &camera : cameraNames) {
			auto tile = std::make_unique<SrMultiviewTile>(camera, gridHost);
			SrMultiviewTile *raw = tile.get();
			raw->setCallbacks([this, camera]() { selectCamera(camera); },
					  [this, camera]() { toggleSolo(camera); });
			tiles.emplace_back(std::move(tile));
		}
		rebuildCameraMenu(cameraNames);
		rebuildGrid();
	}

	void toggleSolo(const QString &camera)
	{
		soloCamera = soloCamera == camera ? QString() : camera;
		rebuildGrid();
	}

	void selectCamera(const QString &camera)
	{
		if (!lastSnapshot.available || !lastSnapshot.edit_mode)
			return;
		const QByteArray utf8 = camera.toUtf8();
		sr_event_dock_editor_select_camera(utf8.constData());
		forceDecode = true;
	}

	int targetHeight() const
	{
		const int configured = quality->currentData().toInt();
		if (configured)
			return configured;
		const int count = (int)visibleTiles().size();
		return count <= 2 ? 720 : count <= 4 ? 540 : 360;
	}

	int previewFps() const
	{
		const int configured = fps->currentData().toInt();
		if (configured)
			return configured;
		const int count = (int)visibleTiles().size();
		return count <= 2 ? 25 : count <= 4 ? 20 : 15;
	}

	void setControlsEnabled(bool enabled)
	{
		for (QWidget *widget : {static_cast<QWidget *>(autoAngle), playPause, playFromIn, gotoIn, setIn, setOut,
					gotoOut, prevFrame, nextFrame, loop, fit, live})
			widget->setEnabled(enabled);
		timeline->setEnabled(enabled);
	}

	void updateTileState(const sr_event_editor_snapshot &snapshot)
	{
		const QString selected = QString::fromUtf8(snapshot.selected_camera);
		const QString preview = QString::fromUtf8(snapshot.preview_camera);
		for (const auto &tile : tiles) {
			tile->setSelected(!selected.isEmpty() && tile->camera() == selected);
			tile->setPreview(!preview.isEmpty() && tile->camera() == preview);
			sr_replay_coverage_info coverage = {};
			const QByteArray camera = tile->camera().toUtf8();
			if (!snapshot.available ||
			    !sr_replay_coverage_query(camera.constData(), snapshot.in_ns, snapshot.out_ns, &coverage))
				coverage.coverage = SR_REPLAY_COVERAGE_NONE;
			const bool atCursor = coverage.coverage != SR_REPLAY_COVERAGE_NONE &&
					      snapshot.playhead_ns >= coverage.playable_in_ns &&
					      snapshot.playhead_ns <= coverage.playable_out_ns;
			tile->setCoverage(coverage.coverage, atCursor);
		}
	}

	void requestFrames(const sr_event_editor_snapshot &snapshot, bool immediate)
	{
		if (!isVisible() || !snapshot.available || !snapshot.edit_mode)
			return;
		char *sessionRaw = sr_session_get_or_create_path();
		if (!sessionRaw)
			return;
		const QString session = QString::fromUtf8(sessionRaw);
		bfree(sessionRaw);
		const int height = targetHeight();
		const int fpsLimit = std::max(1, previewFps());
		const qint64 nowMs = clock.elapsed();
		const QString selected = QString::fromUtf8(snapshot.selected_camera);

		for (const auto &tile : tiles) {
			if (hiddenCameras.contains(tile->camera()) ||
			    (!soloCamera.isEmpty() && tile->camera() != soloCamera))
				continue;
			sr_replay_coverage_info coverage = {};
			const QByteArray camera = tile->camera().toUtf8();
			if (!sr_replay_coverage_query(camera.constData(), snapshot.in_ns, snapshot.out_ns, &coverage) ||
			    coverage.coverage == SR_REPLAY_COVERAGE_NONE ||
			    snapshot.playhead_ns < coverage.playable_in_ns ||
			    snapshot.playhead_ns > coverage.playable_out_ns)
				continue;

			tile->decoder().setSource(session, tile->camera());
			const bool primary = !selected.isEmpty() && tile->camera() == selected;
			const int effectiveFps = primary ? std::max(fpsLimit, 25) : fpsLimit;
			const qint64 minimumMs = std::max<qint64>(1, 1000 / effectiveFps);
			qint64 &last = lastRequestMs[tile->camera()];
			if (!immediate && nowMs - last < minimumMs)
				continue;
			last = nowMs;
			const uint64_t cameraTimestamp = addSignedOffset(snapshot.playhead_ns, coverage.sync_offset_ns);
			tile->decoder().request(cameraTimestamp, height);
		}
	}

	void collectFrames(const sr_event_editor_snapshot &snapshot)
	{
		for (const auto &tile : tiles) {
			QImage image;
			uint64_t actualNs = 0;
			bool success = false;
			if (!tile->decoder().takeImage(&image, &actualNs, &success))
				continue;
			if (!success) {
				tile->setDecodeFailed();
				continue;
			}
			const uint64_t relative =
				actualNs > snapshot.record_start_ns ? actualNs - snapshot.record_start_ns : 0;
			tile->setImage(image, relative);
		}
	}

	void tick()
	{
		sr_event_editor_snapshot snapshot = {};
		if (!sr_event_dock_get_editor_snapshot(&snapshot)) {
			stateLabel->setText(T("Multiview.EditorUnavailable"));
			setControlsEnabled(false);
			return;
		}
		lastSnapshot = snapshot;
		if (!snapshot.available) {
			stateLabel->setText(T("Multiview.SelectEvent"));
			setControlsEnabled(false);
			return;
		}
		if (!snapshot.edit_mode) {
			stateLabel->setText(T("Multiview.PlayoutLocked"));
			setControlsEnabled(false);
			return;
		}

		setControlsEnabled(true);
		{
			const bool blocked = loop->blockSignals(true);
			loop->setChecked(snapshot.loop);
			loop->blockSignals(blocked);
		}
		stateLabel->setText(QStringLiteral("Event #%1 · %2")
					    .arg(snapshot.event_id)
					    .arg(snapshot.selected_camera[0]
							 ? QString::fromUtf8(snapshot.selected_camera)
							 : T("Multiview.AutoAngle")));
		const uint64_t relative = snapshot.playhead_ns > snapshot.record_start_ns
						  ? snapshot.playhead_ns - snapshot.record_start_ns
						  : 0;
		timeLabel->setText(QStringLiteral("CUR %1 · IN %2 · OUT %3")
					   .arg(clockText(relative))
					   .arg(clockText(snapshot.in_ns > snapshot.record_start_ns
								  ? snapshot.in_ns - snapshot.record_start_ns
								  : 0))
					   .arg(clockText(snapshot.out_ns > snapshot.record_start_ns
								  ? snapshot.out_ns - snapshot.record_start_ns
								  : 0)));
		timeline->setState(snapshot);
		updateTileState(snapshot);

		const bool eventChanged = snapshot.event_id != lastEventId;
		const bool cursorChanged = snapshot.playhead_ns != lastPlayheadNs;
		const bool immediate = forceDecode || eventChanged || (!snapshot.playing && cursorChanged);
		if (immediate || (snapshot.playing && cursorChanged))
			requestFrames(snapshot, immediate);
		collectFrames(snapshot);
		lastEventId = snapshot.event_id;
		lastPlayheadNs = snapshot.playhead_ns;
		forceDecode = false;
	}

	sr_event_controller *controller = nullptr;
	QToolButton *cameraMenuButton = nullptr;
	QMenu *cameraMenu = nullptr;
	QComboBox *quality = nullptr;
	QComboBox *fps = nullptr;
	QPushButton *autoAngle = nullptr;
	QLabel *stateLabel = nullptr;
	QWidget *gridHost = nullptr;
	QGridLayout *grid = nullptr;
	SrMultiviewTimeline *timeline = nullptr;
	QPushButton *playPause = nullptr;
	QPushButton *playFromIn = nullptr;
	QPushButton *gotoIn = nullptr;
	QPushButton *gotoOut = nullptr;
	QPushButton *setIn = nullptr;
	QPushButton *setOut = nullptr;
	QPushButton *prevFrame = nullptr;
	QPushButton *nextFrame = nullptr;
	QPushButton *loop = nullptr;
	QPushButton *fit = nullptr;
	QPushButton *live = nullptr;
	QLabel *timeLabel = nullptr;
	QTimer *cameraTimer = nullptr;
	QTimer *uiTimer = nullptr;
	QElapsedTimer clock;
	QStringList cameraNames;
	QSet<QString> hiddenCameras;
	QString soloCamera;
	std::vector<std::unique_ptr<SrMultiviewTile>> tiles;
	QMap<QString, qint64> lastRequestMs;
	sr_event_editor_snapshot lastSnapshot = {};
	uint64_t lastEventId = 0;
	uint64_t lastPlayheadNs = 0;
	bool forceDecode = true;
};

QPointer<QWidget> g_multiview;

} // namespace

QWidget *sr_multiview_dock_create(struct sr_event_controller *controller, QWidget *parent)
{
	auto *dock = new SrMultiviewDock(controller, parent);
	g_multiview = dock;
	return dock;
}

void sr_multiview_dock_show(void)
{
	if (!g_multiview)
		return;
	QWidget *widget = g_multiview.data();
	QWidget *cursor = widget;
	QDockWidget *dock = nullptr;
	while (cursor && !dock) {
		dock = qobject_cast<QDockWidget *>(cursor);
		cursor = cursor->parentWidget();
	}
	if (dock) {
		dock->show();
		dock->raise();
	} else {
		widget->show();
		widget->raise();
	}
	widget->setFocus(Qt::OtherFocusReason);
}
