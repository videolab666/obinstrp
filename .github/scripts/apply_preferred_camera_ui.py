from pathlib import Path

path = Path("src/sr-event-dock.cpp")
text = path.read_text(encoding="utf-8")

if 'T("EventDock.SetPreferred")' not in text:
    old = '''\t\tauto *cueA = new QPushButton(T("EventDock.CueA"), this);\n\t\tauto *cueB = new QPushButton(T("EventDock.CueB"), this);\n\t\tcueBar->addWidget(cueA);\n\t\tcueBar->addWidget(cueB);\n'''
    new = '''\t\tauto *setPreferred = new QPushButton(T("EventDock.SetPreferred"), this);\n\t\tauto *clearPreferred = new QPushButton(T("EventDock.ClearPreferred"), this);\n\t\tauto *cueA = new QPushButton(T("EventDock.CueA"), this);\n\t\tauto *cueB = new QPushButton(T("EventDock.CueB"), this);\n\t\tcueBar->addWidget(setPreferred);\n\t\tcueBar->addWidget(clearPreferred);\n\t\tcueBar->addWidget(cueA);\n\t\tcueBar->addWidget(cueB);\n'''
    if old not in text:
        raise SystemExit("cue bar marker not found")
    text = text.replace(old, new, 1)

    old = '''\t\tconnect(duplicate, &QPushButton::clicked, this, [this]() { duplicateSelected(); });\n\t\tconnect(cueA, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_A); });\n'''
    new = '''\t\tconnect(duplicate, &QPushButton::clicked, this, [this]() { duplicateSelected(); });\n\t\tconnect(setPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(false); });\n\t\tconnect(clearPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(true); });\n\t\tconnect(cueA, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_A); });\n'''
    if old not in text:
        raise SystemExit("cue connect marker not found")
    text = text.replace(old, new, 1)

    old = '''\tvoid refreshAngleCoverage()\n\t{\n\t\tconst uint64_t eventId = angleEventId();\n\t\tsr_event_record event = {};\n\t\tconst bool haveEvent = controller && eventId &&\n\t\t\t\t       sr_event_controller_get_event(controller, eventId, &event);\n\n\t\tfor (QPushButton *button : angleButtons) {\n'''
    new = '''\tvoid refreshAngleCoverage()\n\t{\n\t\tconst uint64_t eventId = angleEventId();\n\t\tsr_event_record event = {};\n\t\tconst bool haveEvent = controller && eventId &&\n\t\t\t\t       sr_event_controller_get_event(controller, eventId, &event);\n\n\t\tQString preferredCamera;\n\t\tif (haveEvent && event.preferred_camera_id) {\n\t\t\tchar *preferredName = nullptr;\n\t\t\tif (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &preferredName) &&\n\t\t\t    preferredName)\n\t\t\t\tpreferredCamera = QString::fromUtf8(preferredName);\n\t\t\tbfree(preferredName);\n\t\t}\n\n\t\tfor (QPushButton *button : angleButtons) {\n'''
    if old not in text:
        raise SystemExit("coverage marker not found")
    text = text.replace(old, new, 1)

    old = '''\t\t\tbutton->setText(QStringLiteral("%1 %2").arg(marker, camera));\n\t\t\tbutton->setProperty("coverageTooltip", tooltip);\n'''
    new = '''\t\t\tconst QString preferredMarker = camera == preferredCamera ? QStringLiteral("★ ") : QString();\n\t\t\tbutton->setText(QStringLiteral("%1%2 %3").arg(preferredMarker, marker, camera));\n\t\t\tif (camera == preferredCamera)\n\t\t\t\ttooltip += QStringLiteral(" — ") + T("EventDock.Preferred");\n\t\t\tbutton->setProperty("coverageTooltip", tooltip);\n'''
    if old not in text:
        raise SystemExit("angle text marker not found")
    text = text.replace(old, new, 1)

    marker = '''\tQString playlistSummary(enum sr_replay_bus bus) const\n'''
    method = '''\tvoid setPreferredCamera(bool clear)\n\t{\n\t\tconst uint64_t eventId = selectedEventId();\n\t\tif (!controller || !eventId) {\n\t\t\tsetStatus("EventDock.NoEventSelected");\n\t\t\treturn;\n\t\t}\n\n\t\tconst QString camera = selectedCamera();\n\t\tif (!clear && camera.isEmpty()) {\n\t\t\tsetStatus("EventDock.NoCameraSelected");\n\t\t\treturn;\n\t\t}\n\t\tconst QByteArray cameraUtf8 = camera.toUtf8();\n\t\tconst char *name = clear ? nullptr : cameraUtf8.constData();\n\t\tif (!sr_event_controller_set_preferred_camera(controller, eventId, name)) {\n\t\t\tsetStatus("EventDock.PreferredFailed");\n\t\t\treturn;\n\t\t}\n\n\t\tsetStatus(clear ? "EventDock.PreferredCleared" : "EventDock.PreferredSet");\n\t\trefreshAngleCoverage();\n\t}\n\n'''
    if marker not in text:
        raise SystemExit("playlist summary marker not found")
    text = text.replace(marker, method + marker, 1)

    path.write_text(text, encoding="utf-8")

locale = Path("data/locale/en-US.ini")
loc = locale.read_text(encoding="utf-8")
if 'EventDock.SetPreferred=' not in loc:
    marker = 'EventDock.Camera="Camera"\n'
    insert = (
        'EventDock.Camera="Camera"\n'
        'EventDock.SetPreferred="★ Preferred"\n'
        'EventDock.ClearPreferred="Auto"\n'
        'EventDock.Preferred="preferred Event angle"\n'
        'EventDock.PreferredSet="Preferred camera saved for this Event"\n'
        'EventDock.PreferredCleared="Preferred camera cleared; automatic angle selection restored"\n'
        'EventDock.PreferredFailed="Could not save the preferred camera for this Event"\n'
    )
    if marker not in loc:
        raise SystemExit("locale camera marker not found")
    loc = loc.replace(marker, insert, 1)
    locale.write_text(loc, encoding="utf-8")

print("preferred camera UI applied")
