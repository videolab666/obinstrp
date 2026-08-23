from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


dock = Path("src/sr-event-dock.cpp")

replace_once(
    dock,
    "#include <QPushButton>\n#include <QStringList>\n",
    "#include <QPushButton>\n#include <QSignalBlocker>\n#include <QSlider>\n#include <QStringList>\n",
)

replace_once(
    dock,
    "QString durationText(const sr_event_record &event)\n{\n\tif (event.out_ns < event.in_ns)\n\t\treturn QStringLiteral(\"-\");\n\treturn QString::number((double)(event.out_ns - event.in_ns) / 1e9, 'f', 3) + QStringLiteral(\" s\");\n}\n",
    "QString durationText(const sr_event_record &event)\n{\n\tif (event.out_ns < event.in_ns)\n\t\treturn QStringLiteral(\"-\");\n\treturn QString::number((double)(event.out_ns - event.in_ns) / 1e9, 'f', 3) + QStringLiteral(\" s\");\n}\n\nQString replayClockText(uint64_t ns)\n{\n\tconst uint64_t totalMs = ns / 1000000ULL;\n\tconst uint64_t minutes = totalMs / 60000ULL;\n\tconst uint64_t seconds = (totalMs / 1000ULL) % 60ULL;\n\tconst uint64_t millis = totalMs % 1000ULL;\n\treturn QStringLiteral(\"%1:%2.%3\")\n\t\t.arg(minutes, 2, 10, QChar('0'))\n\t\t.arg(seconds, 2, 10, QChar('0'))\n\t\t.arg(millis, 3, 10, QChar('0'));\n}\n",
)

replace_once(
    dock,
    "\t\tcueBar->addWidget(audioCombo);\n\t\troot->addLayout(cueBar);\n\n\t\tauto *takeBar = new QHBoxLayout();\n",
    "\t\tcueBar->addWidget(audioCombo);\n\t\troot->addLayout(cueBar);\n\n\t\tauto *timelineBar = new QHBoxLayout();\n\t\ttimelineBar->addWidget(new QLabel(T(\"EventDock.Timeline\"), this));\n\t\ttimelineSlider = new QSlider(Qt::Horizontal, this);\n\t\ttimelineSlider->setRange(0, 10000);\n\t\ttimelineSlider->setSingleStep(1);\n\t\ttimelineSlider->setPageStep(100);\n\t\ttimelineSlider->setEnabled(false);\n\t\ttimelineSlider->setToolTip(T(\"EventDock.Timeline.Tooltip\"));\n\t\ttimelineBar->addWidget(timelineSlider, 1);\n\t\ttimelineTime = new QLabel(QStringLiteral(\"--:--.--- / --:--.---\"), this);\n\t\ttimelineTime->setMinimumWidth(150);\n\t\ttimelineBar->addWidget(timelineTime);\n\t\troot->addLayout(timelineBar);\n\n\t\tauto *jogShuttleBar = new QHBoxLayout();\n\t\tjogShuttleBar->addWidget(new QLabel(T(\"EventDock.Jog\"), this));\n\t\tjogSlider = new QSlider(Qt::Horizontal, this);\n\t\tjogSlider->setRange(-24, 24);\n\t\tjogSlider->setValue(0);\n\t\tjogSlider->setSingleStep(1);\n\t\tjogSlider->setPageStep(1);\n\t\tjogSlider->setToolTip(T(\"EventDock.Jog.Tooltip\"));\n\t\tjogShuttleBar->addWidget(jogSlider, 1);\n\t\tjogShuttleBar->addSpacing(8);\n\t\tjogShuttleBar->addWidget(new QLabel(T(\"EventDock.Shuttle\"), this));\n\t\tshuttleSlider = new QSlider(Qt::Horizontal, this);\n\t\tshuttleSlider->setRange(-5, 5);\n\t\tshuttleSlider->setValue(0);\n\t\tshuttleSlider->setSingleStep(1);\n\t\tshuttleSlider->setPageStep(1);\n\t\tshuttleSlider->setToolTip(T(\"EventDock.Shuttle.Tooltip\"));\n\t\tjogShuttleBar->addWidget(shuttleSlider, 1);\n\t\tshuttleValue = new QLabel(QStringLiteral(\"0\"), this);\n\t\tshuttleValue->setMinimumWidth(48);\n\t\tjogShuttleBar->addWidget(shuttleValue);\n\t\troot->addLayout(jogShuttleBar);\n\n\t\tauto *takeBar = new QHBoxLayout();\n",
)

