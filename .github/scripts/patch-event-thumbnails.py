from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"anchor not found: {label}")
    return text.replace(old, new, 1)


path = Path("src/sr-event-dock.cpp")
s = path.read_text(encoding="utf-8")

s = replace_once(
    s,
    "#include <QAbstractItemDelegate>\n#include <QAction>\n",
    "#include <QAbstractItemDelegate>\n#include <QAction>\n#include <QApplication>\n",
    "QApplication include",
)
s = replace_once(
    s,
    "#include <QItemSelectionModel>\n#include <QLabel>\n",
    "#include <QItemSelectionModel>\n#include <QLabel>\n#include <QListWidget>\n",
    "QListWidget include",
)
s = replace_once(
    s,
    "#include <QSlider>\n#include <QStandardPaths>\n",
    "#include <QSlider>\n#include <QStackedWidget>\n#include <QStandardPaths>\n",
    "QStackedWidget include",
)
s = replace_once(s, "#include <memory>\n", "#include <map>\n#include <memory>\n", "map include")

s = replace_once(
    s,
    "constexpr int ANGLE_PREVIEW_WIDTH = 128;\nconstexpr int ANGLE_PREVIEW_HEIGHT = 72;\n",
    "constexpr int ANGLE_PREVIEW_WIDTH = 176;\nconstexpr int ANGLE_PREVIEW_HEIGHT = 99;\nconstexpr int EVENT_THUMB_WIDTH = 192;\nconstexpr int EVENT_THUMB_HEIGHT = 108;\nconstexpr size_t EVENT_THUMB_BATCH = 24;\n",
    "preview constants",
)

anchor = """struct AnglePreviewJob {
\tuint64_t eventId = 0;
\tstd::string sessionDir;
\tstd::vector<AnglePreviewTask> tasks;
\tstd::vector<AnglePreviewResult> results;
\tstd::atomic<bool> done{false};
\tstd::thread worker;
};
"""
insert = anchor + """
struct EventThumbnailResult {
\tuint64_t eventId = 0;
\tuint64_t inNs = 0;
\tuint64_t outNs = 0;
\tstd::vector<uint8_t> rgba;
};

struct EventThumbnailTask {
\tuint64_t eventId = 0;
\tuint64_t inNs = 0;
\tuint64_t outNs = 0;
\tstd::string camera;
\tuint64_t timestampNs = 0;
};

struct EventThumbnailJob {
\tuint64_t generation = 0;
\tstd::string sessionDir;
\tstd::vector<EventThumbnailTask> tasks;
\tstd::vector<EventThumbnailResult> results;
\tstd::atomic<bool> done{false};
\tstd::thread worker;
};

struct CachedEventThumbnail {
\tuint64_t inNs = 0;
\tuint64_t outNs = 0;
\tQIcon icon;
};
"""
s = replace_once(s, anchor, insert, "Event thumbnail structs")

anchor = """void runAnglePreviewJob(AnglePreviewJob *job)
{
\tfor (const AnglePreviewTask &task : job->tasks) {
\t\tuint8_t *rgba = nullptr;
\t\tAnglePreviewResult result;
\t\tresult.camera = task.camera;
\t\tif (sr_disk_thumbnail_rgba(job->sessionDir.c_str(), task.camera.c_str(), task.timestampNs,
\t\t\t\t\t   ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT, &rgba) &&
\t\t    rgba) {
\t\t\tconst size_t bytes = (size_t)ANGLE_PREVIEW_WIDTH * ANGLE_PREVIEW_HEIGHT * 4;
\t\t\tresult.rgba.assign(rgba, rgba + bytes);
\t\t}
\t\tbfree(rgba);
\t\tjob->results.emplace_back(std::move(result));
\t}
\tjob->done.store(true, std::memory_order_release);
}
"""
insert = anchor + """

void runEventThumbnailJob(EventThumbnailJob *job)
{
\tfor (const EventThumbnailTask &task : job->tasks) {
\t\tuint8_t *rgba = nullptr;
\t\tEventThumbnailResult result;
\t\tresult.eventId = task.eventId;
\t\tresult.inNs = task.inNs;
\t\tresult.outNs = task.outNs;
\t\tif (sr_disk_thumbnail_rgba(job->sessionDir.c_str(), task.camera.c_str(), task.timestampNs,
\t\t\t\t\t   EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT, &rgba) &&
\t\t    rgba) {
\t\t\tconst size_t bytes = (size_t)EVENT_THUMB_WIDTH * EVENT_THUMB_HEIGHT * 4;
\t\t\tresult.rgba.assign(rgba, rgba + bytes);
\t\t}
\t\tbfree(rgba);
\t\tjob->results.emplace_back(std::move(result));
\t}
\tjob->done.store(true, std::memory_order_release);
}
"""
s = replace_once(s, anchor, insert, "Event thumbnail worker")

