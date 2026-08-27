from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    if new in text:
        return
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


replace_once(
    "src/sr-disk-player.c",
    """struct sr_disk_player *sr_disk_player_create(const char *session_dir, const char *camera_name)
{
\tif (!session_dir || !*session_dir || !camera_name || !*camera_name)
\t\treturn NULL;

\tstruct sr_disk_player *p = bzalloc(sizeof(*p));
\tp->session_dir = bstrdup(session_dir);
\tp->camera_name = bstrdup(camera_name);
\tp->current_position = -1;
\tsr_frame_cache_init(&p->frame_cache, (size_t)SR_DISK_PLAYER_FRAME_CACHE_BYTES);

\tif (!sr_disk_player_refresh(p)) {
\t\tsr_disk_player_destroy(p);
\t\treturn NULL;
\t}
\treturn p;
}
""",
    """struct sr_disk_player *sr_disk_player_create(const char *session_dir, const char *camera_name)
{
\treturn sr_disk_player_create_with_cache(session_dir, camera_name,
\t\t\t\t\t       (size_t)SR_DISK_PLAYER_FRAME_CACHE_BYTES);
}

struct sr_disk_player *sr_disk_player_create_with_cache(const char *session_dir, const char *camera_name,
\t\t\t\t\t\t size_t max_cache_bytes)
{
\tif (!session_dir || !*session_dir || !camera_name || !*camera_name)
\t\treturn NULL;

\tstruct sr_disk_player *p = bzalloc(sizeof(*p));
\tp->session_dir = bstrdup(session_dir);
\tp->camera_name = bstrdup(camera_name);
\tp->current_position = -1;
\tsr_frame_cache_init(&p->frame_cache, max_cache_bytes);

\tif (!sr_disk_player_refresh(p)) {
\t\tsr_disk_player_destroy(p);
\t\treturn NULL;
\t}
\treturn p;
}
""",
    "disk-player cache constructor",
)

replace_once(
    "src/sr-multiview-dock.cpp",
    """#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>""",
    """#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMenu>""",
    "multiview Qt map/hide includes",
)
replace_once(
    "src/sr-multiview-dock.cpp",
    """#include <QPointer>
#include <QPushButton>
#include <QScrollArea>""",
    """#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShowEvent>""",
    "multiview resize/show includes",
)
replace_once(
    "src/sr-multiview-dock.cpp",
    """explicit SrMultiviewDock(sr_event_controller *eventController, QWidget *parent = nullptr)
\t\t: QWidget(parent), controller(eventController)
\t{
\t\tsetObjectName""",
    """explicit SrMultiviewDock(sr_event_controller *eventController, QWidget *parent = nullptr) : QWidget(parent)
\t{
\t\t(void)eventController;
\t\tsetObjectName""",
    "multiview unused controller",
)
replace_once(
    "src/sr-multiview-dock.cpp",
    "\t\thiddenCameras.intersect(QSet<QString>(cameraNames.begin(), cameraNames.end()));",
    """\t\tQSet<QString> availableCameras;
\t\tfor (const QString &camera : cameraNames)
\t\t\tavailableCameras.insert(camera);
\t\thiddenCameras.intersect(availableCameras);""",
    "multiview Qt6 camera set",
)
replace_once(
    "src/sr-multiview-dock.cpp",
    """\tvoid hideEvent(QHideEvent *event) override
\t{
\t\tQWidget::hideEvent(event);
\t\t/* Hidden dock performs no new decode work. Worker threads stay asleep and
\t\t * keep only their bounded warm caches for a quick reopen. */
\t}""",
    """\tvoid hideEvent(QHideEvent *event) override
\t{
\t\tQWidget::hideEvent(event);
\t\t/* Release all readers/decoders while the dock is hidden. The worker
\t\t * threads remain asleep, so a closed Multiview costs no decoder or
\t\t * segment-reader resources. */
\t\tfor (const auto &tile : tiles)
\t\t\ttile->decoder().setSource(QString(), QString());
\t}""",
    "multiview hidden decoder release",
)
replace_once(
    "src/sr-multiview-dock.cpp",
    """\t\tfor (const auto &tile : tiles) {
\t\t\tif (std::find(visible.begin(), visible.end(), tile.get()) == visible.end())
\t\t\t\ttile->setVisible(false);
\t\t}""",
    """\t\tfor (const auto &tile : tiles) {
\t\t\tif (std::find(visible.begin(), visible.end(), tile.get()) == visible.end()) {
\t\t\t\ttile->setVisible(false);
\t\t\t\ttile->decoder().setSource(QString(), QString());
\t\t\t}
\t\t}""",
    "multiview hidden tile decoder release",
)
replace_once(
    "src/sr-multiview-dock.cpp",
    "\tsr_event_controller *controller = nullptr;\n\tQToolButton *cameraMenuButton",
    "\tQToolButton *cameraMenuButton",
    "multiview controller member",
)

