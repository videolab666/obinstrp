from pathlib import Path

p = Path('src/sr-event-dock.cpp')
text = p.read_text(encoding='utf-8')

def rep(old, new, count=-1):
    global text
    if old not in text:
        raise SystemExit(f'missing anchor: {old[:120]!r}')
    text = text.replace(old, new, count)

rep('#include <QStandardPaths>\n#include <QStyle>\n', '#include <QStandardPaths>\n#include <QStyledItemDelegate>\n#include <QStyle>\n')

rep('constexpr int EVENT_THUMB_WIDTH = 192;\nconstexpr int EVENT_THUMB_HEIGHT = 108;\nconstexpr size_t EVENT_THUMB_BATCH = 24;\n', '''constexpr int EVENT_THUMB_WIDTH = 192;
constexpr int EVENT_THUMB_HEIGHT = 108;
constexpr int EVENT_THUMB_DISPLAY_WIDTH = 176;
constexpr int EVENT_THUMB_DISPLAY_HEIGHT = 99;
constexpr int EVENT_THUMB_CELL_WIDTH = 184;
constexpr int EVENT_THUMB_CELL_HEIGHT = 142;
constexpr size_t EVENT_THUMB_BATCH = 24;
constexpr size_t ANGLE_PREVIEW_CACHE_EVENTS = 12;

class SrEventThumbnailDelegate : public QStyledItemDelegate {
public:
\texplicit SrEventThumbnailDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

\tQSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
\t{
\t\treturn QSize(EVENT_THUMB_CELL_WIDTH, EVENT_THUMB_CELL_HEIGHT);
\t}

\tvoid paint(QPainter *painter, const QStyleOptionViewItem &option,
\t\t   const QModelIndex &index) const override
\t{
\t\tQStyleOptionViewItem itemOption(option);
\t\tinitStyleOption(&itemOption, index);
\t\tconst QIcon icon = itemOption.icon;
\t\tconst QString text = itemOption.text;

\t\titemOption.icon = QIcon();
\t\titemOption.text.clear();
\t\tQStyle *style = itemOption.widget ? itemOption.widget->style() : QApplication::style();
\t\tstyle->drawControl(QStyle::CE_ItemViewItem, &itemOption, painter, itemOption.widget);

\t\tconst QRect content = option.rect.adjusted(4, 3, -4, -3);
\t\tconst QRect imageBox(content.left(), content.top(), content.width(), EVENT_THUMB_DISPLAY_HEIGHT);
\t\tif (!icon.isNull()) {
\t\t\tconst QIcon::Mode mode = !(option.state & QStyle::State_Enabled)
\t\t\t\t\t\t ? QIcon::Disabled
\t\t\t\t\t\t : (option.state & QStyle::State_Selected) ? QIcon::Selected
\t\t\t\t\t\t\t\t\t\t\t     : QIcon::Normal;
\t\t\tQPixmap pixmap = icon.pixmap(QSize(EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT), mode);
\t\t\tif (!pixmap.isNull()) {
\t\t\t\tQSize drawSize = pixmap.size();
\t\t\t\tdrawSize.scale(QSize(EVENT_THUMB_DISPLAY_WIDTH, EVENT_THUMB_DISPLAY_HEIGHT),
\t\t\t\t\t       Qt::KeepAspectRatio);
\t\t\t\tQRect target(QPoint(0, 0), drawSize);
\t\t\t\ttarget.moveCenter(imageBox.center());
\t\t\t\tpainter->save();
\t\t\t\tpainter->setRenderHint(QPainter::SmoothPixmapTransform, true);
\t\t\t\tpainter->drawPixmap(target, pixmap);
\t\t\t\tpainter->restore();
\t\t\t}
\t\t}

\t\tconst QRect textRect(content.left(), imageBox.bottom() + 4, content.width(),
\t\t\t\t       std::max(0, content.bottom() - imageBox.bottom() - 3));
\t\tpainter->save();
\t\tpainter->setPen((option.state & QStyle::State_Selected) ? option.palette.highlightedText().color()
\t\t\t\t\t\t\t\t\t    : option.palette.text().color());
\t\tpainter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, text);
\t\tpainter->restore();
\t}
};
''')

