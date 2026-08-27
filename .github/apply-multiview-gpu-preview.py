from pathlib import Path
import re

path = Path('src/sr-multiview-dock.cpp')
text = path.read_text(encoding='utf-8')


def replace_once(old: str, new: str, label: str):
    global text
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'{label}: expected one match, found {count}')
    text = text.replace(old, new, 1)


replace_once('#include "sr-event-dock.h"\n', '#include "sr-event-dock.h"\n#include "sr-gpu-video.h"\n', 'gpu header include')

replace_once('#include <QWidget>\n', '#include <QWidget>\n#include <QWindow>\n\n#if !defined(_WIN32) && !defined(__APPLE__)\n#include <obs-nix-platform.h>\n#endif\n\n#ifdef _WIN32\n#define WIN32_LEAN_AND_MEAN\n#include <Windows.h>\n#endif\n', 'Qt/native window includes')

anchor = 'class SrMultiviewDecoder {\n'
helper = r'''static QSize previewPixelSize(const QWidget *widget)
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
		pthread_mutex_lock(&frameMutex);
		AVFrame *old = frame;
		frame = nullptr;
		pthread_mutex_unlock(&frameMutex);
		av_frame_free(&old);
		sr_gpu_renderer_destroy(renderer);
		pthread_mutex_destroy(&frameMutex);
	}

	void setFrame(const AVFrame *next)
	{
		AVFrame *copy = next ? av_frame_clone(next) : nullptr;
		pthread_mutex_lock(&frameMutex);
		AVFrame *old = frame;
		frame = copy;
		pthread_mutex_unlock(&frameMutex);
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
		pthread_mutex_lock(&frameMutex);
		AVFrame *current = frame ? av_frame_clone(frame) : nullptr;
		pthread_mutex_unlock(&frameMutex);
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
		sr_gpu_renderer_draw(renderer, current, drawWidth, drawHeight);
		gs_matrix_pop();
		gs_projection_pop();
		gs_viewport_pop();
		av_frame_free(&current);
	}

	obs_display_t *display = nullptr;
	struct sr_gpu_renderer *renderer = nullptr;
	pthread_mutex_t frameMutex = PTHREAD_MUTEX_INITIALIZER;
	AVFrame *frame = nullptr;
};

''' + anchor
replace_once(anchor, helper, 'GPU preview display class')

# Replace decoder class wholesale, preserving the tile class that follows.
pattern = re.compile(r'class SrMultiviewDecoder \{.*?\n\};\n\nclass SrMultiviewTile', re.S)
match = pattern.search(text)
if not match:
    raise RuntimeError('decoder class not found')
new_decoder = r'''class SrMultiviewDecoder {
public:
	SrMultiviewDecoder() { worker = std::thread([this]() { run(); }); }

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

	void request(uint64_t timestampNs, int targetHeight)
	{
		UNUSED_PARAMETER(targetHeight);
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
				condition.wait(lock, [this, handledSerial]() { return stopping || requestSerial != handledSerial; });
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
				if (!session.empty() && !camera.empty())
					player = sr_disk_player_create_with_cache(session.c_str(), camera.c_str(),
										  12ULL * 1024ULL * 1024ULL);
			}

			bool ok = false;
			uint64_t actualNs = timestampNs;
			AVFrame *decoded = nullptr;
			if (player && timestampNs) {
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

class SrMultiviewTile'''
text = text[:match.start()] + new_decoder + text[match.end():]

