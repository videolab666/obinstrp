from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / "src/sr-event-dock.cpp"
EN = ROOT / "data/locale/en-US.ini"
ES = ROOT / "data/locale/es-ES.ini"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected 1 match, found {count}")
    return text.replace(old, new, 1)


def replace_between(text: str, start: str, end: str, replacement: str, label: str) -> str:
    i = text.find(start)
    if i < 0:
        raise RuntimeError(f"{label}: start marker not found")
    j = text.find(end, i)
    if j < 0:
        raise RuntimeError(f"{label}: end marker not found")
    return text[:i] + replacement + text[j:]


cpp = CPP.read_text(encoding="utf-8")

cpp = replace_once(
    cpp,
    '''\t\tcueBar->addWidget(new QLabel(T("EventDock.Camera"), this));
\t\tcameraCombo = new QComboBox(this);
\t\tcameraCombo->setMinimumContentsLength(18);
\t\tcueBar->addWidget(cameraCombo, 1);
\t\tauto *setPreferred = new QPushButton(T("EventDock.SetPreferred"), this);
\t\tauto *clearPreferred = new QPushButton(T("EventDock.ClearPreferred"), this);
\t\tauto *cueA = new QPushButton(T("EventDock.CueA"), this);
\t\tauto *cueB = new QPushButton(T("EventDock.CueB"), this);
\t\tcueBar->addWidget(setPreferred);
\t\tcueBar->addWidget(clearPreferred);
\t\tcueBar->addWidget(cueA);
\t\tcueBar->addWidget(cueB);
''',
    '''\t\tcueBar->addWidget(new QLabel(T("EventDock.AngleSelector"), this));
\t\tcameraCombo = new QComboBox(this);
\t\tcameraCombo->setMinimumContentsLength(18);
\t\tcameraCombo->setToolTip(T("EventDock.AngleSelector.Tooltip"));
\t\tcueBar->addWidget(cameraCombo, 1);
\t\tauto *cueA = new QPushButton(T("EventDock.CueA"), this);
\t\tauto *cueB = new QPushButton(T("EventDock.CueB"), this);
\t\tcueBar->addWidget(cueA);
\t\tcueBar->addWidget(cueB);
''',
    "remove Preferred buttons",
)

cpp = replace_once(
    cpp,
    '''\t\tconnect(setPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(false); });
\t\tconnect(clearPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(true); });
''',
    "",
    "remove Preferred connections",
)

cpp = replace_once(
    cpp,
    '''\t\tconnect(cameraCombo, &QComboBox::currentIndexChanged, this, [this](int) {
\t\t\tif (!replayPlayoutActive()) {
\t\t\t\tpreviewSelectedEvent(true);
\t\t\t\tsyncTimeline();
\t\t\t}
\t\t});
''',
    '''\t\tconnect(cameraCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
\t\t\tif (index < 0)
\t\t\t\treturn;
\t\t\tselectAngle(selectedCamera());
\t\t});
''',
    "camera combo behavior",
)