s = replace_once(
    s,
    "\t\troot->addLayout(markBar);\n\n\t\ttable = new SrEventTable(this);\n",
    """\t\troot->addLayout(markBar);

\t\tauto *eventViewBar = new QHBoxLayout();
\t\teventViewBar->setSpacing(3);
\t\teventViewBar->addStretch(1);
\t\tauto *viewListButton = new QToolButton(this);
\t\tviewListButton->setText(QStringLiteral(\"☷ \" ) + T(\"EventDock.ViewList\"));
\t\tviewListButton->setCheckable(true);
\t\tviewListButton->setChecked(true);
\t\tauto *viewThumbButton = new QToolButton(this);
\t\tviewThumbButton->setText(QStringLiteral(\"▦ \" ) + T(\"EventDock.ViewThumbnails\"));
\t\tviewThumbButton->setCheckable(true);
\t\teventViewBar->addWidget(viewListButton);
\t\teventViewBar->addWidget(viewThumbButton);
\t\troot->addLayout(eventViewBar);

\t\teventViewStack = new QStackedWidget(this);
\t\ttable = new SrEventTable(eventViewStack);
""",
    "event view controls",
)

s = replace_once(
    s,
    "\t\troot->addWidget(table, 1);\n",
    """\t\teventViewStack->addWidget(table);

\t\tthumbnailList = new QListWidget(eventViewStack);
\t\tthumbnailList->setViewMode(QListView::IconMode);
\t\tthumbnailList->setResizeMode(QListView::Adjust);
\t\tthumbnailList->setMovement(QListView::Static);
\t\tthumbnailList->setWrapping(true);
\t\tthumbnailList->setWordWrap(true);
\t\tthumbnailList->setSelectionMode(QAbstractItemView::ExtendedSelection);
\t\tthumbnailList->setSelectionRectVisible(true);
\t\tthumbnailList->setIconSize(QSize(EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT));
\t\tthumbnailList->setGridSize(QSize(220, 164));
\t\tthumbnailList->setSpacing(4);
\t\tthumbnailList->setUniformItemSizes(true);
\t\tthumbnailList->setToolTip(T(\"EventDock.ViewThumbnails.Tooltip\"));
\t\teventViewStack->addWidget(thumbnailList);
\t\teventViewStack->setCurrentWidget(table);
\t\troot->addWidget(eventViewStack, 1);
""",
    "gallery widget",
)

s = replace_once(
    s,
    "\t\tangleGrid->setHorizontalSpacing(2);\n\t\tangleGrid->setVerticalSpacing(1);\n",
    "\t\tangleGrid->setHorizontalSpacing(4);\n\t\tangleGrid->setVerticalSpacing(4);\n",
    "angle grid spacing",
)