replace_once(
    dock,
    "\t\tconnect(audioCombo, &QComboBox::currentIndexChanged, this, [this](int index) {\n\t\t\tif (index >= 0)\n\t\t\t\tsr_replay_channel_set_audio_mode(\n\t\t\t\t\ttransportBus(),\n\t\t\t\t\tstatic_cast<sr_replay_audio_mode>(audioCombo->itemData(index).toInt()));\n\t\t\trefreshTransportStatus();\n\t\t});\n\t\tconnect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });\n",
    "\t\tconnect(audioCombo, &QComboBox::currentIndexChanged, this, [this](int index) {\n\t\t\tif (index >= 0)\n\t\t\t\tsr_replay_channel_set_audio_mode(\n\t\t\t\t\ttransportBus(),\n\t\t\t\t\tstatic_cast<sr_replay_audio_mode>(audioCombo->itemData(index).toInt()));\n\t\t\trefreshTransportStatus();\n\t\t});\n\t\tconnect(timelineSlider, &QSlider::sliderPressed, this, [this]() {\n\t\t\ttimelineDragging = true;\n\t\t\tsr_replay_channel_pause(transportBus(), true);\n\t\t});\n\t\tconnect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });\n\t\tconnect(timelineSlider, &QSlider::sliderReleased, this, [this]() {\n\t\t\tseekTimeline(timelineSlider->value());\n\t\t\ttimelineDragging = false;\n\t\t\tsyncTimeline();\n\t\t});\n\t\tconnect(jogSlider, &QSlider::sliderPressed, this, [this]() { jogLastValue = jogSlider->value(); });\n\t\tconnect(jogSlider, &QSlider::sliderMoved, this, [this](int value) { jogMoved(value); });\n\t\tconnect(jogSlider, &QSlider::sliderReleased, this, [this]() {\n\t\t\tconst QSignalBlocker blocker(jogSlider);\n\t\t\tjogSlider->setValue(0);\n\t\t\tjogLastValue = 0;\n\t\t});\n\t\tconnect(shuttleSlider, &QSlider::valueChanged, this, [this](int value) { applyShuttle(value); });\n\t\tconnect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });\n",
)

replace_once(
    dock,
    "\t\trefreshTimer->start();\n\n\t\tif (controller)\n",
    "\t\trefreshTimer->start();\n\n\t\ttransportTimer = new QTimer(this);\n\t\ttransportTimer->setInterval(100);\n\t\tconnect(transportTimer, &QTimer::timeout, this, [this]() {\n\t\t\trefreshTransportStatus();\n\t\t\tsyncTimeline();\n\t\t});\n\t\ttransportTimer->start();\n\n\t\tif (controller)\n",
)