old_selected = '''\tQString selectedCamera() const { return cameraCombo ? cameraCombo->currentData().toString() : QString(); }
'''
new_selected = '''\tQString selectedCamera() const { return cameraCombo ? cameraCombo->currentData().toString() : QString(); }

\tQString eventPreferredCamera(const sr_event_record &event) const
\t{
\t\tif (!controller || !event.preferred_camera_id)
\t\t\treturn QString();
\t\tchar *name = nullptr;
\t\tQString camera;
\t\tif (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &name) && name)
\t\t\tcamera = QString::fromUtf8(name);
\t\tbfree(name);
\t\treturn camera;
\t}

\tQString eventAngleText(const sr_event_record &event) const
\t{
\t\tconst QString camera = eventPreferredCamera(event);
\t\treturn camera.isEmpty() ? T("EventDock.AngleAuto") : camera;
\t}

\tbool cameraHasEventCoverage(const QString &camera, const sr_event_record &event,
\t\t\t\t    enum sr_replay_coverage wanted) const
\t{
\t\tif (camera.isEmpty())
\t\t\treturn false;
\t\tsr_replay_coverage_info coverage = {};
\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\treturn sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns, &coverage) &&
\t\t       coverage.coverage == wanted;
\t}

\tQString automaticCameraForEvent(const sr_event_record &event, enum sr_replay_bus bus) const
\t{
\t\tQStringList candidates;
\t\tauto add = [&candidates](const QString &camera) {
\t\t\tif (!camera.isEmpty() && !candidates.contains(camera))
\t\t\t\tcandidates.append(camera);
\t\t};
\t\tsr_replay_channel_state current = {};
\t\tif (sr_replay_channel_get_state(bus, &current) && current.cued && current.camera_name[0])
\t\t\tadd(QString::fromUtf8(current.camera_name));
\t\tfor (const QString &camera : captureCameraNames())
\t\t\tadd(camera);

\t\tfor (enum sr_replay_coverage wanted : {SR_REPLAY_COVERAGE_FULL, SR_REPLAY_COVERAGE_PARTIAL}) {
\t\t\tfor (const QString &camera : candidates) {
\t\t\t\tif (cameraHasEventCoverage(camera, event, wanted))
\t\t\t\t\treturn camera;
\t\t\t}
\t\t}
\t\treturn QString();
\t}

\tvoid syncEventAngleSelection()
\t{
\t\tif (!cameraCombo)
\t\t\treturn;
\t\tQString desired;
\t\tconst uint64_t eventId = selectedEventId();
\t\tif (replayPlayoutActive()) {
\t\t\tsr_replay_channel_state state = {};
\t\t\tif (sr_replay_channel_get_state(transportBus(), &state) && state.cued &&
\t\t\t    (!eventId || state.event_id == eventId))
\t\t\t\tdesired = QString::fromUtf8(state.camera_name);
\t\t} else if (controller && eventId) {
\t\t\tsr_event_record event = {};
\t\t\tif (sr_event_controller_get_event(controller, eventId, &event)) {
\t\t\t\tdesired = eventPreferredCamera(event);
\t\t\t\tsr_event_controller_free_event(&event);
\t\t\t}
\t\t}

\t\tconst QSignalBlocker blocker(cameraCombo);
\t\tint index = desired.isEmpty() ? cameraCombo->findData(QString()) : cameraCombo->findData(desired);
\t\tif (index < 0)
\t\t\tindex = cameraCombo->findData(QString());
\t\tif (index >= 0)
\t\t\tcameraCombo->setCurrentIndex(index);
\t}
'''
cpp = replace_once(cpp, old_selected, new_selected, "event angle helpers")

start = '''\tQString eventGalleryText(const sr_event_record &event) const

\t{
'''
end = '''\n\tbool makeEventThumbnailTask'''
new_gallery = '''\tQString eventGalleryText(const sr_event_record &event) const

\t{
\t\tconst QString name = QString::fromUtf8(event.name ? event.name : "").trimmed();
\t\tconst QString tag = QString::fromUtf8(event.tag ? event.tag : "").trimmed();
\t\tQString first = QStringLiteral("#%1").arg(event.id);
\t\tif (!name.isEmpty())
\t\t\tfirst += QStringLiteral("  ") + name;
\t\tconst bool inherited = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL &&
\t\t\t\t       !event.speed_override;
\t\tconst QString speed = inherited ? QStringLiteral("--")
\t\t\t\t\t\t: QStringLiteral("%1%").arg(event.speed_percent, 0, 'f', 0);
\t\tQString second = QStringLiteral("%1  ·  %2  ·  %3")
\t\t\t\t\t .arg(durationText(event))
\t\t\t\t\t .arg(speed)
\t\t\t\t\t .arg(eventAngleText(event));
\t\tif (!tag.isEmpty())
\t\t\tsecond += QStringLiteral("  ·  ") + tag;
\t\treturn first + QStringLiteral("\\n") + second;
\t}
'''
cpp = replace_between(cpp, start, end, new_gallery, "gallery angle text")