connect_anchor = """\t\tconnect(table, &QTableWidget::itemSelectionChanged, this, [this]() { refreshAngleCoverage(); });
\t\tconnect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) { editEvent(item); });
"""
connect_new = """\t\tconnect(viewListButton, &QToolButton::clicked, this, [this, viewListButton, viewThumbButton]() {
\t\t\tviewListButton->setChecked(true);
\t\t\tviewThumbButton->setChecked(false);
\t\t\teventViewStack->setCurrentWidget(table);
\t\t\tsyncGallerySelectionFromTable();
\t\t});
\t\tconnect(viewThumbButton, &QToolButton::clicked, this, [this, viewListButton, viewThumbButton]() {
\t\t\tviewListButton->setChecked(false);
\t\t\tviewThumbButton->setChecked(true);
\t\t\teventViewStack->setCurrentWidget(thumbnailList);
\t\t\trefreshEventGallery();
\t\t\tsyncGallerySelectionFromTable();
\t\t\trequestEventThumbnails();
\t\t});
\t\tconnect(table, &QTableWidget::itemSelectionChanged, this, [this]() {
\t\t\tif (!syncingEventViews)
\t\t\t\tsyncGallerySelectionFromTable();
\t\t\trefreshAngleCoverage();
\t\t});
\t\tconnect(thumbnailList, &QListWidget::itemSelectionChanged, this, [this]() {
\t\t\tif (!syncingEventViews)
\t\t\t\tsyncTableSelectionFromGallery();
\t\t});
\t\tconnect(thumbnailList, &QListWidget::itemDoubleClicked, this,
\t\t\t[this](QListWidgetItem *) { playSelectedEvent(); });
\t\tconnect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) { editEvent(item); });
"""
s = replace_once(s, connect_anchor, connect_new, "view connections")

s = replace_once(
    s,
    "\t\t\tpollExport();\n\t\t\tpollAnglePreviews();\n",
    "\t\t\tpollExport();\n\t\t\tpollAnglePreviews();\n\t\t\tpollEventThumbnails();\n",
    "thumbnail poll timer",
)
s = replace_once(
    s,
    "\t\tif (anglePreviewJob && anglePreviewJob->worker.joinable())\n\t\t\tanglePreviewJob->worker.join();\n",
    "\t\tif (anglePreviewJob && anglePreviewJob->worker.joinable())\n\t\t\tanglePreviewJob->worker.join();\n\t\tif (eventThumbnailJob && eventThumbnailJob->worker.joinable())\n\t\t\teventThumbnailJob->worker.join();\n",
    "thumbnail destructor",
)

