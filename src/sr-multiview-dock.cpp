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
#include "sr-gpu-video.h"
#include "sr-replay-coverage.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>

extern "C" {
#include <libavutil/frame.h>
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
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
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
#include <QWindow>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

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

static QSize previewPixelSize(const QWidget *widget)
{
	const qreal ratio = widget ? widget->devicePixelRatioF() : 1.0;
	return QSize(std::max(1, (int)std::lround(widget->width() * ratio)),
		     std::max(1, (int)std::lround(widget->height() * ratio)));
}

static bool qtToGsWindow(QWindow *window, gs_window &gsWindow)
{
	if (!window)
		return false;
#ifdef _WIN32
	gsWindow.hwnd = (HWND)window->winId();
#elif __APPLE__
	gsWindow.view = (id)window->winId();
#else
	if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
		return false;
	gsWindow.id = window->winId();
	gsWindow.display = obs_get_nix_platform_display();
#endif
	return true;
}

class SrMultiviewGpuDisplay : public QWidget {
public:
	explicit SrMultiviewGpuDisplay(QWidget *parent = nullptr) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
		setMinimumSize(160, 90);
		renderer = sr_gpu_renderer_create();
	}

	~SrMultiviewGpuDisplay() override
	{
		if (display) {
			obs_display_remove_draw_callback(display, drawCallback, this);
			obs_display_destroy(display);
			display = nullptr;
		}
		frameMutex.lock();
		AVFrame *old = frame;
		frame = nullptr;
		frameMutex.unlock();
		av_frame_free(&old);
		sr_gpu_renderer_destroy(renderer);
	}

	void setFrame(const AVFrame *next)
	{
		AVFrame *copy = next ? av_frame_clone(next) : nullptr;
		frameMutex.lock();
		AVFrame *old = frame;
		frame = copy;
		frameMutex.unlock();
		av_frame_free(&old);
	}

protected:
	QPaintEngine *paintEngine() const override { return nullptr; }

	void paintEvent(QPaintEvent *event) override
	{
		createDisplay();
		QWidget::paintEvent(event);
	}

	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		createDisplay();
		if (display)
			obs_display_set_enabled(display, true);
	}

	void hideEvent(QHideEvent *event) override
	{
		if (display)
			obs_display_set_enabled(display, false);
		QWidget::hideEvent(event);
	}

	void resizeEvent(QResizeEvent *event) override
	{
		QWidget::resizeEvent(event);
		createDisplay();
		if (display) {
			const QSize size = previewPixelSize(this);
			obs_display_resize(display, (uint32_t)size.width(), (uint32_t)size.height());
		}
	}

