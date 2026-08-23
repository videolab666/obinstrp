from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise SystemExit(f"expected block not found in {path}: {old[:120]!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


coverage_h = r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sr_replay_coverage {
    SR_REPLAY_COVERAGE_NONE = 0,
    SR_REPLAY_COVERAGE_PARTIAL = 1,
    SR_REPLAY_COVERAGE_FULL = 2,
};

struct sr_replay_coverage_info {
    enum sr_replay_coverage coverage;
    uint64_t playable_in_ns;
    uint64_t playable_out_ns;
    bool active;
};

/* Cheap metadata-only camera probe used by the operator UI. It scans the
 * camera segment catalog but does not open a decoder. Coverage deliberately
 * follows the same first/last indexed bounds used by sr_replay_channel_cue(),
 * so an angle advertised as FULL/PARTIAL behaves the same when selected. */
bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
                              struct sr_replay_coverage_info *info);

#ifdef __cplusplus
}
#endif
'''

coverage_c = r'''/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-coverage.h"

#include "sr-segment-catalog.h"
#include "sr-session.h"

#include <util/bmem.h>

#include <limits.h>
#include <string.h>

bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
                              struct sr_replay_coverage_info *info)
{
    if (!info)
        return false;

    memset(info, 0, sizeof(*info));
    info->coverage = SR_REPLAY_COVERAGE_NONE;

    if (!camera_name || !*camera_name || event_out_ns < event_in_ns)
        return false;

    char *session_dir = sr_session_get_or_create_path();
    if (!session_dir)
        return false;

    struct sr_segment_descriptor *segments = NULL;
    size_t count = 0;
    const bool scanned = sr_segment_catalog_scan(session_dir, camera_name, &segments, &count);
    bfree(session_dir);
    if (!scanned)
        return false;

    uint64_t first_ns = UINT64_MAX;
    uint64_t last_ns = 0;
    bool active_overlap = false;
    for (size_t i = 0; i < count; i++) {
        if (segments[i].start_ns < first_ns)
            first_ns = segments[i].start_ns;
        if (segments[i].end_ns > last_ns)
            last_ns = segments[i].end_ns;
        if (segments[i].active && segments[i].end_ns >= event_in_ns && segments[i].start_ns <= event_out_ns)
            active_overlap = true;
    }

    if (first_ns != UINT64_MAX && event_out_ns >= first_ns && event_in_ns <= last_ns) {
        info->playable_in_ns = event_in_ns < first_ns ? first_ns : event_in_ns;
        info->playable_out_ns = event_out_ns > last_ns ? last_ns : event_out_ns;
        info->active = active_overlap;
        info->coverage = info->playable_in_ns == event_in_ns && info->playable_out_ns == event_out_ns
                             ? SR_REPLAY_COVERAGE_FULL
                             : SR_REPLAY_COVERAGE_PARTIAL;
    }

    sr_segment_catalog_free(segments, count);
    return true;
}
'''

Path("src/sr-replay-coverage.h").write_text(coverage_h, encoding="utf-8")
Path("src/sr-replay-coverage.c").write_text(coverage_c, encoding="utf-8")

cmake = Path("CMakeLists.txt")
replace_once(
    cmake,
    "    src/sr-replay-channel.c\n    src/sr-replay-take.c\n",
    "    src/sr-replay-channel.c\n    src/sr-replay-coverage.c\n    src/sr-replay-take.c\n",
)

locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    'EventDock.Camera="Camera"\nEventDock.CueA="Cue A"\n',
    'EventDock.Camera="Camera"\n'
    'EventDock.Angles="Angles"\n'
    'EventDock.AnglesLegend="● full   ◐ partial   ○ none"\n'
    'EventDock.AngleFull="%1 — full Event coverage"\n'
    'EventDock.AnglePartial="%1 — partial Event coverage (%2 / %3 s)"\n'
    'EventDock.AngleNone="%1 — no recorded media for this Event"\n'
    'EventDock.AngleUnavailable="%1 — recorded media exists, but not at the current replay playhead"\n'
    'EventDock.AngleSwitchFailed="Could not switch/cue this Event to the selected camera at the current playhead"\n'
    'EventDock.AngleSelected="Bus %1: camera %2"\n'
    'EventDock.CueA="Cue A"\n',
)

dock = Path("src/sr-event-dock.cpp")
replace_once(
    dock,
    '#include "sr-replay-channel.h"\n#include "sr-replay-take.h"\n',
    '#include "sr-replay-channel.h"\n#include "sr-replay-coverage.h"\n#include "sr-replay-take.h"\n',
)
replace_once(
    dock,
    '#include <QComboBox>\n#include <QHeaderView>\n',
    '#include <QComboBox>\n#include <QGridLayout>\n#include <QHeaderView>\n',
)
replace_once(
    dock,
    '#include <QVBoxLayout>\n#include <QWidget>\n',
    '#include <QVBoxLayout>\n#include <QVector>\n#include <QWidget>\n',
)
replace_once(
    dock,
    '\t\troot->addLayout(cueBar);\n\n\t\tauto *timelineBar = new QHBoxLayout();\n',
    '''\t\troot->addLayout(cueBar);\n\n\t\tauto *angleHeader = new QHBoxLayout();\n\t\tangleHeader->addWidget(new QLabel(T("EventDock.Angles"), this));\n\t\tangleHeader->addStretch(1);\n\t\tauto *angleLegend = new QLabel(T("EventDock.AnglesLegend"), this);\n\t\tangleLegend->setStyleSheet(QStringLiteral("color: gray;"));\n\t\tangleHeader->addWidget(angleLegend);\n\t\troot->addLayout(angleHeader);\n\t\tangleGrid = new QGridLayout();\n\t\tangleGrid->setHorizontalSpacing(4);\n\t\tangleGrid->setVerticalSpacing(3);\n\t\troot->addLayout(angleGrid);\n\n\t\tauto *timelineBar = new QHBoxLayout();\n''',
)
replace_once(
    dock,
    '\t\tconnect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });\n\n\t\trefreshTimer = new QTimer(this);\n',
    '''\t\tconnect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });\n\t\tconnect(table, &QTableWidget::itemSelectionChanged, this, [this]() { refreshAngleCoverage(); });\n\n\t\trefreshTimer = new QTimer(this);\n''',
)
replace_once(
    dock,
    '''\t\tconnect(transportTimer, &QTimer::timeout, this, [this]() {\n\t\t\trefreshTransportStatus();\n\t\t\tsyncTimeline();\n\t\t});\n''',
    '''\t\tconnect(transportTimer, &QTimer::timeout, this, [this]() {\n\t\t\trefreshTransportStatus();\n\t\t\tsyncTimeline();\n\t\t\tsyncAngleButtonState();\n\t\t});\n''',
)

old_refresh_cameras = r'''	void refreshCameras()
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
		if (current == names)
			return;

		cameraCombo->clear();
		if (names.isEmpty()) {
			cameraCombo->addItem(T("EventDock.NoCamera"), QString());
			return;
		}
		for (const QString &name : names)
			cameraCombo->addItem(name, name);
		const int previousIndex = cameraCombo->findData(previous);
		if (previousIndex >= 0)
			cameraCombo->setCurrentIndex(previousIndex);
	}