method_anchor = "\tQString selectedCamera() const { return cameraCombo ? cameraCombo->currentData().toString() : QString(); }\n\n"
methods = method_anchor + r'''\tbool thumbnailViewActive() const
\t{
\t\treturn eventViewStack && thumbnailList && eventViewStack->currentWidget() == thumbnailList;
\t}

\tQListWidgetItem *galleryItemById(uint64_t eventId) const
\t{
\t\tif (!thumbnailList || !eventId)
\t\t\treturn nullptr;
\t\tfor (int i = 0; i < thumbnailList->count(); i++) {
\t\t\tQListWidgetItem *item = thumbnailList->item(i);
\t\t\tif (item && item->data(Qt::UserRole).toULongLong() == eventId)
\t\t\t\treturn item;
\t\t}
\t\treturn nullptr;
\t}

\tvoid syncGallerySelectionFromTable()
\t{
\t\tif (!table || !thumbnailList || syncingEventViews)
\t\t\treturn;
\t\tsyncingEventViews = true;
\t\tconst QSignalBlocker blocker(thumbnailList);
\t\tthumbnailList->clearSelection();
\t\tconst std::vector<uint64_t> ids = selectedEventIds();
\t\tconst uint64_t currentId = selectedEventId();
\t\tfor (uint64_t id : ids) {
\t\t\tif (QListWidgetItem *item = galleryItemById(id))
\t\t\t\titem->setSelected(true);
\t\t}
\t\tif (QListWidgetItem *current = galleryItemById(currentId))
\t\t\tthumbnailList->setCurrentItem(current, QItemSelectionModel::NoUpdate);
\t\tsyncingEventViews = false;
\t}

\tvoid syncTableSelectionFromGallery()
\t{
\t\tif (!table || !thumbnailList || syncingEventViews)
\t\t\treturn;
\t\tsyncingEventViews = true;
\t\tconst QSignalBlocker blocker(table);
\t\ttable->clearSelection();
\t\tuint64_t currentId = 0;
\t\tif (QListWidgetItem *current = thumbnailList->currentItem())
\t\t\tcurrentId = current->data(Qt::UserRole).toULongLong();
\t\tint currentRow = -1;
\t\tfor (int row = 0; row < table->rowCount(); row++) {
\t\t\tQTableWidgetItem *idItem = table->item(row, 0);
\t\t\tconst uint64_t id = idItem ? idItem->data(Qt::UserRole).toULongLong() : 0;
\t\t\tQListWidgetItem *galleryItem = galleryItemById(id);
\t\t\tif (galleryItem && galleryItem->isSelected())
\t\t\t\ttable->selectionModel()->select(table->model()->index(row, 0),
\t\t\t\t\t\t\tQItemSelectionModel::Select | QItemSelectionModel::Rows);
\t\t\tif (id && id == currentId)
\t\t\t\tcurrentRow = row;
\t\t}
\t\tif (currentRow >= 0)
\t\t\ttable->setCurrentCell(currentRow, 0, QItemSelectionModel::NoUpdate);
\t\tsyncingEventViews = false;
\t\trefreshAngleCoverage();
\t}

\tQString eventGalleryText(const sr_event_record &event) const
\t{
\t\tconst QString name = QString::fromUtf8(event.name ? event.name : "").trimmed();
\t\tconst QString tag = QString::fromUtf8(event.tag ? event.tag : "").trimmed();
\t\tQString first = QStringLiteral("#%1").arg(event.id);
\t\tif (!name.isEmpty())
\t\t\tfirst += QStringLiteral("  ") + name;
\t\tQString second = QStringLiteral("%1  ·  %2%")
\t\t\t\t\t .arg(durationText(event))
\t\t\t\t\t .arg(event.speed_percent, 0, 'f', 0);
\t\tif (!tag.isEmpty())
\t\t\tsecond += QStringLiteral("  ·  ") + tag;
\t\treturn first + QStringLiteral("\n") + second;
\t}

\tbool makeEventThumbnailTask(const sr_event_record &event, const QStringList &cameras,
\t\t\t\t    EventThumbnailTask *task)
\t{
\t\tif (!task || cameras.isEmpty() || event.out_ns <= event.in_ns)
\t\t\treturn false;

\t\tQString preferred;
\t\tif (event.preferred_camera_id) {
\t\t\tchar *name = nullptr;
\t\t\tif (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &name) && name)
\t\t\t\tpreferred = QString::fromUtf8(name);
\t\t\tbfree(name);
\t\t}

\t\tauto prepare = [&](const QString &camera, bool fullOnly) -> bool {
\t\t\tif (camera.isEmpty())
\t\t\t\treturn false;
\t\t\tsr_replay_coverage_info coverage = {};
\t\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\t\tif (!sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns, &coverage) ||
\t\t\t    coverage.coverage == SR_REPLAY_COVERAGE_NONE ||
\t\t\t    (fullOnly && coverage.coverage != SR_REPLAY_COVERAGE_FULL))
\t\t\t\treturn false;
\t\t\tuint64_t timestamp = event.in_ns + (event.out_ns - event.in_ns) / 2;
\t\t\tif (coverage.coverage == SR_REPLAY_COVERAGE_PARTIAL && coverage.playable_out_ns > coverage.playable_in_ns)
\t\t\t\ttimestamp = coverage.playable_in_ns + (coverage.playable_out_ns - coverage.playable_in_ns) / 2;
\t\t\tconst int64_t offset = coverage.sync_offset_ns;
\t\t\tif (offset >= 0 && (uint64_t)offset <= UINT64_MAX - timestamp)
\t\t\t\ttimestamp += (uint64_t)offset;
\t\t\telse if (offset < 0 && (uint64_t)(-offset) < timestamp)
\t\t\t\ttimestamp -= (uint64_t)(-offset);
\t\t\ttask->eventId = event.id;
\t\t\ttask->inNs = event.in_ns;
\t\t\ttask->outNs = event.out_ns;
\t\t\ttask->camera = cameraUtf8.constData();
\t\t\ttask->timestampNs = timestamp;
\t\t\treturn true;
\t\t};

\t\tif (!preferred.isEmpty() && prepare(preferred, false))
\t\t\treturn true;
\t\tfor (const QString &camera : cameras) {
\t\t\tif (camera != preferred && prepare(camera, true))
\t\t\t\treturn true;
\t\t}
\t\tfor (const QString &camera : cameras) {
\t\t\tif (camera != preferred && prepare(camera, false))
\t\t\t\treturn true;
\t\t}
\t\treturn false;
\t}

\tvoid refreshEventGallery()
\t{
\t\tif (!thumbnailList || !table || !controller)
\t\t\treturn;

\t\tstd::vector<uint64_t> ids;
\t\tids.reserve((size_t)table->rowCount());
\t\tfor (int row = 0; row < table->rowCount(); row++) {
\t\t\tQTableWidgetItem *item = table->item(row, 0);
\t\t\tconst uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
\t\t\tif (id)
\t\t\t\tids.push_back(id);
\t\t}

\t\tconst bool structureChanged = galleryListId != currentList() || ids != galleryEventIds;
\t\tif (structureChanged) {
\t\t\tgalleryListId = currentList();
\t\t\tgalleryEventIds = ids;
\t\t\tgalleryGeneration++;
\t\t\tconst QSignalBlocker blocker(thumbnailList);
\t\t\tthumbnailList->clear();
\t\t\tfor (uint64_t id : galleryEventIds) {
\t\t\t\tauto *item = new QListWidgetItem(thumbnailList);
\t\t\t\titem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
\t\t\t\titem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
\t\t\t}
\t\t}

\t\tfor (int i = 0; i < thumbnailList->count(); i++) {
\t\t\tQListWidgetItem *item = thumbnailList->item(i);
\t\t\tconst uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
\t\t\tsr_event_record event = {};
\t\t\tif (!id || !sr_event_controller_get_event(controller, id, &event))
\t\t\t\tcontinue;
\t\t\titem->setText(eventGalleryText(event));
\t\t\titem->setToolTip(QStringLiteral("#%1 · %2 · %3")
\t\t\t\t\t\t .arg(event.id)
\t\t\t\t\t\t .arg(stateText(event))
\t\t\t\t\t\t .arg(QString::fromUtf8(event.tag ? event.tag : "")));
\t\t\titem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(event.in_ns));
\t\t\titem->setData(Qt::UserRole + 2, QVariant::fromValue<qulonglong>(event.out_ns));
\t\t\tauto cached = eventThumbnailCache.find(id);
\t\t\tif (cached != eventThumbnailCache.end() && cached->second.inNs == event.in_ns &&
\t\t\t    cached->second.outNs == event.out_ns)
\t\t\t\titem->setIcon(cached->second.icon);
\t\t\telse
\t\t\t\titem->setIcon(QIcon());
\t\t\tsr_event_controller_free_event(&event);
\t\t}

\t\tif (structureChanged)
\t\t\tsyncGallerySelectionFromTable();
\t\tif (thumbnailViewActive())
\t\t\trequestEventThumbnails();
\t}

\tvoid requestEventThumbnails()
\t{
\t\tif (!thumbnailViewActive() || eventThumbnailJob || !controller || !thumbnailList)
\t\t\treturn;
\t\tconst QStringList cameras = captureCameraNames();
\t\tif (cameras.isEmpty())
\t\t\treturn;

\t\tauto job = std::make_unique<EventThumbnailJob>();
\t\tjob->generation = galleryGeneration;
\t\tfor (int i = 0; i < thumbnailList->count() && job->tasks.size() < EVENT_THUMB_BATCH; i++) {
\t\t\tQListWidgetItem *item = thumbnailList->item(i);
\t\t\tconst uint64_t id = item ? item->data(Qt::UserRole).toULongLong() : 0;
\t\t\tconst uint64_t inNs = item ? item->data(Qt::UserRole + 1).toULongLong() : 0;
\t\t\tconst uint64_t outNs = item ? item->data(Qt::UserRole + 2).toULongLong() : 0;
\t\t\tauto cached = eventThumbnailCache.find(id);
\t\t\tif (cached != eventThumbnailCache.end() && cached->second.inNs == inNs &&
\t\t\t    cached->second.outNs == outNs && !cached->second.icon.isNull())
\t\t\t\tcontinue;
\t\t\tsr_event_record event = {};
\t\t\tif (!id || !sr_event_controller_get_event(controller, id, &event))
\t\t\t\tcontinue;
\t\t\tEventThumbnailTask task;
\t\t\tif (makeEventThumbnailTask(event, cameras, &task))
\t\t\t\tjob->tasks.emplace_back(std::move(task));
\t\t\tsr_event_controller_free_event(&event);
\t\t}
\t\tif (job->tasks.empty())
\t\t\treturn;
\t\tchar *sessionPath = sr_session_get_or_create_path();
\t\tif (!sessionPath)
\t\t\treturn;
\t\tjob->sessionDir = sessionPath;
\t\tbfree(sessionPath);
\t\teventThumbnailJob = std::move(job);
\t\tEventThumbnailJob *workerJob = eventThumbnailJob.get();
\t\tworkerJob->worker = std::thread([workerJob]() { runEventThumbnailJob(workerJob); });
\t}

\tvoid pollEventThumbnails()
\t{
\t\tif (!eventThumbnailJob || !eventThumbnailJob->done.load(std::memory_order_acquire))
\t\t\treturn;
\t\tif (eventThumbnailJob->worker.joinable())
\t\t\teventThumbnailJob->worker.join();
\t\tif (eventThumbnailJob->generation == galleryGeneration) {
\t\t\tfor (const EventThumbnailResult &result : eventThumbnailJob->results) {
\t\t\t\tif (result.rgba.empty())
\t\t\t\t\tcontinue;
\t\t\t\tQListWidgetItem *item = galleryItemById(result.eventId);
\t\t\t\tif (!item || item->data(Qt::UserRole + 1).toULongLong() != result.inNs ||
\t\t\t\t    item->data(Qt::UserRole + 2).toULongLong() != result.outNs)
\t\t\t\t\tcontinue;
\t\t\t\tconst QImage image(result.rgba.data(), EVENT_THUMB_WIDTH, EVENT_THUMB_HEIGHT,
\t\t\t\t\t\t   EVENT_THUMB_WIDTH * 4, QImage::Format_RGBA8888);
\t\t\t\tCachedEventThumbnail cached;
\t\t\t\tcached.inNs = result.inNs;
\t\t\t\tcached.outNs = result.outNs;
\t\t\t\tcached.icon = QIcon(QPixmap::fromImage(image.copy()));
\t\t\t\teventThumbnailCache[result.eventId] = cached;
\t\t\t\titem->setIcon(cached.icon);
\t\t\t}
\t\t}
\t\teventThumbnailJob.reset();
\t\trequestEventThumbnails();
\t}

'''
s = replace_once(s, method_anchor, methods, "gallery methods")