replace_once(
    "src/sr-event-dock.cpp",
    """#include "sr-event-controller.h"
#include "sr-dock.h"
#include "sr-event-export.h""",
    """#include "sr-event-controller.h"
#include "sr-dock.h"
#include "sr-event-export.h"
#include "sr-multiview-dock.h""",
    "event dock multiview include",
)
replace_once(
    "src/sr-event-dock.cpp",
    """#include <QProgressBar>
#include <QRubberBand>""",
    """#include <QProgressBar>
#include <QPointer>
#include <QRubberBand>""",
    "event dock QPointer include",
)
replace_once(
    "src/sr-event-dock.cpp",
    """\t\tangleHeader->addWidget(new QLabel(T("EventDock.Angles"), this));
\t\tangleHeader->addStretch(1);
\t\tauto *angleLegend = new QLabel(T("EventDock.AnglesLegend"), this);""",
    """\t\tangleHeader->addWidget(new QLabel(T("EventDock.Angles"), this));
\t\tangleHeader->addStretch(1);
\t\tauto *multiviewButton = new QToolButton(this);
\t\tmultiviewButton->setText(T("Multiview.Open"));
\t\tmultiviewButton->setAutoRaise(true);
\t\tmultiviewButton->setToolTip(T("Multiview.OpenTooltip"));
\t\tangleHeader->addWidget(multiviewButton);
\t\tauto *angleLegend = new QLabel(T("EventDock.AnglesLegend"), this);""",
    "event dock multiview button",
)
replace_once(
    "src/sr-event-dock.cpp",
    """\t\tconnect(setupButton, &QToolButton::clicked, this, [this]() { openReplaySetup(); });
\t\tconnect(up, &QPushButton::clicked, this, [this]() { moveRow(-1); });""",
    """\t\tconnect(setupButton, &QToolButton::clicked, this, [this]() { openReplaySetup(); });
\t\tconnect(multiviewButton, &QToolButton::clicked, this, []() { sr_multiview_dock_show(); });
\t\tconnect(up, &QPushButton::clicked, this, [this]() { moveRow(-1); });""",
    "event dock multiview button connection",
)