start = '''\tvoid refreshCameras()
\t{
'''
end = '''\n\tuint64_t angleEventId() const'''
new_refresh_cameras = '''\tvoid refreshCameras()
\t{
\t\tif (!cameraCombo)
\t\t\treturn;
\t\tconst QStringList names = captureCameraNames();
\t\tQStringList current;
\t\tfor (int i = 0; i < cameraCombo->count(); i++) {
\t\t\tconst QString value = cameraCombo->itemData(i).toString();
\t\t\tif (!value.isEmpty())
\t\t\t\tcurrent.append(value);
\t\t}

\t\tif (cameraCombo->count() == 0 || current != names) {
\t\t\tconst QSignalBlocker blocker(cameraCombo);
\t\t\tcameraCombo->clear();
\t\t\tcameraCombo->addItem(T("EventDock.AngleAuto"), QString());
\t\t\tfor (const QString &name : names)
\t\t\t\tcameraCombo->addItem(name, name);
\t\t\trebuildAngleButtons(names);
\t\t}

\t\tsyncEventAngleSelection();
\t\trefreshAngleCoverage();
\t}
'''
cpp = replace_between(cpp, start, end, new_refresh_cameras, "refresh cameras with AUTO")

cpp = replace_once(
    cpp,
    '''\t\tQString preferredCamera;
\t\tif (haveEvent && event.preferred_camera_id) {
\t\t\tchar *preferredName = nullptr;
\t\t\tif (sr_event_controller_get_camera_name(controller, event.preferred_camera_id,
\t\t\t\t\t\t\t\t&preferredName) &&
\t\t\t    preferredName)
\t\t\t\tpreferredCamera = QString::fromUtf8(preferredName);
\t\t\tbfree(preferredName);
\t\t}
''',
    '''\t\tconst QString preferredCamera = haveEvent ? eventPreferredCamera(event) : QString();
''',
    "preferred camera lookup",
)

cpp = replace_once(
    cpp,
    '''\t\t\tconst QString preferredMarker = camera == preferredCamera ? QStringLiteral("★ ") : QString();
\t\t\tbutton->setText(QStringLiteral("%1%2 %3").arg(preferredMarker, marker, camera));
\t\t\tif (camera == preferredCamera)
\t\t\t\ttooltip += QStringLiteral(" — ") + T("EventDock.Preferred");
\t\t\ttooltip += QStringLiteral("\\n") + T("EventDock.AnglePreviewHint");
\t\t\tbutton->setProperty("coverageTooltip", tooltip);
\t\t\tbutton->setToolTip(tooltip);
''',
    '''\t\t\tconst bool storedAngle = !preferredCamera.isEmpty() && camera == preferredCamera;
\t\t\tbutton->setProperty("storedAngle", storedAngle);
\t\t\tconst QString selectedMarker = storedAngle ? QStringLiteral("✓ ") : QString();
\t\t\tbutton->setText(QStringLiteral("%1%2 %3").arg(selectedMarker, marker, camera));
\t\t\tif (storedAngle)
\t\t\t\ttooltip += QStringLiteral(" — ") + T("EventDock.AngleStored");
\t\t\ttooltip += QStringLiteral("\\n") + T("EventDock.AnglePreviewHint");
\t\t\tbutton->setProperty("coverageTooltip", tooltip);
\t\t\tbutton->setToolTip(tooltip);
''',
    "angle card stored marker",
)

cpp = replace_once(
    cpp,
    '''\t\tif (haveEvent)
\t\t\tsr_event_controller_free_event(&event);
\t\tsyncAngleButtonState();
\t}
''',
    '''\t\tif (haveEvent)
\t\t\tsr_event_controller_free_event(&event);
\t\tsyncEventAngleSelection();
\t\tsyncAngleButtonState();
\t}
''',
    "sync angle combo from event",
)