# Make angle cards visibly image-first instead of a narrow text row.
s = replace_once(
    s,
    "\t\t\tauto *button = new QPushButton(camera, this);\n\t\t\tbutton->setCheckable(true);\n\t\t\tbutton->setMinimumSize(170, 86);\n\t\t\tbutton->setIconSize(QSize(ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT));\n",
    "\t\t\tauto *button = new QToolButton(this);\n\t\t\tbutton->setText(camera);\n\t\t\tbutton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);\n\t\t\tbutton->setCheckable(true);\n\t\t\tbutton->setAutoRaise(false);\n\t\t\tbutton->setMinimumSize(210, 132);\n\t\t\tbutton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);\n\t\t\tbutton->setIconSize(QSize(ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT));\n",
    "angle tool button",
)
s = s.replace("for (QPushButton *button : angleButtons)", "for (QToolButton *button : angleButtons)")
s = s.replace("QPushButton *button = angleButton(", "QToolButton *button = angleButton(")
s = s.replace("QPushButton *angle = angleButton(", "QToolButton *angle = angleButton(")
s = s.replace("\tQPushButton *angleButton(const QString &camera) const", "\tQToolButton *angleButton(const QString &camera) const")
s = s.replace("\tQVector<QPushButton *> angleButtons;", "\tQVector<QToolButton *> angleButtons;")