public_anchor = """\t~SrEventDock() override
\t{
\t\tif (exportJob) {
\t\t\texportJob->cancel.store(true, std::memory_order_relaxed);
\t\t\tif (exportJob->worker.joinable())
\t\t\t\texportJob->worker.join();
\t\t}
\t\tif (anglePreviewJob && anglePreviewJob->worker.joinable())
\t\t\tanglePreviewJob->worker.join();
\t\tif (eventThumbnailJob && eventThumbnailJob->worker.joinable())
\t\t\teventThumbnailJob->worker.join();
\t}

private:
"""
public_replacement = """\t~SrEventDock() override
\t{
\t\tif (exportJob) {
\t\t\texportJob->cancel.store(true, std::memory_order_relaxed);
\t\t\tif (exportJob->worker.joinable())
\t\t\t\texportJob->worker.join();
\t\t}
\t\tif (anglePreviewJob && anglePreviewJob->worker.joinable())
\t\t\tanglePreviewJob->worker.join();
\t\tif (eventThumbnailJob && eventThumbnailJob->worker.joinable())
\t\t\teventThumbnailJob->worker.join();
\t}

\tbool editorSnapshot(sr_event_editor_snapshot *snapshot)
\t{
\t\tif (!snapshot)
\t\t\treturn false;
\t\tstd::memset(snapshot, 0, sizeof(*snapshot));
\t\tif (!controller)
\t\t\treturn false;
\t\tupdateEditTimelineBounds();
\t\tsnapshot->edit_mode = !replayPlayoutActive();
\t\tif (editTimelineHaveBounds) {
\t\t\tsnapshot->record_start_ns = editTimelineStartNs;
\t\t\tsnapshot->record_end_ns = editTimelineEndNs;
\t\t}
\t\tconst uint64_t eventId = selectedEventId();
\t\tsr_event_record event = {};
\t\tif (!eventId || !sr_event_controller_get_event(controller, eventId, &event))
\t\t\treturn true;
\t\tsnapshot->event_id = event.id;
\t\tsnapshot->in_ns = event.in_ns;
\t\tsnapshot->out_ns = event.out_ns;
\t\tsnapshot->available = editTimelineHaveBounds && event.out_ns > event.in_ns;
\t\tconst QString storedCamera = eventPreferredCamera(event);
\t\tconst QByteArray storedUtf8 = storedCamera.toUtf8();
\t\tif (!storedUtf8.isEmpty()) {
\t\t\tstd::strncpy(snapshot->selected_camera, storedUtf8.constData(), sizeof(snapshot->selected_camera) - 1);
\t\t\tsnapshot->selected_camera[sizeof(snapshot->selected_camera) - 1] = '\\0';
\t\t}
\t\tuint64_t playhead = editTimeline ? editTimeline->playheadTimestamp() : event.in_ns;
\t\tsr_replay_channel_state state = {};
\t\tif (sr_replay_channel_get_state(transportBus(), &state) && state.cued && state.preview_mode &&
\t\t    state.event_id == event.id) {
\t\t\tplayhead = state.playhead_ns;
\t\t\tsnapshot->playing = state.playing && !state.paused;
\t\t\tsnapshot->paused = state.paused;
\t\t\tsnapshot->loop = state.loop;
\t\t\tstd::strncpy(snapshot->preview_camera, state.camera_name, sizeof(snapshot->preview_camera) - 1);
\t\t\tsnapshot->preview_camera[sizeof(snapshot->preview_camera) - 1] = '\\0';
\t\t} else {
\t\t\tsnapshot->loop = loopButton && loopButton->isChecked();
\t\t\tconst QByteArray previewUtf8 = editPreviewCamera.toUtf8();
\t\t\tif (!previewUtf8.isEmpty()) {
\t\t\t\tstd::strncpy(snapshot->preview_camera, previewUtf8.constData(),
\t\t\t\t\t     sizeof(snapshot->preview_camera) - 1);
\t\t\t\tsnapshot->preview_camera[sizeof(snapshot->preview_camera) - 1] = '\\0';
\t\t\t}
\t\t}
\t\tif (snapshot->available)
\t\t\tplayhead = qBound(editTimelineStartNs, playhead, editTimelineEndNs);
\t\tsnapshot->playhead_ns = playhead;
\t\tsr_event_controller_free_event(&event);
\t\treturn true;
\t}

\tbool editorSeek(uint64_t timestampNs)
\t{
\t\treturn !replayPlayoutActive() && previewSeekTo(timestampNs);
\t}

\tbool editorSetRange(uint64_t inNs, uint64_t outNs)
\t{
\t\tif (replayPlayoutActive() || !selectedEventId() || outNs <= inNs)
\t\t\treturn false;
\t\tconst uint64_t eventId = selectedEventId();
\t\teditSelectedEventRange(inNs, outNs);
\t\tsr_event_record event = {};
\t\tif (!sr_event_controller_get_event(controller, eventId, &event))
\t\t\treturn false;
\t\tconst bool matched = event.in_ns == inNs && event.out_ns == outNs;
\t\tsr_event_controller_free_event(&event);
\t\treturn matched;
\t}

\tbool editorSetMarker(bool outMarker)
\t{
\t\tif (replayPlayoutActive() || !editTimeline || !editTimeline->hasSelection())
\t\t\treturn false;
\t\tsetEditMarkerAtPlayhead(outMarker);
\t\treturn true;
\t}

\tbool editorGotoMarker(bool outMarker)
\t{
\t\tif (replayPlayoutActive() || !editTimeline || !editTimeline->hasSelection())
\t\t\treturn false;
\t\treturn previewSeekTo(outMarker ? editTimeline->selectionOut() : editTimeline->selectionIn());
\t}

\tbool editorStepFrames(int frames)
\t{
\t\tif (replayPlayoutActive() || !editTimelineHaveBounds || !editTimeline || !frames)
\t\t\treturn false;
\t\tstepEditFrames(frames);
\t\treturn true;
\t}

\tbool editorSelectCamera(const QString &camera)
\t{
\t\tif (replayPlayoutActive() || !controller || !selectedEventId())
\t\t\treturn false;
\t\tconst uint64_t eventId = selectedEventId();
\t\tselectAngle(camera);
\t\tsr_event_record event = {};
\t\tif (!sr_event_controller_get_event(controller, eventId, &event))
\t\t\treturn false;
\t\tconst QString stored = eventPreferredCamera(event);
\t\tsr_event_controller_free_event(&event);
\t\treturn stored == camera;
\t}

\tbool editorTogglePlay()
\t{
\t\tif (replayPlayoutActive() || !selectedEventId())
\t\t\treturn false;
\t\ttoggleEditPreview();
\t\treturn true;
\t}

\tbool editorPlayFromIn()
\t{
\t\tif (replayPlayoutActive() || !selectedEventId())
\t\t\treturn false;
\t\tpreviewPlayFromIn();
\t\treturn true;
\t}

\tbool editorSetLoop(bool enabled)
\t{
\t\tif (replayPlayoutActive() || !selectedEventId())
\t\t\treturn false;
\t\tif (loopButton) {
\t\t\tconst QSignalBlocker blocker(loopButton);
\t\t\tloopButton->setChecked(enabled);
\t\t}
\t\tsr_replay_channel_state state = {};
\t\tif (sr_replay_channel_get_state(transportBus(), &state) && state.cued && state.preview_mode)
\t\t\tsr_replay_channel_set_loop(transportBus(), enabled);
\t\treturn true;
\t}

private:
"""
replace_once("src/sr-event-dock.cpp", public_anchor, public_replacement, "event editor bridge methods")