rep('''\t\tthumbnailList->setIconSize(QSize(EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT));
\t\tthumbnailList->setGridSize(QSize(220, 164));
\t\tthumbnailList->setSpacing(4);
\t\tthumbnailList->setUniformItemSizes(true);
''', '''\t\tthumbnailList->setIconSize(QSize(EVENT_THUMB_DISPLAY_WIDTH, EVENT_THUMB_DISPLAY_HEIGHT));
\t\tthumbnailList->setGridSize(QSize(EVENT_THUMB_CELL_WIDTH, EVENT_THUMB_CELL_HEIGHT));
\t\tthumbnailList->setSpacing(1);
\t\tthumbnailList->setUniformItemSizes(true);
\t\tthumbnailList->setItemDelegate(new SrEventThumbnailDelegate(thumbnailList));
''')

rep('''\t\t\tbutton->setMinimumSize(210, 132);
\t\t\tbutton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
''', '''\t\t\tbutton->setMinimumWidth(184);
\t\t\tbutton->setFixedHeight(132);
\t\t\tbutton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
''')

old_request = '''\tvoid requestAnglePreviews(uint64_t eventId, uint64_t timestampNs)
\t{
\t\tif (previewTargetEventId != eventId) {
\t\t\tpreviewTargetEventId = eventId;
\t\t\tpreviewLoadedEventId = 0;
\t\t\tfor (QToolButton *button : angleButtons)
\t\t\t\tbutton->setIcon(QIcon());
\t\t}
\t\tif (!eventId || previewLoadedEventId == eventId || anglePreviewJob)
\t\t\treturn;

\t\tchar *sessionPath = sr_session_get_or_create_path();
\t\tif (!sessionPath)
\t\t\treturn;
\t\tauto job = std::make_unique<AnglePreviewJob>();
\t\tjob->eventId = eventId;
\t\tjob->sessionDir = sessionPath;
\t\tbfree(sessionPath);
\t\tfor (QToolButton *button : angleButtons) {
\t\t\tif (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE)
\t\t\t\tcontinue;
\t\t\tconst qint64 offset = button->property("syncOffsetNs").toLongLong();
\t\t\tuint64_t cameraTimestamp = timestampNs;
\t\t\tif (offset >= 0 && (uint64_t)offset <= UINT64_MAX - cameraTimestamp)
\t\t\t\tcameraTimestamp += (uint64_t)offset;
\t\t\telse if (offset < 0 && (uint64_t)(-offset) < cameraTimestamp)
\t\t\t\tcameraTimestamp -= (uint64_t)(-offset);
\t\t\tAnglePreviewTask task;
\t\t\ttask.camera = button->property("cameraName").toString().toUtf8().constData();
\t\t\ttask.timestampNs = cameraTimestamp;
\t\t\tjob->tasks.emplace_back(std::move(task));
\t\t}
\t\tif (job->tasks.empty())
\t\t\treturn;
\t\tanglePreviewJob = std::move(job);
\t\tAnglePreviewJob *workerJob = anglePreviewJob.get();
\t\tworkerJob->worker = std::thread([workerJob]() { runAnglePreviewJob(workerJob); });
\t}
'''
new_request = '''\tvoid requestAnglePreviews(uint64_t eventId, uint64_t timestampNs)
\t{
\t\tif (previewTargetEventId != eventId) {
\t\t\tpreviewTargetEventId = eventId;
\t\t\tpreviewLoadedEventId = 0;
\t\t}
\t\tif (!eventId)
\t\t\treturn;

\t\tauto cacheIt = anglePreviewCache.find(eventId);
\t\tbool allCached = true;
\t\tfor (QToolButton *button : angleButtons) {
\t\t\tif (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE)
\t\t\t\tcontinue;
\t\t\tconst std::string camera = button->property("cameraName").toString().toUtf8().constData();
\t\t\tif (cacheIt != anglePreviewCache.end()) {
\t\t\t\tauto iconIt = cacheIt->second.find(camera);
\t\t\t\tif (iconIt != cacheIt->second.end() && !iconIt->second.isNull()) {
\t\t\t\t\tbutton->setIcon(iconIt->second);
\t\t\t\t\tcontinue;
\t\t\t\t}
\t\t\t}
\t\t\tallCached = false;
\t\t}
\t\tif (allCached) {
\t\t\tpreviewLoadedEventId = eventId;
\t\t\treturn;
\t\t}
\t\tif (previewLoadedEventId == eventId || anglePreviewJob)
\t\t\treturn;

\t\tchar *sessionPath = sr_session_get_or_create_path();
\t\tif (!sessionPath)
\t\t\treturn;
\t\tauto job = std::make_unique<AnglePreviewJob>();
\t\tjob->eventId = eventId;
\t\tjob->sessionDir = sessionPath;
\t\tbfree(sessionPath);
\t\tfor (QToolButton *button : angleButtons) {
\t\t\tif (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE)
\t\t\t\tcontinue;
\t\t\tconst std::string camera = button->property("cameraName").toString().toUtf8().constData();
\t\t\tcacheIt = anglePreviewCache.find(eventId);
\t\t\tif (cacheIt != anglePreviewCache.end()) {
\t\t\t\tauto iconIt = cacheIt->second.find(camera);
\t\t\t\tif (iconIt != cacheIt->second.end() && !iconIt->second.isNull())
\t\t\t\t\tcontinue;
\t\t\t}
\t\t\tconst qint64 offset = button->property("syncOffsetNs").toLongLong();
\t\t\tuint64_t cameraTimestamp = timestampNs;
\t\t\tif (offset >= 0 && (uint64_t)offset <= UINT64_MAX - cameraTimestamp)
\t\t\t\tcameraTimestamp += (uint64_t)offset;
\t\t\telse if (offset < 0 && (uint64_t)(-offset) < cameraTimestamp)
\t\t\t\tcameraTimestamp -= (uint64_t)(-offset);
\t\t\tAnglePreviewTask task;
\t\t\ttask.camera = camera;
\t\t\ttask.timestampNs = cameraTimestamp;
\t\t\tjob->tasks.emplace_back(std::move(task));
\t\t}
\t\tif (job->tasks.empty()) {
\t\t\tpreviewLoadedEventId = eventId;
\t\t\treturn;
\t\t}
\t\tanglePreviewJob = std::move(job);
\t\tAnglePreviewJob *workerJob = anglePreviewJob.get();
\t\tworkerJob->worker = std::thread([workerJob]() { runAnglePreviewJob(workerJob); });
\t}
'''
rep(old_request, new_request)