# Avoid rebuilding the gallery under a rubber-band drag; refresh it after release.
s = replace_once(
    s,
    "\t\tif (!controller || !table || tableEditing || table->selectionGestureActive())\n\t\t\treturn;\n",
    "\t\tif (!controller || !table || tableEditing || table->selectionGestureActive() ||\n\t\t    (thumbnailViewActive() && (QApplication::mouseButtons() & Qt::LeftButton)))\n\t\t\treturn;\n",
    "gallery gesture refresh guard",
)
s = replace_once(
    s,
    "\t\ttableRefreshing = false;\n\t\trefreshAngleCoverage();\n",
    "\t\ttableRefreshing = false;\n\t\trefreshEventGallery();\n\t\trefreshAngleCoverage();\n",
    "refresh gallery",
)

member_anchor = """\tQComboBox *exportModeCombo = nullptr;
\tQGridLayout *angleGrid = nullptr;
\tQVector<QToolButton *> angleButtons;
"""
member_new = """\tQComboBox *exportModeCombo = nullptr;
\tQGridLayout *angleGrid = nullptr;
\tQVector<QToolButton *> angleButtons;
\tQStackedWidget *eventViewStack = nullptr;
\tQListWidget *thumbnailList = nullptr;
"""
s = replace_once(s, member_anchor, member_new, "gallery members")
s = replace_once(
    s,
    "\tstd::unique_ptr<AnglePreviewJob> anglePreviewJob;\n\tuint64_t timelineEventId = 0;\n",
    "\tstd::unique_ptr<AnglePreviewJob> anglePreviewJob;\n\tstd::unique_ptr<EventThumbnailJob> eventThumbnailJob;\n\tstd::map<uint64_t, CachedEventThumbnail> eventThumbnailCache;\n\tstd::vector<uint64_t> galleryEventIds;\n\tunsigned galleryListId = 0;\n\tuint64_t galleryGeneration = 1;\n\tbool syncingEventViews = false;\n\tuint64_t timelineEventId = 0;\n",
    "gallery state members",
)