start = '''\tvoid syncAngleButtonState()
\t{
'''
end = '''\n\tQString playlistSummary'''
new_angle_functions = '''\tvoid syncAngleButtonState()
\t{
\t\tconst uint64_t eventId = angleEventId();
\t\tsr_replay_channel_state state = {};
\t\tconst bool haveState = sr_replay_channel_get_state(transportBus(), &state);
\t\tconst bool sameEvent = haveState && state.cued && eventId && state.event_id == eventId;
\t\tconst QString activeCamera = sameEvent ? QString::fromUtf8(state.camera_name) : QString();
\t\tconst bool playout = replayPlayoutActive();

\t\tfor (QToolButton *button : angleButtons) {
\t\t\tconst auto coverage = static_cast<sr_replay_coverage>(button->property("coverage").toInt());
\t\t\tconst uint64_t playableIn = button->property("playableInNs").toULongLong();
\t\t\tconst uint64_t playableOut = button->property("playableOutNs").toULongLong();
\t\t\tconst QString camera = button->property("cameraName").toString();
\t\t\tconst bool atPlayhead = !sameEvent ||
\t\t\t\t\t\t(state.playhead_ns >= playableIn && state.playhead_ns <= playableOut);
\t\t\tbutton->setEnabled(eventId && coverage != SR_REPLAY_COVERAGE_NONE && atPlayhead);
\t\t\tbutton->setChecked(playout ? (sameEvent && activeCamera == camera)
\t\t\t\t\t\t   : button->property("storedAngle").toBool());
\t\t\tbutton->setToolTip(button->property("coverageTooltip").toString());
\t\t\tif (sameEvent && coverage != SR_REPLAY_COVERAGE_NONE && !atPlayhead)
\t\t\t\tbutton->setToolTip(T("EventDock.AngleUnavailable").arg(camera));
\t\t}
\t}

\tbool storeEventAngle(uint64_t eventId, const QString &camera)
\t{
\t\tif (!controller || !eventId)
\t\t\treturn false;
\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\tconst char *name = camera.isEmpty() ? nullptr : cameraUtf8.constData();
\t\tif (!sr_event_controller_set_preferred_camera(controller, eventId, name)) {
\t\t\tsetStatus("EventDock.AngleSaveFailed");
\t\t\treturn false;
\t\t}
\t\teventThumbnailCache.erase(eventId);
\t\treturn true;
\t}

\tvoid selectAutoAngle()
\t{
\t\tconst uint64_t eventId = selectedEventId();
\t\tif (!controller || !eventId) {
\t\t\tsetStatus("EventDock.NoEventSelected");
\t\t\treturn;
\t\t}
\t\tif (replayPlayoutActive()) {
\t\t\tsetStatus("EventDock.AngleAutoEditOnly");
\t\t\tsyncEventAngleSelection();
\t\t\treturn;
\t\t}

\t\tconst uint64_t target = editTimeline ? editTimeline->playheadTimestamp() : 0;
\t\tif (!storeEventAngle(eventId, QString())) {
\t\t\tsyncEventAngleSelection();
\t\t\treturn;
\t\t}
\t\teditPreviewEventId = 0;
\t\teditPreviewCamera.clear();
\t\trefresh(eventId);
\t\trefreshAngleCoverage();
\t\tpreviewSelectedEvent(true);
\t\tif (target)
\t\t\tpreviewSeekTo(target);
\t\tstatus->setText(T("EventDock.AngleAutoSaved").arg(eventId));
\t\tsyncTimeline();
\t}

\tvoid selectAngle(const QString &camera)
\t{
\t\tif (camera.isEmpty()) {
\t\t\tselectAutoAngle();
\t\t\treturn;
\t\t}

\t\tconst enum sr_replay_bus bus = transportBus();
\t\tuint64_t eventId = selectedEventId();
\t\tsr_replay_channel_state state = {};
\t\tconst bool haveState = sr_replay_channel_get_state(bus, &state);
\t\tif (!eventId && haveState && state.cued)
\t\t\teventId = state.event_id;
\t\tif (!eventId) {
\t\t\tsetStatus("EventDock.NoEventSelected");
\t\t\treturn;
\t\t}

\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\tbool ok = false;
\t\tif (!replayPlayoutActive()) {
\t\t\tupdateEditTimelineBounds();
\t\t\tsr_event_record event = {};
\t\t\tif (!controller || !sr_event_controller_get_event(controller, eventId, &event)) {
\t\t\t\tsetStatus("EventDock.Failed");
\t\t\t\treturn;
\t\t\t}
\t\t\tsr_replay_coverage_info coverage = {};
\t\t\tif (!sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns, &coverage) ||
\t\t\t    coverage.coverage == SR_REPLAY_COVERAGE_NONE) {
\t\t\t\tsr_event_controller_free_event(&event);
\t\t\t\tstatus->setText(T("EventDock.CueNoCoverage").arg(camera));
\t\t\t\tsyncEventAngleSelection();
\t\t\t\treturn;
\t\t\t}

\t\t\tuint64_t target = editTimeline ? editTimeline->playheadTimestamp() : event.in_ns;
\t\t\tif (!target)
\t\t\t\ttarget = event.in_ns;
\t\t\tconst uint64_t rangeIn = editTimelineHaveBounds ? editTimelineStartNs : event.in_ns;
\t\t\tconst uint64_t rangeOut = editTimelineHaveBounds ? editTimelineEndNs : event.out_ns;
\t\t\ttarget = qBound(rangeIn, target, rangeOut);
\t\t\tif (!storeEventAngle(eventId, camera)) {
\t\t\t\tsr_event_controller_free_event(&event);
\t\t\t\tsyncEventAngleSelection();
\t\t\t\treturn;
\t\t\t}

\t\t\tif (haveState && state.cued && state.preview_mode && state.event_id == eventId)
\t\t\t\tok = sr_replay_channel_switch_camera(bus, cameraUtf8.constData());
\t\t\tif (ok) {
\t\t\t\teditPreviewEventId = eventId;
\t\t\t\teditPreviewBus = bus;
\t\t\t\teditPreviewCamera = camera;
\t\t\t\tsr_replay_channel_pause(bus, true);
\t\t\t\tsr_replay_channel_seek(bus, target);
\t\t\t} else {
\t\t\t\tok = cueEditPreviewAt(camera, eventId, target, rangeIn, rangeOut);
\t\t\t}
\t\t\tsr_event_controller_free_event(&event);

\t\t\t{
\t\t\t\tconst QSignalBlocker blocker(cameraCombo);
\t\t\t\tconst int comboIndex = cameraCombo ? cameraCombo->findData(camera) : -1;
\t\t\t\tif (comboIndex >= 0)
\t\t\t\t\tcameraCombo->setCurrentIndex(comboIndex);
\t\t\t}
\t\t\trefresh(eventId);
\t\t\trefreshAngleCoverage();
\t\t\tstatus->setText(T("EventDock.AngleSaved").arg(eventId).arg(camera));
\t\t\tif (!ok)
\t\t\t\tstatus->setText(T("EventDock.AngleSavedPreviewFailed").arg(eventId).arg(camera));
\t\t\tsyncTransportControls();
\t\t\tsyncTimeline();
\t\t\treturn;
\t\t}

\t\tconst bool switching = haveState && state.cued && state.event_id == eventId;
\t\tok = switching ? sr_replay_channel_switch_camera(bus, cameraUtf8.constData())
\t\t\t       : sr_replay_channel_cue(bus, eventId, cameraUtf8.constData());
\t\tif (!ok) {
\t\t\tsetStatus("EventDock.AngleSwitchFailed");
\t\t\trefreshAngleCoverage();
\t\t\treturn;
\t\t}

\t\t{
\t\t\tconst QSignalBlocker blocker(cameraCombo);
\t\t\tconst int comboIndex = cameraCombo ? cameraCombo->findData(camera) : -1;
\t\t\tif (comboIndex >= 0)
\t\t\t\tcameraCombo->setCurrentIndex(comboIndex);
\t\t}
\t\tstatus->setText(T("EventDock.AngleLiveSwitched")
\t\t\t\t\t.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
\t\t\t\t\t.arg(camera));
\t\tsyncTransportControls();
\t\trefreshAngleCoverage();
\t}
'''
cpp = replace_between(cpp, start, end, new_angle_functions, "angle selection semantics")