replace_once(
    "src/sr-event-dock.cpp",
    """\tuint64_t previewTargetEventId = 0;
\tuint64_t previewLoadedEventId = 0;
};

} // namespace

QWidget *sr_event_dock_create(struct sr_event_controller *controller, QWidget *parent)
{
\treturn new SrEventDock(controller, parent);
}
""",
    """\tuint64_t previewTargetEventId = 0;
\tuint64_t previewLoadedEventId = 0;
};

QPointer<SrEventDock> g_event_dock;

} // namespace

QWidget *sr_event_dock_create(struct sr_event_controller *controller, QWidget *parent)
{
\tauto *dock = new SrEventDock(controller, parent);
\tg_event_dock = dock;
\treturn dock;
}

bool sr_event_dock_get_editor_snapshot(struct sr_event_editor_snapshot *snapshot)
{
\treturn g_event_dock && g_event_dock->editorSnapshot(snapshot);
}

bool sr_event_dock_editor_seek(uint64_t timestamp_ns)
{
\treturn g_event_dock && g_event_dock->editorSeek(timestamp_ns);
}

bool sr_event_dock_editor_set_range(uint64_t in_ns, uint64_t out_ns)
{
\treturn g_event_dock && g_event_dock->editorSetRange(in_ns, out_ns);
}

bool sr_event_dock_editor_set_marker(bool out_marker)
{
\treturn g_event_dock && g_event_dock->editorSetMarker(out_marker);
}

bool sr_event_dock_editor_goto_marker(bool out_marker)
{
\treturn g_event_dock && g_event_dock->editorGotoMarker(out_marker);
}

bool sr_event_dock_editor_step_frames(int frames)
{
\treturn g_event_dock && g_event_dock->editorStepFrames(frames);
}

bool sr_event_dock_editor_select_camera(const char *camera_name)
{
\treturn g_event_dock && g_event_dock->editorSelectCamera(QString::fromUtf8(camera_name ? camera_name : ""));
}

bool sr_event_dock_editor_toggle_play(void)
{
\treturn g_event_dock && g_event_dock->editorTogglePlay();
}

bool sr_event_dock_editor_play_from_in(void)
{
\treturn g_event_dock && g_event_dock->editorPlayFromIn();
}

bool sr_event_dock_editor_set_loop(bool enabled)
{
\treturn g_event_dock && g_event_dock->editorSetLoop(enabled);
}
""",
    "event editor bridge wrappers",
)