path.write_text(s, encoding="utf-8")

for locale in [Path("data/locale/en-US.ini"), Path("data/locale/es-ES.ini")]:
    text = locale.read_text(encoding="utf-8")
    if "EventDock.ViewThumbnails=" in text:
        continue
    if locale.name == "en-US.ini":
        addition = (
            'EventDock.ViewList="List"\n'
            'EventDock.ViewThumbnails="Thumbnails"\n'
            'EventDock.ViewThumbnails.Tooltip="Visual Event bin. Drag a selection box or Ctrl-click multiple cards, then use Play Selected. The thumbnail uses the preferred camera when available, otherwise the first camera with usable coverage."\n'
        )
    else:
        addition = (
            'EventDock.ViewList="Lista"\n'
            'EventDock.ViewThumbnails="Miniaturas"\n'
            'EventDock.ViewThumbnails.Tooltip="Bin visual de eventos. Arrastrá un cuadro de selección o usá Ctrl+clic para elegir varias tarjetas y luego Play Selected. La miniatura usa la cámara preferida cuando está disponible."\n'
        )
    marker = 'EventDock.PlayMenu='
    pos = text.find(marker)
    if pos < 0:
        raise SystemExit(f"locale marker not found: {locale}")
    text = text[:pos] + addition + text[pos:]
    locale.write_text(text, encoding="utf-8")