start = '''\tbool cueSelected(enum sr_replay_bus bus)
\t{
'''
end = '''\n\tvoid playSelectedEvent()'''
new_cue = '''\tbool cueSelected(enum sr_replay_bus bus)
\t{
\t\tconst uint64_t eventId = selectedEventId();
\t\tif (!eventId) {
\t\t\tsetStatus("EventDock.NoEventSelected");
\t\t\treturn false;
\t\t}

\t\tsr_event_record event = {};
\t\tif (!controller || !sr_event_controller_get_event(controller, eventId, &event)) {
\t\t\tsetStatus("EventDock.Failed");
\t\t\treturn false;
\t\t}
\t\tQString camera = eventPreferredCamera(event);
\t\tif (camera.isEmpty())
\t\t\tcamera = automaticCameraForEvent(event, bus);
\t\tsr_event_controller_free_event(&event);
\t\tif (camera.isEmpty()) {
\t\t\tsetStatus("EventDock.CueFailed");
\t\t\treturn false;
\t\t}

\t\tQToolButton *angle = angleButton(camera);
\t\tif (angle && angle->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE) {
\t\t\tstatus->setText(T("EventDock.CueNoCoverage").arg(camera));
\t\t\treturn false;
\t\t}
\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\tsr_replay_playlist_stop(bus);
\t\tif (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData())) {
\t\t\tsetStatus("EventDock.CueFailed");
\t\t\treturn false;
\t\t}
\t\tstatus->setText(T("EventDock.Cued")
\t\t\t\t\t.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
\t\t\t\t\t.arg(eventId));
\t\tif (transportBus() == bus)
\t\t\tsyncTransportControls();
\t\trefreshTransportStatus();
\t\treturn true;
\t}
'''
cpp = replace_between(cpp, start, end, new_cue, "cue selected auto angle")