private:
	static void drawCallback(void *data, uint32_t cx, uint32_t cy)
	{
		static_cast<SrMultiviewGpuDisplay *>(data)->render(cx, cy);
	}

	void createDisplay()
	{
		if (display || !renderer || !windowHandle() || !windowHandle()->isExposed())
			return;
		const QSize size = previewPixelSize(this);
		gs_init_data info = {};
		info.cx = (uint32_t)size.width();
		info.cy = (uint32_t)size.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		if (!qtToGsWindow(windowHandle(), info.window))
			return;
		display = obs_display_create(&info, 0x00000000);
		if (display)
			obs_display_add_draw_callback(display, drawCallback, this);
	}

	void render(uint32_t cx, uint32_t cy)
	{
		if (!renderer || !cx || !cy)
			return;
		frameMutex.lock();
		AVFrame *current = frame ? av_frame_clone(frame) : nullptr;
		frameMutex.unlock();
		if (!current)
			return;

		const uint32_t sourceWidth = (uint32_t)std::max(current->width, 1);
		const uint32_t sourceHeight = (uint32_t)std::max(current->height, 1);
		const double scale = std::min((double)cx / sourceWidth, (double)cy / sourceHeight);
		const uint32_t drawWidth = std::max(1u, (uint32_t)std::lround(sourceWidth * scale));
		const uint32_t drawHeight = std::max(1u, (uint32_t)std::lround(sourceHeight * scale));
		const int x = ((int)cx - (int)drawWidth) / 2;
		const int y = ((int)cy - (int)drawHeight) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_matrix_push();
		gs_set_viewport(0, 0, (int)cx, (int)cy);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_matrix_translate3f((float)x, (float)y, 0.0f);

		/* Multiview is drawn directly into an obs_display, not through an
		 * OBS_SOURCE_SRGB source. Reusing the source-render linear-sRGB state here
		 * decompresses SDR twice and makes the tiles visibly brighter than Replay
		 * A. Force the direct display pass to stay nonlinear, then restore the
		 * caller's graphics state. */
		const bool previousLinearSrgb = gs_set_linear_srgb(false);
		sr_gpu_renderer_draw(renderer, current, drawWidth, drawHeight);
		gs_set_linear_srgb(previousLinearSrgb);

		gs_matrix_pop();
		gs_projection_pop();
		gs_viewport_pop();
		av_frame_free(&current);
	}

	obs_display_t *display = nullptr;
	struct sr_gpu_renderer *renderer = nullptr;
	std::mutex frameMutex;
	AVFrame *frame = nullptr;
};

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
		av_frame_free(&publishedFrame);
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

	void request(uint64_t timestampNs)
	{
		std::lock_guard<std::mutex> lock(mutex);
		requestedTimestampNs = timestampNs;
		requestSerial++;
		condition.notify_all();
	}

	bool takeFrame(AVFrame **outFrame, uint64_t *actualTimestampNs, bool *success)
	{
		if (!outFrame)
			return false;
		*outFrame = nullptr;
		std::lock_guard<std::mutex> lock(mutex);
		if (!publishedSerial || publishedSerial == consumedSerial)
			return false;
		consumedSerial = publishedSerial;
		*outFrame = publishedFrame ? av_frame_clone(publishedFrame) : nullptr;
		if (actualTimestampNs)
			*actualTimestampNs = publishedTimestampNs;
		if (success)
			*success = publishedSuccess && *outFrame;
		return true;
	}

private:
	void publish(uint64_t serial, bool success, uint64_t timestampNs, AVFrame *decoded)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (serial != requestSerial)
			return;
		AVFrame *copy = success && decoded ? av_frame_clone(decoded) : nullptr;
		av_frame_free(&publishedFrame);
		publishedFrame = copy;
		publishedSerial = serial;
		publishedSuccess = copy != nullptr;
		publishedTimestampNs = timestampNs;
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
				session = requestedSession;
				camera = requestedCamera;
			}

			if (session != openedSession || camera != openedCamera) {
				sr_disk_player_destroy(player);
				player = nullptr;
				openedSession = session;
				openedCamera = camera;
				if (!session.empty() && !camera.empty()) {
					player = sr_disk_player_create_with_cache(session.c_str(), camera.c_str(),
										  12ULL * 1024ULL * 1024ULL);
					if (player)
						sr_disk_player_set_hardware_decode(
							player, sr_gpu_multiview_hardware_decode_safe());
				}
			}

			bool ok = false;
			uint64_t actualNs = timestampNs;
			AVFrame *decoded = nullptr;
			if (player) {
				ok = sr_disk_player_decode_at(player, timestampNs, &decoded, &actualNs);
				if (!ok) {
					sr_disk_player_refresh(player);
					ok = sr_disk_player_decode_at(player, timestampNs, &decoded, &actualNs);
				}
			}
			publish(serial, ok, actualNs, decoded);
			av_frame_free(&decoded);
			handledSerial = serial;
		}

		sr_disk_player_destroy(player);
	}

	std::mutex mutex;
	std::condition_variable condition;
	std::thread worker;
	bool stopping = false;
	uint64_t requestSerial = 0;
	uint64_t requestedTimestampNs = 0;
	std::string requestedSession;
	std::string requestedCamera;
	uint64_t publishedSerial = 0;
	uint64_t consumedSerial = 0;
	uint64_t publishedTimestampNs = 0;
	bool publishedSuccess = false;
	AVFrame *publishedFrame = nullptr;
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
		videoBorder = new QFrame(this);
		videoBorder->setObjectName(QStringLiteral("multiviewVideoBorder"));
		auto *videoLayout = new QVBoxLayout(videoBorder);
		videoLayout->setContentsMargins(2, 2, 2, 2);
		videoLayout->setSpacing(0);
		picture = new SrMultiviewGpuDisplay(videoBorder);
		picture->setAttribute(Qt::WA_TransparentForMouseEvents);
		videoLayout->addWidget(picture);
		layout->addWidget(videoBorder, 1);
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
		/* Event coverage controls only the marker in the title. Preview
		 * availability is determined independently from the complete recording
		 * timeline so moving the editor playhead outside IN/OUT keeps showing the
		 * recorded camera frames. */
		if (!atPlayhead) {
			clearFrame();
			setMessage(T("Multiview.NoMediaAtCursor"));
		} else if (coverage == SR_REPLAY_COVERAGE_NONE && !haveFrame) {
			setMessage(T("Multiview.NoCoverage"));
		}
	}

	void setFrame(const AVFrame *frame, uint64_t relativeTimestampNs)
	{
		if (!frame)
			return;
		haveFrame = true;
		picture->setFrame(frame);
		footer->setText(clockText(relativeTimestampNs));
	}

	void clearFrame()
	{
		haveFrame = false;
		if (picture)
			picture->setFrame(nullptr);
	}

	void setDecodeFailed() { setMessage(T("Multiview.DecodeWaiting")); }

	void setMessage(const QString &message) { footer->setText(message); }