replace_once(
    dock,
    "\tvoid cueSelected(enum sr_replay_bus bus)\n",
    "\tvoid syncTimeline()\n\t{\n\t\tif (!timelineSlider || !timelineTime)\n\t\t\treturn;\n\n\t\tsr_replay_channel_state state = {};\n\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued || state.out_ns <= state.in_ns) {\n\t\t\ttimelineSlider->setEnabled(false);\n\t\t\tif (!timelineDragging)\n\t\t\t\ttimelineSlider->setValue(0);\n\t\t\ttimelineTime->setText(QStringLiteral(\"--:--.--- / --:--.---\"));\n\t\t\treturn;\n\t\t}\n\n\t\ttimelineSlider->setEnabled(true);\n\t\tconst uint64_t duration = state.out_ns - state.in_ns;\n\t\tconst uint64_t position =\n\t\t\tstate.playhead_ns <= state.in_ns ? 0 : state.playhead_ns >= state.out_ns ? duration\n\t\t\t\t\t\t\t\t\t      : state.playhead_ns - state.in_ns;\n\t\tif (!timelineDragging) {\n\t\t\tconst int sliderValue = (int)((long double)position * 10000.0L / (long double)duration);\n\t\t\ttimelineSlider->setValue(sliderValue);\n\t\t}\n\t\ttimelineTime->setText(replayClockText(position) + QStringLiteral(\" / \") + replayClockText(duration));\n\t}\n\n\tvoid seekTimeline(int value)\n\t{\n\t\tsr_replay_channel_state state = {};\n\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued || state.out_ns <= state.in_ns)\n\t\t\treturn;\n\n\t\tif (value < 0)\n\t\t\tvalue = 0;\n\t\tif (value > 10000)\n\t\t\tvalue = 10000;\n\t\tconst uint64_t duration = state.out_ns - state.in_ns;\n\t\tconst uint64_t offset = (uint64_t)((long double)duration * (long double)value / 10000.0L);\n\t\tconst uint64_t target = offset >= duration ? state.out_ns : state.in_ns + offset;\n\t\tsr_replay_channel_pause(transportBus(), true);\n\t\tsr_replay_channel_seek(transportBus(), target);\n\t\ttimelineTime->setText(replayClockText(offset > duration ? duration : offset) + QStringLiteral(\" / \") +\n\t\t\t\t      replayClockText(duration));\n\t}\n\n\tvoid jogMoved(int value)\n\t{\n\t\tconst int delta = value - jogLastValue;\n\t\tjogLastValue = value;\n\t\tif (!delta)\n\t\t\treturn;\n\n\t\tconst int direction = delta > 0 ? 1 : -1;\n\t\tif (!sr_replay_channel_step_frames(transportBus(), direction))\n\t\t\tsetStatus(\"EventDock.FrameStepFailed\");\n\t\trefreshTransportStatus();\n\t\tsyncTimeline();\n\t}\n\n\tstatic int shuttleSpeed(int position)\n\t{\n\t\tswitch (position < 0 ? -position : position) {\n\t\tcase 1:\n\t\t\treturn 25;\n\t\tcase 2:\n\t\t\treturn 50;\n\t\tcase 3:\n\t\t\treturn 100;\n\t\tcase 4:\n\t\t\treturn 200;\n\t\tcase 5:\n\t\t\treturn 400;\n\t\tdefault:\n\t\t\treturn 0;\n\t\t}\n\t}\n\n\tvoid applyShuttle(int position)\n\t{\n\t\tif (!shuttleSlider || !shuttleValue)\n\t\t\treturn;\n\n\t\tsr_replay_channel_state state = {};\n\t\tif (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued) {\n\t\t\tif (position != 0) {\n\t\t\t\tconst QSignalBlocker blocker(shuttleSlider);\n\t\t\t\tshuttleSlider->setValue(0);\n\t\t\t}\n\t\t\tshuttleValue->setText(QStringLiteral(\"0\"));\n\t\t\tsetStatus(\"EventDock.NoCue\");\n\t\t\treturn;\n\t\t}\n\n\t\tif (!position) {\n\t\t\tsr_replay_channel_pause(transportBus(), true);\n\t\t\tshuttleValue->setText(QStringLiteral(\"0\"));\n\t\t\trefreshTransportStatus();\n\t\t\treturn;\n\t\t}\n\n\t\tconst int speed = shuttleSpeed(position);\n\t\tif (!speed)\n\t\t\treturn;\n\t\tsr_replay_channel_set_backward(transportBus(), position < 0);\n\t\tsr_replay_channel_set_speed(transportBus(), speed);\n\t\tif (state.paused)\n\t\t\tsr_replay_channel_pause(transportBus(), false);\n\t\telse if (!state.playing)\n\t\t\tsr_replay_channel_play(transportBus());\n\t\tshuttleValue->setText(QStringLiteral(\"%1%\").arg(position < 0 ? -speed : speed));\n\t\trefreshTransportStatus();\n\t}\n\n\tvoid cueSelected(enum sr_replay_bus bus)\n",
)