# Tile video surface: native OBS display inside a styled border container.
old = '''\t\tpicture = new QLabel(this);\n\t\tpicture->setAlignment(Qt::AlignCenter);\n\t\tpicture->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);\n\t\tpicture->setMinimumSize(160, 90);\n\t\tpicture->setStyleSheet(QStringLiteral("background: #080808; color: #aaa;"));\n\t\tpicture->setText(T("Multiview.Waiting"));\n\t\tpicture->setAttribute(Qt::WA_TransparentForMouseEvents);\n\t\tlayout->addWidget(picture, 1);\n'''.replace('\\t','\t').replace('\\n','\n')
new = '''\t\tvideoBorder = new QFrame(this);\n\t\tvideoBorder->setObjectName(QStringLiteral("multiviewVideoBorder"));\n\t\tauto *videoLayout = new QVBoxLayout(videoBorder);\n\t\tvideoLayout->setContentsMargins(2, 2, 2, 2);\n\t\tvideoLayout->setSpacing(0);\n\t\tpicture = new SrMultiviewGpuDisplay(videoBorder);\n\t\tpicture->setAttribute(Qt::WA_TransparentForMouseEvents);\n\t\tvideoLayout->addWidget(picture);\n\t\tlayout->addWidget(videoBorder, 1);\n'''.replace('\\t','\t').replace('\\n','\n')
replace_once(old, new, 'tile GPU video surface')

# Replace setImage + setMessage + resize handler and remove pixmap scaler.
text = re.sub(r'\tvoid setImage\(const QImage &image, uint64_t relativeTimestampNs\)\n\t\{.*?\n\t\}\n\n\tvoid setDecodeFailed\(\) \{ setMessage\(T\("Multiview.DecodeWaiting"\)\); \}\n\n\tvoid setMessage\(const QString &message\)\n\t\{.*?\n\t\}\n',
'''\tvoid setFrame(const AVFrame *frame, uint64_t relativeTimestampNs)\n\t{\n\t\tif (!frame)\n\t\t\treturn;\n\t\thaveFrame = true;\n\t\tpicture->setFrame(frame);\n\t\tfooter->setText(clockText(relativeTimestampNs));\n\t}\n\n\tvoid clearFrame()\n\t{\n\t\thaveFrame = false;\n\t\tif (picture)\n\t\t\tpicture->setFrame(nullptr);\n\t}\n\n\tvoid setDecodeFailed() { setMessage(T("Multiview.DecodeWaiting")); }\n\n\tvoid setMessage(const QString &message)\n\t{\n\t\tfooter->setText(message);\n\t}\n''', text, count=1, flags=re.S)

text = re.sub(r'\tvoid resizeEvent\(QResizeEvent \*event\) override\n\t\{.*?\n\t\}\n\n', '', text, count=1, flags=re.S)
text = re.sub(r'\tvoid refreshPixmap\(\)\n\t\{.*?\n\t\}\n\n', '', text, count=1, flags=re.S)

old_style = '''\tvoid updateStyle()\n\t{\n\t\tQString border = selected  ? QStringLiteral("#2f83ff")\n\t\t\t\t : preview ? QStringLiteral("#d3a11f")\n\t\t\t\t\t   : QStringLiteral("#555");\n\t\tsetStyleSheet(\n\t\t\tQStringLiteral("SrMultiviewTile { border: 2px solid %1; border-radius: 4px; }").arg(border));\n\t\tupdateTitle();\n\t}\n'''.replace('\\t','\t').replace('\\n','\n')
new_style = '''\tvoid updateStyle()\n\t{\n\t\tconst QString tileBorder = selected  ? QStringLiteral("#00bfe8")\n\t\t\t\t\t: preview ? QStringLiteral("#d3a11f")\n\t\t\t\t\t\t  : QStringLiteral("#555");\n\t\tconst QString imageBorder = selected  ? QStringLiteral("#00e5ff")\n\t\t\t\t\t : preview ? QStringLiteral("#e1a91c")\n\t\t\t\t\t\t   : QStringLiteral("#303030");\n\t\tconst int imageWidth = selected ? 4 : preview ? 2 : 1;\n\t\tsetStyleSheet(QStringLiteral(\n\t\t\t\t  "SrMultiviewTile { border: 2px solid %1; border-radius: 4px; } "\n\t\t\t\t  "QFrame#multiviewVideoBorder { border: %2px solid %3; background: #080808; }")\n\t\t\t\t  .arg(tileBorder)\n\t\t\t\t  .arg(imageWidth)\n\t\t\t\t  .arg(imageBorder));\n\t\tupdateTitle();\n\t}\n'''.replace('\\t','\t').replace('\\n','\n')
replace_once(old_style, new_style, 'bright selected video border')