'''

new_refresh_cameras = r'''	void rebuildAngleButtons(const QStringList &names)
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
			auto *button = new QPushButton(camera, this);
			button->setCheckable(true);
			button->setMinimumWidth(92);
			button->setProperty("cameraName", camera);
			button->setProperty("coverage", (int)SR_REPLAY_COVERAGE_NONE);
			button->setProperty("playableInNs", QVariant::fromValue<qulonglong>(0));
			button->setProperty("playableOutNs", QVariant::fromValue<qulonglong>(0));
			connect(button, &QPushButton::clicked, this, [this, camera]() { selectAngle(camera); });
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
		const bool haveEvent = controller && eventId && sr_event_controller_get_event(controller, eventId, &event);

		for (QPushButton *button : angleButtons) {
			const QString camera = button->property("cameraName").toString();
			sr_replay_coverage_info coverage = {};
			if (haveEvent) {
				const QByteArray cameraUtf8 = camera.toUtf8();
				if (!sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns, &coverage))
					coverage.coverage = SR_REPLAY_COVERAGE_NONE;
			}

			button->setProperty("coverage", (int)coverage.coverage);
			button->setProperty("playableInNs", QVariant::fromValue<qulonglong>(coverage.playable_in_ns));
			button->setProperty("playableOutNs", QVariant::fromValue<qulonglong>(coverage.playable_out_ns));

			QString marker = QStringLiteral("○");
			QString tooltip = haveEvent ? T("EventDock.AngleNone").arg(camera) : T("EventDock.NoEventSelected");
			if (coverage.coverage == SR_REPLAY_COVERAGE_FULL) {
				marker = QStringLiteral("●");
				tooltip = T("EventDock.AngleFull").arg(camera);
			} else if (coverage.coverage == SR_REPLAY_COVERAGE_PARTIAL) {
				marker = QStringLiteral("◐");
				const double eventSeconds = event.out_ns >= event.in_ns ? (double)(event.out_ns - event.in_ns) / 1e9 : 0.0;
				const double playableSeconds = coverage.playable_out_ns >= coverage.playable_in_ns
							       ? (double)(coverage.playable_out_ns - coverage.playable_in_ns) / 1e9
							       : 0.0;
				tooltip = T("EventDock.AnglePartial")
						  .arg(camera)
						  .arg(playableSeconds, 0, 'f', 2)
						  .arg(eventSeconds, 0, 'f', 2);
			}
			button->setText(QStringLiteral("%1 %2").arg(marker, camera));
			button->setToolTip(tooltip);
		}

		if (haveEvent)
			sr_event_controller_free_event(&event);
		syncAngleButtonState();
	}

	void syncAngleButtonState()
	{
		const uint64_t eventId = angleEventId();
		sr_replay_channel_state state = {};
		const bool haveState = sr_replay_channel_get_state(transportBus(), &state);
		const bool sameEvent = haveState && state.cued && eventId && state.event_id == eventId;
		const QString activeCamera = sameEvent ? QString::fromUtf8(state.camera_name) : QString();

		for (QPushButton *button : angleButtons) {
			const auto coverage = static_cast<sr_replay_coverage>(button->property("coverage").toInt());
			const uint64_t playableIn = button->property("playableInNs").toULongLong();
			const uint64_t playableOut = button->property("playableOutNs").toULongLong();
			const QString camera = button->property("cameraName").toString();
			const bool atPlayhead = !sameEvent || (state.playhead_ns >= playableIn && state.playhead_ns <= playableOut);
			button->setEnabled(eventId && coverage != SR_REPLAY_COVERAGE_NONE && atPlayhead);
			button->setChecked(sameEvent && activeCamera == camera);
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
'''
replace_once(dock, old_refresh_cameras, new_refresh_cameras)

replace_once(
    dock,
    '\tQComboBox *audioCombo = nullptr;\n\tQSlider *timelineSlider = nullptr;\n',
    '\tQComboBox *audioCombo = nullptr;\n\tQGridLayout *angleGrid = nullptr;\n\tQVector<QPushButton *> angleButtons;\n\tQSlider *timelineSlider = nullptr;\n',
)

print("angle coverage UI integration applied")