protected:
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
		const QString tileBorder = selected  ? QStringLiteral("#00bfe8")
					   : preview ? QStringLiteral("#d3a11f")
						     : QStringLiteral("#555");
		const QString imageBorder = selected  ? QStringLiteral("#00e5ff")
					    : preview ? QStringLiteral("#e1a91c")
						      : QStringLiteral("#303030");
		const int imageWidth = selected ? 4 : preview ? 2 : 1;
		setStyleSheet(
			QStringLiteral("SrMultiviewTile { border: 2px solid %1; border-radius: 4px; } "
				       "QFrame#multiviewVideoBorder { border: %2px solid %3; background: #080808; }")
				.arg(tileBorder)
				.arg(imageWidth)
				.arg(imageBorder));
		updateTitle();
	}

	QString cameraName;
	QLabel *title = nullptr;
	QFrame *videoBorder = nullptr;
	SrMultiviewGpuDisplay *picture = nullptr;
	QLabel *footer = nullptr;
	bool haveFrame = false;
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
		const bool eventChanged = state.event_id && state.event_id != focusedEventId;
		if (!state.event_id)
			focusedEventId = 0;
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
			if (eventChanged && haveRange)
				focusRange(inNs, outNs);
		}
		if (state.event_id)
			focusedEventId = state.event_id;
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
		if (!span || area.width() <= 0)
			return;

		const QRect rulerBand(area.left(), 0, area.width(), area.top());
		painter.fillRect(rulerBand, palette().alternateBase());
		painter.setPen(palette().mid().color());
		painter.drawLine(area.left(), area.top() - 1, area.right(), area.top() - 1);

		/* Select the major step by pixel density, not just tick count. This keeps
		 * labels readable from sub-second edits through multi-hour sessions. */
		const uint64_t candidates[] = {100000000ULL,      250000000ULL,      500000000ULL,     1000000000ULL,
					       2000000000ULL,     5000000000ULL,     10000000000ULL,   30000000000ULL,
					       60000000000ULL,    120000000000ULL,   300000000000ULL,  600000000000ULL,
					       900000000000ULL,   1800000000000ULL,  3600000000000ULL, 7200000000000ULL,
					       21600000000000ULL, 43200000000000ULL, 86400000000000ULL};
		uint64_t step = candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
		for (uint64_t candidate : candidates) {
			const long double pixels = (long double)area.width() * candidate / (long double)span;
			if (pixels >= 88.0L) {
				step = candidate;
				break;
			}
		}

		const uint64_t relativeStart = viewStartNs > recordStartNs ? viewStartNs - recordStartNs : 0;
		uint64_t firstRelative = (relativeStart / step) * step;
		if (firstRelative < relativeStart && firstRelative <= UINT64_MAX - step)
			firstRelative += step;
		if (firstRelative > UINT64_MAX - recordStartNs)
			return;
		const uint64_t first = recordStartNs + firstRelative;

		/* Minor ticks make the scale useful while scrubbing even when the next
		 * labelled major mark is relatively far away. */
		const uint64_t minorStep = step / 4;
		if (minorStep) {
			uint64_t firstMinorRelative = (relativeStart / minorStep) * minorStep;
			if (firstMinorRelative < relativeStart && firstMinorRelative <= UINT64_MAX - minorStep)
				firstMinorRelative += minorStep;
			if (firstMinorRelative <= UINT64_MAX - recordStartNs) {
				for (uint64_t relative = firstMinorRelative; relative <= viewEndNs - recordStartNs;) {
					if (relative % step != 0) {
						const int x = xFromTimestamp(recordStartNs + relative);
						painter.drawLine(x, area.top() - 4, x, area.top() - 1);
					}
					if (relative > UINT64_MAX - minorStep)
						break;
					relative += minorStep;
				}
			}
		}

		const QRect zoomReserved(area.right() - 86, 0, 86, area.top());
		for (uint64_t t = first; t <= viewEndNs;) {
			const int x = xFromTimestamp(t);
			painter.setPen(palette().mid().color());
			painter.drawLine(x, area.top() - 8, x, area.top() - 1);
			const uint64_t relative = t > recordStartNs ? t - recordStartNs : 0;
			const QString labelText = clockText(relative);
			const int labelWidth = std::max(76, painter.fontMetrics().horizontalAdvance(labelText) + 10);
			const QRect label(x - labelWidth / 2, 1, labelWidth, std::max(12, area.top() - 8));
			if (label.right() >= area.left() && label.left() <= area.right() &&
			    !label.intersects(zoomReserved)) {
				painter.setPen(palette().text().color());
				painter.drawText(label, Qt::AlignHCenter | Qt::AlignTop, labelText);
			}
			if (t > UINT64_MAX - step)
				break;
			t += step;
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

	void focusRange(uint64_t rangeInNs, uint64_t rangeOutNs)
	{
		if (!haveRecording || rangeOutNs <= rangeInNs || recordEndNs <= recordStartNs)
			return;
		rangeInNs = clamp(rangeInNs);
		rangeOutNs = clamp(rangeOutNs);
		if (rangeOutNs <= rangeInNs)
			return;

		const uint64_t total = recordEndNs - recordStartNs;
		const uint64_t rangeSpan = rangeOutNs - rangeInNs;
		uint64_t span = (uint64_t)std::ceil((long double)rangeSpan / 0.60L);
		span = std::max<uint64_t>(250000000ULL, std::max<uint64_t>(rangeSpan, span));
		span = std::min<uint64_t>(span, total);
		if (span >= total) {
			zoomLocked = false;
			viewStartNs = recordStartNs;
			viewEndNs = recordEndNs;
			return;
		}

		const uint64_t center = rangeInNs + rangeSpan / 2;
		zoomLocked = true;
		viewStartNs = center > span / 2 ? center - span / 2 : recordStartNs;
		viewEndNs = viewStartNs <= UINT64_MAX - span ? viewStartNs + span : recordEndNs;
		clampView();
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
	uint64_t focusedEventId = 0;
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
		for (const auto &tile : tiles) {
			tile->decoder().setSource(QString(), QString());
			tile->clearFrame();
		}
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
				tile->clearFrame();
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
		QWidget *controls[] = {autoAngle, playPause, playFromIn, gotoIn, setIn, setOut,
				       gotoOut,   prevFrame, nextFrame,  loop,   fit,   live};
		for (QWidget *widget : controls)
			widget->setEnabled(enabled);
		timeline->setEnabled(enabled);
	}

	void updateTileState(const sr_event_editor_snapshot &snapshot)
	{
		const QString selected = QString::fromUtf8(snapshot.selected_camera);
		const QString preview = QString::fromUtf8(snapshot.preview_camera);
		cursorAvailable.clear();
		cursorOffsets.clear();

		for (const auto &tile : tiles) {
			tile->setSelected(!selected.isEmpty() && tile->camera() == selected);
			tile->setPreview(!preview.isEmpty() && tile->camera() == preview);

			const QByteArray camera = tile->camera().toUtf8();
			sr_replay_coverage_info eventCoverage = {};
			if (!snapshot.available || !sr_replay_coverage_query(camera.constData(), snapshot.in_ns,
									     snapshot.out_ns, &eventCoverage))
				eventCoverage.coverage = SR_REPLAY_COVERAGE_NONE;

			/* The IN/OUT range describes the event, but the operator may scrub the
			 * complete session before/after that range to choose better marks. Query
			 * the interval that contains the current cursor across the full recording
			 * and use it only for preview availability/sync. */
			sr_replay_coverage_info cursorCoverage = {};
			const bool atCursor = snapshot.record_end_ns >= snapshot.record_start_ns &&
					      sr_replay_coverage_query_at(camera.constData(), snapshot.record_start_ns,
									  snapshot.record_end_ns, snapshot.playhead_ns,
									  &cursorCoverage) &&
					      cursorCoverage.coverage != SR_REPLAY_COVERAGE_NONE;
			if (atCursor) {
				cursorAvailable.insert(tile->camera());
				cursorOffsets.insert(tile->camera(), (qint64)cursorCoverage.sync_offset_ns);
			}

			tile->setCoverage(eventCoverage.coverage, atCursor);
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
		const int fpsLimit = std::max(1, previewFps());
		const qint64 nowMs = clock.elapsed();
		const QString selected = QString::fromUtf8(snapshot.selected_camera);

		for (const auto &tile : tiles) {
			if (hiddenCameras.contains(tile->camera()) ||
			    (!soloCamera.isEmpty() && tile->camera() != soloCamera))
				continue;
			if (!cursorAvailable.contains(tile->camera()))
				continue;

			tile->decoder().setSource(session, tile->camera());
			const bool primary = !selected.isEmpty() && tile->camera() == selected;
			const int effectiveFps = primary ? std::max(fpsLimit, 25) : fpsLimit;
			const qint64 minimumMs = std::max<qint64>(1, 1000 / effectiveFps);
			qint64 &last = lastRequestMs[tile->camera()];
			if (!immediate && nowMs - last < minimumMs)
				continue;
			last = nowMs;
			const int64_t syncOffset = (int64_t)cursorOffsets.value(tile->camera(), 0);
			const uint64_t cameraTimestamp = addSignedOffset(snapshot.playhead_ns, syncOffset);
			tile->decoder().request(cameraTimestamp);
		}
	}

	void collectFrames(const sr_event_editor_snapshot &snapshot)
	{
		for (const auto &tile : tiles) {
			AVFrame *frame = nullptr;
			uint64_t actualNs = 0;
			bool success = false;
			if (!tile->decoder().takeFrame(&frame, &actualNs, &success))
				continue;
			if (!success || !frame) {
				av_frame_free(&frame);
				tile->setDecodeFailed();
				continue;
			}
			const uint64_t relative =
				actualNs > snapshot.record_start_ns ? actualNs - snapshot.record_start_ns : 0;
			tile->setFrame(frame, relative);
			av_frame_free(&frame);
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

	QToolButton *cameraMenuButton = nullptr;
	QMenu *cameraMenu = nullptr;
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
	QSet<QString> cursorAvailable;
	QMap<QString, qint64> cursorOffsets;
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