cpp = replace_once(
    cpp,
    '''\t\tconst QString camera = selectedCamera();
\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\tconst char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
\t\tbool transitionRequested = false;
\t\tconst bool crossBus = eventTransitionCrossBus(&transitionRequested);
\t\tif (!controller || !sr_replay_playlist_start_events_with_transitions(
\t\t\t\t\t   bus, currentList(), eventIds.data(), eventIds.size(), preferred, crossBus)) {
''',
    '''\t\tbool transitionRequested = false;
\t\tconst bool crossBus = eventTransitionCrossBus(&transitionRequested);
\t\tif (!controller || !sr_replay_playlist_start_events_with_transitions(
\t\t\t\t\t   bus, currentList(), eventIds.data(), eventIds.size(), nullptr, crossBus)) {
''',
    "selected event sequence uses per-event angles",
)

cpp = replace_once(
    cpp,
    '''\tvoid startPlaylist(enum sr_replay_bus bus)
\t{
\t\tconst QString camera = selectedCamera();
\t\tconst QByteArray cameraUtf8 = camera.toUtf8();
\t\tconst char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
\t\tbool transitionRequested = false;
\t\tconst bool crossBus = eventTransitionCrossBus(&transitionRequested);
\t\tif (!controller ||
\t\t    !sr_replay_playlist_start_with_transitions(bus, currentList(), preferred, crossBus)) {
''',
    '''\tvoid startPlaylist(enum sr_replay_bus bus)
\t{
\t\tbool transitionRequested = false;
\t\tconst bool crossBus = eventTransitionCrossBus(&transitionRequested);
\t\tif (!controller ||
\t\t    !sr_replay_playlist_start_with_transitions(bus, currentList(), nullptr, crossBus)) {
''',
    "playlist uses per-event angles",
)

cpp = replace_once(
    cpp,
    '''\t\t} else {
\t\t\tQString camera = preferred;
\t\t\tif (!fullAngles.contains(camera))
\t\t\t\tcamera = selectedCamera();
\t\t\tif (!fullAngles.contains(camera))
\t\t\t\tcamera = fullAngles.first();
''',
    '''\t\t} else {
\t\t\tQString camera = preferred;
\t\t\tif (!fullAngles.contains(camera))
\t\t\t\tcamera = fullAngles.first();
''',
    "export event angle",
)