replace_once('\tQLabel *picture = nullptr;\n\tQLabel *footer = nullptr;\n\tQImage lastImage;\n',
             '\tQFrame *videoBorder = nullptr;\n\tSrMultiviewGpuDisplay *picture = nullptr;\n\tQLabel *footer = nullptr;\n\tbool haveFrame = false;\n', 'tile fields')

# collectFrames now forwards AVFrame refs directly to the GPU display.
old_collect = '''\tvoid collectFrames(const sr_event_editor_snapshot &snapshot)\n\t{\n\t\tfor (const auto &tile : tiles) {\n\t\t\tQImage image;\n\t\t\tuint64_t actualNs = 0;\n\t\t\tbool success = false;\n\t\t\tif (!tile->decoder().takeImage(&image, &actualNs, &success))\n\t\t\t\tcontinue;\n\t\t\tif (!success) {\n\t\t\t\ttile->setDecodeFailed();\n\t\t\t\tcontinue;\n\t\t\t}\n\t\t\tconst uint64_t relative =\n\t\t\t\tactualNs > snapshot.record_start_ns ? actualNs - snapshot.record_start_ns : 0;\n\t\t\ttile->setImage(image, relative);\n\t\t}\n\t}\n'''.replace('\\t','\t').replace('\\n','\n')
new_collect = '''\tvoid collectFrames(const sr_event_editor_snapshot &snapshot)\n\t{\n\t\tfor (const auto &tile : tiles) {\n\t\t\tAVFrame *frame = nullptr;\n\t\t\tuint64_t actualNs = 0;\n\t\t\tbool success = false;\n\t\t\tif (!tile->decoder().takeFrame(&frame, &actualNs, &success))\n\t\t\t\tcontinue;\n\t\t\tif (!success || !frame) {\n\t\t\t\tav_frame_free(&frame);\n\t\t\t\ttile->setDecodeFailed();\n\t\t\t\tcontinue;\n\t\t\t}\n\t\t\tconst uint64_t relative =\n\t\t\t\tactualNs > snapshot.record_start_ns ? actualNs - snapshot.record_start_ns : 0;\n\t\t\ttile->setFrame(frame, relative);\n\t\t\tav_frame_free(&frame);\n\t\t}\n\t}\n'''.replace('\\t','\t').replace('\\n','\n')
replace_once(old_collect, new_collect, 'GPU frame collection')

# Release native frame refs when the dock/tile is hidden.
replace_once('\t\tfor (const auto &tile : tiles)\n\t\t\ttile->decoder().setSource(QString(), QString());\n',
             '\t\tfor (const auto &tile : tiles) {\n\t\t\ttile->decoder().setSource(QString(), QString());\n\t\t\ttile->clearFrame();\n\t\t}\n', 'dock hide GPU release')
replace_once('\t\t\t\ttile->setVisible(false);\n\t\t\t\ttile->decoder().setSource(QString(), QString());\n',
             '\t\t\t\ttile->setVisible(false);\n\t\t\t\ttile->decoder().setSource(QString(), QString());\n\t\t\t\ttile->clearFrame();\n', 'hidden tile GPU release')

# QImage/QPixmap and direct swscale are no longer part of Multiview.
text = text.replace('#include <libavutil/hwcontext.h>\n#include <libswscale/swscale.h>\n', '')
text = text.replace('#include <QImage>\n', '')
text = text.replace('#include <QPixmap>\n', '')

path.write_text(text, encoding='utf-8')