replace_once(
    "src/sr-dock.cpp",
    """#include "sr-event-dock.h"
#include "sr-thumb.h""",
    """#include "sr-event-dock.h"
#include "sr-multiview-dock.h"
#include "sr-thumb.h""",
    "dock multiview include",
)
replace_once(
    "src/sr-dock.cpp",
    """\tif (!obs_frontend_add_dock_by_id("pitel_instant_replay_dock", obs_module_text("Dock.Title"), tabs)) {
\t\tdelete tabs;
\t\treturn;
\t}
\tg_dock = clips;""",
    """\tif (!obs_frontend_add_dock_by_id("pitel_instant_replay_dock", obs_module_text("Dock.Title"), tabs)) {
\t\tdelete tabs;
\t\treturn;
\t}

\tauto *multiview = sr_multiview_dock_create(controller);
\tif (!obs_frontend_add_dock_by_id("pitel_instant_replay_multiview_dock", obs_module_text("Multiview.Title"),
\t\t\t\t     multiview))
\t\tdelete multiview;
\tg_dock = clips;""",
    "dock multiview registration",
)

replace_once(
    "CMakeLists.txt",
    "    src/sr-event-dock.cpp\n    src/sr-replay-channel.c",
    "    src/sr-event-dock.cpp\n    src/sr-multiview-dock.cpp\n    src/sr-replay-channel.c",
    "cmake multiview source",
)

strings_en = """
# Replay Multiview editor
Multiview.Title="Pitel Instant Replay — Multiview"
Multiview.Open="MULTIVIEW"
Multiview.OpenTooltip="Open the synchronized multicamera replay editor."
Multiview.Cameras="Cameras"
Multiview.Quality="Quality"
Multiview.QualityAuto="Auto"
Multiview.QualitySource="Source"
Multiview.Fps="Preview FPS"
Multiview.FpsAuto="Auto"
Multiview.AutoAngle="AUTO angle"
Multiview.Waiting="Waiting for replay media…"
Multiview.NoCoverage="No Event coverage"
Multiview.NoMediaAtCursor="No media at cursor"
Multiview.DecodeWaiting="Frame unavailable / decoding…"
Multiview.NoTimeline="No active recording timeline"
Multiview.SelectEvent="Select an Event in Replay Operator"
Multiview.EditorUnavailable="Replay editor is unavailable"
Multiview.PlayoutLocked="PLAYOUT — Multiview editing locked"
Multiview.PlayPause="Play / Pause"
Multiview.PlayFromIn="▶ IN→OUT"
Multiview.SetIn="SET IN"
Multiview.SetOut="SET OUT"
Multiview.Loop="Loop"
"""
strings_es = """
# Editor Replay Multiview
Multiview.Title="Pitel Instant Replay — Multiview"
Multiview.Open="MULTIVIEW"
Multiview.OpenTooltip="Abrir el editor multicámara sincronizado de replay."
Multiview.Cameras="Cámaras"
Multiview.Quality="Calidad"
Multiview.QualityAuto="Auto"
Multiview.QualitySource="Fuente"
Multiview.Fps="FPS de preview"
Multiview.FpsAuto="Auto"
Multiview.AutoAngle="Ángulo AUTO"
Multiview.Waiting="Esperando medios de replay…"
Multiview.NoCoverage="Sin cobertura del Event"
Multiview.NoMediaAtCursor="Sin medios en el cursor"
Multiview.DecodeWaiting="Frame no disponible / decodificando…"
Multiview.NoTimeline="Sin timeline de grabación activa"
Multiview.SelectEvent="Seleccione un Event en Replay Operator"
Multiview.EditorUnavailable="El editor de replay no está disponible"
Multiview.PlayoutLocked="PLAYOUT — edición Multiview bloqueada"
Multiview.PlayPause="Play / Pause"
Multiview.PlayFromIn="▶ IN→OUT"
Multiview.SetIn="SET IN"
Multiview.SetOut="SET OUT"
Multiview.Loop="Loop"
"""
for locale, block in [("data/locale/en-US.ini", strings_en), ("data/locale/es-ES.ini", strings_es)]:
    p = Path(locale)
    text = p.read_text(encoding="utf-8")
    if "Multiview.Title=" not in text:
        p.write_text(text.rstrip() + "\n" + block.lstrip(), encoding="utf-8")