replace_once(
    dock,
    "\t\tconst int audioIndex = audioCombo->findData((int)state.audio_mode);\n\t\tif (audioIndex >= 0)\n\t\t\taudioCombo->setCurrentIndex(audioIndex);\n\t\trefreshTransportStatus();\n",
    "\t\tconst int audioIndex = audioCombo->findData((int)state.audio_mode);\n\t\tif (audioIndex >= 0)\n\t\t\taudioCombo->setCurrentIndex(audioIndex);\n\n\t\tint shuttlePosition = 0;\n\t\tif (state.cued && state.playing && !state.paused) {\n\t\t\tconst int speed = (int)state.speed_percent;\n\t\t\tif (speed == 25)\n\t\t\t\tshuttlePosition = 1;\n\t\t\telse if (speed == 50)\n\t\t\t\tshuttlePosition = 2;\n\t\t\telse if (speed == 100)\n\t\t\t\tshuttlePosition = 3;\n\t\t\telse if (speed == 200)\n\t\t\t\tshuttlePosition = 4;\n\t\t\telse if (speed == 400)\n\t\t\t\tshuttlePosition = 5;\n\t\t\tif (state.backward)\n\t\t\t\tshuttlePosition = -shuttlePosition;\n\t\t}\n\t\tif (shuttleSlider) {\n\t\t\tconst QSignalBlocker blocker(shuttleSlider);\n\t\t\tshuttleSlider->setValue(shuttlePosition);\n\t\t}\n\t\tif (shuttleValue) {\n\t\t\tconst int speed = shuttleSpeed(shuttlePosition);\n\t\t\tshuttleValue->setText(shuttlePosition ? QStringLiteral(\"%1%\").arg(shuttlePosition < 0 ? -speed : speed)\n\t\t\t\t\t\t\t      : QStringLiteral(\"0\"));\n\t\t}\n\t\tsyncTimeline();\n\t\trefreshTransportStatus();\n",
)

replace_once(
    dock,
    "\tQComboBox *audioCombo = nullptr;\n\tQPushButton *reverseButton = nullptr;\n",
    "\tQComboBox *audioCombo = nullptr;\n\tQSlider *timelineSlider = nullptr;\n\tQSlider *jogSlider = nullptr;\n\tQSlider *shuttleSlider = nullptr;\n\tQLabel *timelineTime = nullptr;\n\tQLabel *shuttleValue = nullptr;\n\tQPushButton *reverseButton = nullptr;\n",
)

replace_once(
    dock,
    "\tQTimer *refreshTimer = nullptr;\n\tunsigned cameraRefreshTicks = 0;\n",
    "\tQTimer *refreshTimer = nullptr;\n\tQTimer *transportTimer = nullptr;\n\tbool timelineDragging = false;\n\tint jogLastValue = 0;\n\tunsigned cameraRefreshTicks = 0;\n",
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    'EventDock.FrameStepFailed="No adjacent frame is available inside this Event"\n',
    'EventDock.FrameStepFailed="No adjacent frame is available inside this Event"\n'
    'EventDock.Timeline="Timeline"\n'
    'EventDock.Timeline.Tooltip="Drag to scrub the selected A/B bus inside the cued Event; scrubbing pauses the transport"\n'
    'EventDock.Jog="Jog"\n'
    'EventDock.Jog.Tooltip="Drag left or right to step through recorded frames; release to return the control to center"\n'
    'EventDock.Shuttle="Shuttle"\n'
    'EventDock.Shuttle.Tooltip="Transport shuttle: reverse/forward 25, 50, 100, 200 or 400 percent; center pauses"\n',
)