old_angle_table = '''\t\t\tQString preferredCamera;
\t\t\tif (event.preferred_camera_id) {
\t\t\t\tchar *preferredName = nullptr;
\t\t\t\tif (sr_event_controller_get_camera_name(controller, event.preferred_camera_id,
\t\t\t\t\t\t\t\t\t&preferredName) &&
\t\t\t\t    preferredName)
\t\t\t\t\tpreferredCamera = QString::fromUtf8(preferredName);
\t\t\t\tbfree(preferredName);
\t\t\t}
\t\t\tQString angleText;
\t\t\tif (!preferredCamera.isEmpty())
\t\t\t\tangleText = QStringLiteral("★ ") + preferredCamera;
\t\t\telse if (!selectedCamera().isEmpty())
\t\t\t\tangleText = T("EventDock.AngleAutoCurrent").arg(selectedCamera());
\t\t\telse
\t\t\t\tangleText = T("EventDock.AngleAuto");
\t\t\tauto *angle = new QTableWidgetItem(angleText);
\t\t\tangle->setFlags(angle->flags() & ~Qt::ItemIsEditable);
\t\t\tangle->setToolTip(preferredCamera.isEmpty()
\t\t\t\t\t\t  ? T("EventDock.AngleAuto.Tooltip")
\t\t\t\t\t\t  : T("EventDock.AnglePreferred.Tooltip").arg(preferredCamera));
\t\t\ttable->setItem((int)i, 4, angle);
'''
new_angle_table = '''\t\t\tconst QString preferredCamera = eventPreferredCamera(event);
\t\t\tauto *angle = new QTableWidgetItem(eventAngleText(event));
\t\t\tangle->setFlags(angle->flags() & ~Qt::ItemIsEditable);
\t\t\tangle->setToolTip(preferredCamera.isEmpty()
\t\t\t\t\t\t  ? T("EventDock.AngleAuto.Tooltip")
\t\t\t\t\t\t  : T("EventDock.AnglePreferred.Tooltip").arg(preferredCamera));
\t\t\ttable->setItem((int)i, 4, angle);
'''
cpp = replace_once(cpp, old_angle_table, new_angle_table, "event list angle column")

CPP.write_text(cpp, encoding="utf-8")