old_poll = '''\tvoid pollAnglePreviews()
\t{
\t\tif (!anglePreviewJob || !anglePreviewJob->done.load(std::memory_order_acquire))
\t\t\treturn;
\t\tif (anglePreviewJob->worker.joinable())
\t\t\tanglePreviewJob->worker.join();
\t\tif (anglePreviewJob->eventId == previewTargetEventId && angleEventId() == previewTargetEventId) {
\t\t\tfor (const AnglePreviewResult &result : anglePreviewJob->results) {
\t\t\t\tif (result.rgba.empty())
\t\t\t\t\tcontinue;
\t\t\t\tQToolButton *button = angleButton(QString::fromUtf8(result.camera.c_str()));
\t\t\t\tif (!button)
\t\t\t\t\tcontinue;
\t\t\t\tconst QImage image(result.rgba.data(), ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT,
\t\t\t\t\t\t   ANGLE_PREVIEW_WIDTH * 4, QImage::Format_RGBA8888);
\t\t\t\tbutton->setIcon(QIcon(QPixmap::fromImage(image.copy())));
\t\t\t}
\t\t\tpreviewLoadedEventId = anglePreviewJob->eventId;
\t\t}
\t\tanglePreviewJob.reset();
\t}
'''
new_poll = '''\tvoid pollAnglePreviews()
\t{
\t\tif (!anglePreviewJob || !anglePreviewJob->done.load(std::memory_order_acquire))
\t\t\treturn;
\t\tif (anglePreviewJob->worker.joinable())
\t\t\tanglePreviewJob->worker.join();

\t\tconst uint64_t completedEventId = anglePreviewJob->eventId;
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
\t\twhile (anglePreviewCache.size() > ANGLE_PREVIEW_CACHE_EVENTS)
\t\t\tanglePreviewCache.erase(anglePreviewCache.begin());

\t\tif (completedEventId == previewTargetEventId && angleEventId() == previewTargetEventId)
\t\t\tpreviewLoadedEventId = completedEventId;
\t\tanglePreviewJob.reset();

\t\tif (previewTargetEventId && previewLoadedEventId != previewTargetEventId)
\t\t\trefreshAngleCoverage();
\t}
'''
rep(old_poll, new_poll)

rep('''\tstd::unique_ptr<AnglePreviewJob> anglePreviewJob;
\tstd::unique_ptr<EventThumbnailJob> eventThumbnailJob;
\tstd::map<uint64_t, CachedEventThumbnail> eventThumbnailCache;
''', '''\tstd::unique_ptr<AnglePreviewJob> anglePreviewJob;
\tstd::unique_ptr<EventThumbnailJob> eventThumbnailJob;
\tstd::map<uint64_t, std::map<std::string, QIcon>> anglePreviewCache;
\tstd::map<uint64_t, CachedEventThumbnail> eventThumbnailCache;
''')

p.write_text(text, encoding='utf-8')
print('thumbnail UX patch applied')