def update_locale(path: Path, spanish: bool) -> None:
    text = path.read_text(encoding="utf-8")
    if spanish:
        replacements = {
            'EventDock.ViewThumbnails.Tooltip="Bin visual de eventos. Arrastrá un cuadro de selección o usá Ctrl+clic para elegir varias tarjetas y luego Play Selected. La miniatura usa la cámara preferida cuando está disponible."':
                'EventDock.ViewThumbnails.Tooltip="Bin visual de eventos. Arrastrá un cuadro de selección o usá Ctrl+clic para elegir varias tarjetas y luego Play Selected. La miniatura usa el ángulo guardado del Event; en AUTO usa una cámara con cobertura disponible."',
            'EventDock.ExportPreferred="Ángulo preferido"': 'EventDock.ExportPreferred="Ángulo del Event"',
            'EventDock.AngleAuto.Tooltip="No hay un ángulo preferido guardado para este Event. El Cue manual usa la selección Camera actual; las secuencias automáticas pueden mantener el ángulo actual del bus y usar otra cámara con cobertura disponible."':
                'EventDock.AngleAuto.Tooltip="AUTO: este Event no tiene un ángulo fijo. Al hacer Cue/Play se elige automáticamente una cámara con cobertura utilizable, priorizando la continuidad del ángulo actual del bus."',
            'EventDock.AnglePreferred.Tooltip="Este Event está fijado a %1 como su ángulo preferido de reproducción. Haz clic en otro ángulo para previsualizarlo; pulsa ★ Preferred para guardar otro."':
                'EventDock.AnglePreferred.Tooltip="%1 es el ángulo guardado de este Event y se usará para Cue/Play mientras tenga cobertura utilizable."',
            'EventDock.AnglePreviewHint="Haz clic para previsualizar este ángulo en el playhead actual. Esto no cambia el ángulo Preferred del Event hasta que pulses ★ Preferred."':
                'EventDock.AnglePreviewHint="EDIT: haz clic para seleccionar, guardar y previsualizar este ángulo. Elige AUTO en Angle para volver a la selección automática. Durante PLAYOUT, un clic solo cambia el ángulo en vivo y no modifica el Event."',
        }
        additions = '''\nEventDock.AngleSelector="Ángulo"\nEventDock.AngleSelector.Tooltip="Ángulo de reproducción del Event seleccionado. AUTO elige una cámara al reproducir; seleccionar una cámara la guarda inmediatamente en este Event."\nEventDock.AngleStored="ángulo guardado del Event"\nEventDock.AngleSaved="Event #%1 · ángulo: %2"\nEventDock.AngleAutoSaved="Event #%1 · ángulo: AUTO"\nEventDock.AngleSaveFailed="No se pudo guardar el ángulo del Event"\nEventDock.AngleSavedPreviewFailed="Event #%1 · ángulo %2 guardado, pero el fotograma actual no se pudo cargar en preview"\nEventDock.AngleLiveSwitched="Bus %1 · cambio de ángulo en vivo: %2"\nEventDock.AngleAutoEditOnly="AUTO es una propiedad del Event y solo se puede cambiar en modo EDIT; durante PLAYOUT selecciona una cámara concreta para cambiar el ángulo en vivo"\n'''
    else:
        replacements = {
            'EventDock.ViewThumbnails.Tooltip="Visual Event bin. Drag a selection box or Ctrl-click multiple cards, then use Play Selected. The thumbnail uses the preferred camera when available, otherwise the first camera with usable coverage."':
                'EventDock.ViewThumbnails.Tooltip="Visual Event bin. Drag a selection box or Ctrl-click multiple cards, then use Play Selected. The thumbnail uses the Event\'s saved angle; AUTO uses a camera with usable coverage."',
            'EventDock.ExportPreferred="Preferred angle"': 'EventDock.ExportPreferred="Event angle"',
            'EventDock.AngleAuto.Tooltip="No preferred angle is stored for this Event. Manual Cue uses the current Camera selection; automatic sequences may keep the current bus angle and fall back to another camera with usable coverage."':
                'EventDock.AngleAuto.Tooltip="AUTO: this Event has no fixed angle. Cue/Play automatically chooses a camera with usable coverage, preferring continuity with the current bus angle."',
            'EventDock.AnglePreferred.Tooltip="This Event is pinned to %1 as its preferred playback angle. Click another angle to preview it; press ★ Preferred to store a different one."':
                'EventDock.AnglePreferred.Tooltip="%1 is the saved angle for this Event and is used for Cue/Play while it has usable coverage."',
            'EventDock.AnglePreviewHint="Click to preview this angle at the current playhead. This does not change the Event\'s Preferred angle until you press ★ Preferred."':
                'EventDock.AnglePreviewHint="EDIT: click to select, save, and preview this angle. Choose AUTO in Angle to restore automatic selection. During PLAYOUT, a click is a live angle switch only and does not edit the Event."',
        }
        additions = '''\nEventDock.AngleSelector="Angle"\nEventDock.AngleSelector.Tooltip="Playback angle for the selected Event. AUTO chooses a camera at play time; choosing a camera saves it immediately on this Event."\nEventDock.AngleStored="saved Event angle"\nEventDock.AngleSaved="Event #%1 · angle: %2"\nEventDock.AngleAutoSaved="Event #%1 · angle: AUTO"\nEventDock.AngleSaveFailed="Could not save the Event angle"\nEventDock.AngleSavedPreviewFailed="Event #%1 · angle %2 saved, but the current frame could not be loaded into preview"\nEventDock.AngleLiveSwitched="Bus %1 · live angle switch: %2"\nEventDock.AngleAutoEditOnly="AUTO is an Event property and can only be changed in EDIT mode; during PLAYOUT choose a concrete camera for a live angle switch"\n'''

    for old, new in replacements.items():
        if old not in text:
            raise RuntimeError(f"{path.name}: locale string not found: {old[:80]}")
        text = text.replace(old, new, 1)
    if "EventDock.AngleSelector=" not in text:
        text = text.rstrip() + "\n" + additions
    path.write_text(text, encoding="utf-8")


update_locale(EN, False)
update_locale(ES, True)
