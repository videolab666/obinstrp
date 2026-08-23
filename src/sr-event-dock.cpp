/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-dock.h"

#include "sr-capture.h"
#include "sr-event-controller.h"
#include "sr-replay-channel.h"
#include "sr-replay-take.h"
#include "sr-storage-cleanup.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <cstring>

#include <QAbstractItemView>
#include <QComboBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#define NS_PER_SECOND 1000000000ULL

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString stateText(const sr_event_record &event)
{
	QStringList states;
	if (event.pending)
		states.append(T("EventDock.State.Pending"));
	if (event.protected_event)
		states.append(T("EventDock.State.Protected"));
	if (event.played)
		states.append(T("EventDock.State.Played"));
	if (states.isEmpty())
		states.append(T("EventDock.State.Ready"));
	return states.join(QStringLiteral(" / "));
}

QString durationText(const sr_event_record &event)
{
	if (event.out_ns < event.in_ns)
		return QStringLiteral("-");
	return QString::number((double)(event.out_ns - event.in_ns) / 1e9, 'f', 3) + QStringLiteral(" s");
}

struct camera_enum_ctx {
	QStringList *names;
};

void enum_camera_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	auto *ctx = static_cast<camera_enum_ctx *>(param);
	if (!ctx || !ctx->names || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;

	const QString name = QString::fromUtf8(obs_source_get_name(parent));
	if (!name.isEmpty() && !ctx->names->contains(name))
		ctx->names->append(name);
}

bool enum_camera_source(void *param, obs_source_t *source)
{
	obs_source_enum_filters(source, enum_camera_filter, param);
	return true;
}

QStringList captureCameraNames()
{
	QStringList names;
	camera_enum_ctx ctx;
	ctx.names = &names;
	obs_enum_sources(enum_camera_source, &ctx);
	names.sort(Qt::CaseInsensitive);
	return names;
}

QString channelSummary(enum sr_replay_bus bus, const QString &label)
{
	sr_replay_channel_state state = {};
	if (!sr_replay_channel_get_state(bus, &state) || !state.cued)
		return QStringLiteral("%1: —").arg(label);

	const double duration = state.out_ns >= state.in_ns ? (double)(state.out_ns - state.in_ns) / 1e9 : 0.0;
	const double position = state.playhead_ns >= state.in_ns ? (double)(state.playhead_ns - state.in_ns) / 1e9
								 : 0.0;
	QString mode = T("EventDock.Transport.Cued");
	if (state.playing)
		mode = state.paused ? T("EventDock.Transport.Paused") : T("EventDock.Transport.Playing");
	QString flags;
	if (state.backward)
		flags += QStringLiteral(" REV");
	if (state.loop)
		flags += QStringLiteral(" LOOP");
	if (state.partial_coverage)
		flags += QStringLiteral(" PARTIAL");
	flags += state.audio_mode == SR_REPLAY_AUDIO_MASTER ? QStringLiteral(" AUDIO") : QStringLiteral(" MUTE");

	return QStringLiteral("%1: #%2  %3  %4%  %5/%6 s  %7%8")
		.arg(label)
		.arg(state.event_id)
		.arg(QString::fromUtf8(state.camera_name))
		.arg(state.speed_percent, 0, 'f', 0)
		.arg(position, 0, 'f', 2)
		.arg(duration, 0, 'f', 2)
		.arg(mode)
		.arg(flags);
}

class SrEventDock : public QWidget {
public:
	explicit SrEventDock(sr_event_controller *eventController, QWidget *parent = nullptr)
		: QWidget(parent),
		  controller(eventController)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(4, 4, 4, 4);
		root->setSpacing(4);

		auto *markBar = new QHBoxLayout();
		markBar->addWidget(new QLabel(T("EventDock.List"), this));
		listCombo = new QComboBox(this);
		for (unsigned i = 1; i <= SR_EVENT_LIST_COUNT; i++)
			listCombo->addItem(QString::number(i), i);
		listCombo->setCurrentIndex(0);
		markBar->addWidget(listCombo);

		auto *markIn = new QPushButton(QStringLiteral("IN"), this);
		auto *markOut = new QPushButton(QStringLiteral("OUT"), this);
		auto *mark5 = new QPushButton(QStringLiteral("-5"), this);
		auto *mark10 = new QPushButton(QStringLiteral("-10"), this);
		auto *mark20 = new QPushButton(QStringLiteral("-20"), this);
		markIn->setToolTip(T("Hotkey.EventIn"));
		markOut->setToolTip(T("Hotkey.EventOut"));
		mark5->setToolTip(T("Hotkey.EventLast5"));
		mark10->setToolTip(T("Hotkey.EventLast10"));
		mark20->setToolTip(T("Hotkey.EventLast20"));
		markBar->addWidget(markIn);
		markBar->addWidget(markOut);
		markBar->addWidget(mark5);
		markBar->addWidget(mark10);
		markBar->addWidget(mark20);
		markBar->addStretch(1);
		root->addLayout(markBar);

		table = new QTableWidget(this);
		table->setColumnCount(6);
		table->setHorizontalHeaderLabels({T("EventDock.Column.Id"), T("EventDock.Column.Duration"),
						  T("EventDock.Column.Speed"), T("EventDock.Column.State"),
						  T("EventDock.Column.Name"), T("EventDock.Column.Tag")});
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->verticalHeader()->setVisible(false);
		table->horizontalHeader()->setStretchLastSection(true);
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
		root->addWidget(table, 1);

		auto *actionBar = new QHBoxLayout();
		auto *up = new QPushButton(QStringLiteral("↑"), this);
		auto *down = new QPushButton(QStringLiteral("↓"), this);
		auto *played = new QPushButton(T("EventDock.Played"), this);
		auto *protect = new QPushButton(T("EventDock.Protect"), this);
		auto *remove = new QPushButton(T("EventDock.Delete"), this);
		auto *removeMedia = new QPushButton(T("EventDock.DeleteMedia"), this);
		actionBar->addWidget(up);
		actionBar->addWidget(down);
		actionBar->addWidget(played);
		actionBar->addWidget(protect);
		actionBar->addWidget(remove);
		actionBar->addWidget(removeMedia);
		actionBar->addStretch(1);
		actionBar->addWidget(new QLabel(T("EventDock.TargetList"), this));
		targetCombo = new QComboBox(this);
		for (unsigned i = 1; i <= SR_EVENT_LIST_COUNT; i++)
			targetCombo->addItem(QString::number(i), i);
		targetCombo->setCurrentIndex(1);
		actionBar->addWidget(targetCombo);
		auto *copy = new QPushButton(T("EventDock.Copy"), this);
		auto *move = new QPushButton(T("EventDock.Move"), this);
		auto *duplicate = new QPushButton(T("EventDock.Duplicate"), this);
		actionBar->addWidget(copy);
		actionBar->addWidget(move);
		actionBar->addWidget(duplicate);
		root->addLayout(actionBar);

		auto *cueBar = new QHBoxLayout();
		cueBar->addWidget(new QLabel(T("EventDock.Camera"), this));
		cameraCombo = new QComboBox(this);
		cameraCombo->setMinimumContentsLength(18);
		cueBar->addWidget(cameraCombo, 1);
		auto *cueA = new QPushButton(T("EventDock.CueA"), this);
		auto *cueB = new QPushButton(T("EventDock.CueB"), this);
		cueBar->addWidget(cueA);
		cueBar->addWidget(cueB);
		cueBar->addSpacing(12);
		cueBar->addWidget(new QLabel(T("EventDock.TransportBus"), this));
		busCombo = new QComboBox(this);
		busCombo->addItem(QStringLiteral("A"), SR_REPLAY_BUS_A);
		busCombo->addItem(QStringLiteral("B"), SR_REPLAY_BUS_B);
		cueBar->addWidget(busCombo);
		auto *playPause = new QPushButton(T("EventDock.PlayPause"), this);
		auto *stop = new QPushButton(T("EventDock.Stop"), this);
		auto *restart = new QPushButton(T("EventDock.Restart"), this);
		reverseButton = new QPushButton(T("EventDock.Reverse"), this);
		reverseButton->setCheckable(true);
		loopButton = new QPushButton(T("EventDock.Loop"), this);
		loopButton->setCheckable(true);
		cueBar->addWidget(playPause);
		cueBar->addWidget(stop);
		cueBar->addWidget(restart);
		cueBar->addWidget(reverseButton);
		cueBar->addWidget(loopButton);
		speedCombo = new QComboBox(this);
		const int speeds[] = {25, 33, 50, 75, 100};
		for (int speed : speeds)
			speedCombo->addItem(QStringLiteral("%1%").arg(speed), speed);
		speedCombo->setCurrentIndex(speedCombo->findData(100));
		cueBar->addWidget(speedCombo);
		cueBar->addWidget(new QLabel(T("EventDock.Audio"), this));
		audioCombo = new QComboBox(this);
		audioCombo->addItem(T("EventDock.AudioMaster"), SR_REPLAY_AUDIO_MASTER);
		audioCombo->addItem(T("EventDock.AudioOff"), SR_REPLAY_AUDIO_OFF);
		cueBar->addWidget(audioCombo);
		root->addLayout(cueBar);

		auto *takeBar = new QHBoxLayout();
		takeBar->addStretch(1);
		auto *takeA = new QPushButton(T("EventDock.TakeA"), this);
		auto *takeB = new QPushButton(T("EventDock.TakeB"), this);
		auto *takeToggle = new QPushButton(T("EventDock.TakeToggle"), this);
		takeBar->addWidget(takeA);
		takeBar->addWidget(takeB);
		takeBar->addWidget(takeToggle);
		root->addLayout(takeBar);

		transportStatus = new QLabel(this);
		transportStatus->setStyleSheet(QStringLiteral("color: gray;"));
		transportStatus->setWordWrap(true);
		root->addWidget(transportStatus);

		status = new QLabel(T("EventDock.Ready"), this);
		status->setStyleSheet(QStringLiteral("color: gray;"));
		root->addWidget(status);

		connect(listCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (!controller || index < 0)
				return;
			sr_event_controller_set_current_list(controller, currentList());
			refresh();
		});
		connect(markIn, &QPushButton::clicked, this, [this]() { setMarkIn(); });
		connect(markOut, &QPushButton::clicked, this, [this]() { setMarkOut(); });
		connect(mark5, &QPushButton::clicked, this, [this]() { quickMark(5); });
		connect(mark10, &QPushButton::clicked, this, [this]() { quickMark(10); });
		connect(mark20, &QPushButton::clicked, this, [this]() { quickMark(20); });
		connect(up, &QPushButton::clicked, this, [this]() { moveRow(-1); });
		connect(down, &QPushButton::clicked, this, [this]() { moveRow(1); });
		connect(played, &QPushButton::clicked, this, [this]() { togglePlayed(); });
		connect(protect, &QPushButton::clicked, this, [this]() { toggleProtected(); });
		connect(remove, &QPushButton::clicked, this, [this]() { deleteSelected(false); });
		connect(removeMedia, &QPushButton::clicked, this, [this]() { deleteSelected(true); });
		connect(copy, &QPushButton::clicked, this, [this]() { copySelected(); });
		connect(move, &QPushButton::clicked, this, [this]() { moveSelected(); });
		connect(duplicate, &QPushButton::clicked, this, [this]() { duplicateSelected(); });
		connect(cueA, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_A); });
		connect(cueB, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_B); });
		connect(busCombo, &QComboBox::currentIndexChanged, this, [this](int) { syncTransportControls(); });
		connect(playPause, &QPushButton::clicked, this, [this]() { togglePlayPause(); });
		connect(stop, &QPushButton::clicked, this, [this]() { stopTransport(); });
		connect(restart, &QPushButton::clicked, this, [this]() { restartTransport(); });
		connect(reverseButton, &QPushButton::clicked, this,
			[this](bool checked) { sr_replay_channel_set_backward(transportBus(), checked); });
		connect(loopButton, &QPushButton::clicked, this,
			[this](bool checked) { sr_replay_channel_set_loop(transportBus(), checked); });
		connect(speedCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (index >= 0)
				sr_replay_channel_set_speed(transportBus(), speedCombo->itemData(index).toDouble());
		});
		connect(audioCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
			if (index >= 0)
				sr_replay_channel_set_audio_mode(
					transportBus(),
					static_cast<sr_replay_audio_mode>(audioCombo->itemData(index).toInt()));
			refreshTransportStatus();
		});
		connect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });
		connect(takeB, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_B); });
		connect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });

		refreshTimer = new QTimer(this);
		refreshTimer->setInterval(750);
		connect(refreshTimer, &QTimer::timeout, this, [this]() {
			refresh();
			refreshTransportStatus();
			if (++cameraRefreshTicks >= 4) {
				cameraRefreshTicks = 0;
				refreshCameras();
			}
		});
		refreshTimer->start();

		if (controller)
			sr_event_controller_set_current_list(controller, currentList());
		refreshCameras();
		refresh();
		refreshTransportStatus();
		syncTransportControls();
	}

private:
	unsigned currentList() const { return listCombo ? listCombo->currentData().toUInt() : 1; }

	unsigned targetList() const { return targetCombo ? targetCombo->currentData().toUInt() : 1; }

	enum sr_replay_bus transportBus() const
	{
		return busCombo && busCombo->currentData().toInt() == SR_REPLAY_BUS_B ? SR_REPLAY_BUS_B
										      : SR_REPLAY_BUS_A;
	}

	QString selectedCamera() const { return cameraCombo ? cameraCombo->currentData().toString() : QString(); }

	uint64_t selectedEventId() const
	{
		const int row = table ? table->currentRow() : -1;
		if (row < 0)
			return 0;
		QTableWidgetItem *item = table->item(row, 0);
		return item ? item->data(Qt::UserRole).toULongLong() : 0;
	}

	void setStatus(const char *key) { status->setText(T(key)); }

	void setCreatedStatus(uint64_t eventId) { status->setText(T("EventDock.Created").arg(eventId)); }

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

	void refreshTransportStatus()
	{
		if (!transportStatus)
			return;
		transportStatus->setText(channelSummary(SR_REPLAY_BUS_A, QStringLiteral("A")) + QStringLiteral("    ") +
					 channelSummary(SR_REPLAY_BUS_B, QStringLiteral("B")));
	}

	void syncTransportControls()
	{
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state))
			return;
		reverseButton->setChecked(state.backward);
		loopButton->setChecked(state.loop);
		const int speedIndex = speedCombo->findData((int)state.speed_percent);
		if (speedIndex >= 0)
			speedCombo->setCurrentIndex(speedIndex);
		const int audioIndex = audioCombo->findData((int)state.audio_mode);
		if (audioIndex >= 0)
			audioCombo->setCurrentIndex(audioIndex);
		refreshTransportStatus();
	}

	void cueSelected(enum sr_replay_bus bus)
	{
		const uint64_t eventId = selectedEventId();
		const QString camera = selectedCamera();
		if (!eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}
		if (camera.isEmpty()) {
			setStatus("EventDock.NoCameraSelected");
			return;
		}
		const QByteArray cameraUtf8 = camera.toUtf8();
		if (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData())) {
			setStatus("EventDock.CueFailed");
			return;
		}
		status->setText(T("EventDock.Cued")
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
					.arg(eventId));
		if (transportBus() == bus)
			syncTransportControls();
		refreshTransportStatus();
	}

	void takeBus(enum sr_replay_bus bus)
	{
		if (!controller || !sr_replay_take_bus(controller, bus)) {
			setStatus("EventDock.TakeFailed");
			return;
		}
		status->setText(
			T("EventDock.Taken").arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B")));
		refresh();
		refreshTransportStatus();
	}

	void takeToggleBus()
	{
		if (!controller || !sr_replay_take_toggle(controller)) {
			setStatus("EventDock.TakeFailed");
			return;
		}
		setStatus("EventDock.ToggleTaken");
		refresh();
		refreshTransportStatus();
	}

	void togglePlayPause()
	{
		const enum sr_replay_bus bus = transportBus();
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(bus, &state) || !state.cued) {
			setStatus("EventDock.NoCue");
			return;
		}
		const bool ok = state.playing && !state.paused ? sr_replay_channel_pause(bus, true)
				: state.paused                 ? sr_replay_channel_pause(bus, false)
							       : sr_replay_channel_play(bus);
		if (!ok)
			setStatus("EventDock.TransportFailed");
		refreshTransportStatus();
	}

	void stopTransport()
	{
		sr_replay_channel_stop(transportBus());
		refreshTransportStatus();
	}

	void restartTransport()
	{
		sr_replay_channel_restart(transportBus());
		refreshTransportStatus();
	}

	void setMarkIn()
	{
		if (!controller || !sr_event_controller_mark_in(controller, obs_get_video_frame_time())) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.InSet");
	}

	void setMarkOut()
	{
		uint64_t eventId = 0;
		if (!controller || !sr_event_controller_mark_out(controller, obs_get_video_frame_time(), &eventId)) {
			setStatus("EventDock.NeedIn");
			return;
		}
		setCreatedStatus(eventId);
		refresh(eventId);
	}

	void quickMark(unsigned seconds)
	{
		uint64_t eventId = 0;
		if (!controller || !sr_event_controller_quick_mark(controller, obs_get_video_frame_time(),
								   seconds * NS_PER_SECOND, 0, &eventId)) {
			setStatus("EventDock.Failed");
			return;
		}
		setCreatedStatus(eventId);
		refresh(eventId);
	}

	void refresh(uint64_t selectEventId = 0)
	{
		if (!controller || !table)
			return;
		if (!selectEventId)
			selectEventId = selectedEventId();

		uint64_t *eventIds = nullptr;
		size_t count = 0;
		if (!sr_event_controller_get_list_events(controller, currentList(), &eventIds, &count)) {
			setStatus("EventDock.Failed");
			return;
		}

		table->setUpdatesEnabled(false);
		table->setRowCount((int)count);
		int selectedRow = -1;
		for (size_t i = 0; i < count; i++) {
			sr_event_record event = {};
			if (!sr_event_controller_get_event(controller, eventIds[i], &event))
				continue;

			auto *id = new QTableWidgetItem(QString::number(event.id));
			id->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(event.id));
			table->setItem((int)i, 0, id);
			table->setItem((int)i, 1, new QTableWidgetItem(durationText(event)));
			table->setItem((int)i, 2,
				       new QTableWidgetItem(QString::number(event.speed_percent, 'f', 0) +
							    QStringLiteral("%")));
			table->setItem((int)i, 3, new QTableWidgetItem(stateText(event)));
			table->setItem((int)i, 4,
				       new QTableWidgetItem(QString::fromUtf8(event.name ? event.name : "")));
			table->setItem((int)i, 5, new QTableWidgetItem(QString::fromUtf8(event.tag ? event.tag : "")));
			if (event.id == selectEventId)
				selectedRow = (int)i;
			sr_event_controller_free_event(&event);
		}
		bfree(eventIds);
		table->setUpdatesEnabled(true);
		if (selectedRow >= 0)
			table->selectRow(selectedRow);
	}

	void moveRow(int delta)
	{
		const int row = table->currentRow();
		const uint64_t eventId = selectedEventId();
		if (!eventId || row < 0)
			return;
		const int target = row + delta;
		if (target < 0 || target >= table->rowCount())
			return;
		if (!sr_event_controller_reorder(controller, eventId, currentList(), target)) {
			setStatus("EventDock.Failed");
			return;
		}
		refresh(eventId);
	}

	void copySelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		if (!sr_event_controller_copy_to_list(controller, eventId, targetList(), -1)) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.Copied");
	}

	void moveSelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		const unsigned source = currentList();
		const unsigned target = targetList();
		if (!sr_event_controller_move_to_list(controller, eventId, source, target, -1)) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.Moved");
		refresh();
	}

	void duplicateSelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		uint64_t newEventId = 0;
		if (!sr_event_controller_duplicate(controller, eventId, targetList(), -1, &newEventId)) {
			setStatus("EventDock.Failed");
			return;
		}
		setCreatedStatus(newEventId);
		if (targetList() == currentList())
			refresh(newEventId);
	}

	void togglePlayed()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			return;
		}
		const bool value = !event.played;
		sr_event_controller_free_event(&event);
		if (!sr_event_controller_set_played(controller, eventId, value)) {
			setStatus("EventDock.Failed");
			return;
		}
		refresh(eventId);
	}

	void toggleProtected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		sr_event_record event = {};
		if (!sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			return;
		}
		const bool value = !event.protected_event;
		sr_event_controller_free_event(&event);
		if (!sr_event_controller_set_protected(controller, eventId, value)) {
			setStatus("EventDock.Failed");
			return;
		}
		refresh(eventId);
	}

	void deleteSelected(bool deleteMedia)
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;

		if (deleteMedia) {
			sr_event_record event = {};
			if (!sr_event_controller_get_event(controller, eventId, &event)) {
				setStatus("EventDock.Failed");
				return;
			}
			const bool protectedEvent = event.protected_event;
			sr_event_controller_free_event(&event);
			if (protectedEvent) {
				setStatus("EventDock.ProtectedMedia");
				return;
			}
		}

		const char *confirmKey = deleteMedia ? "EventDock.DeleteMediaConfirm" : "EventDock.DeleteConfirm";
		if (QMessageBox::question(this, T("EventDock.DeleteTitle"), T(confirmKey)) != QMessageBox::Yes)
			return;

		if (!deleteMedia) {
			if (!sr_event_controller_delete_event(controller, eventId)) {
				setStatus("EventDock.Failed");
				return;
			}
			setStatus("EventDock.Deleted");
			refresh();
			return;
		}

		for (int i = SR_REPLAY_BUS_A; i < SR_REPLAY_BUS_COUNT; i++) {
			sr_replay_channel_state state = {};
			const auto bus = static_cast<sr_replay_bus>(i);
			if (sr_replay_channel_get_state(bus, &state) && state.cued && state.event_id == eventId)
				sr_replay_channel_clear(bus);
		}

		sr_storage_cleanup_result cleanup = {};
		if (!sr_event_controller_delete_event_with_media(controller, eventId, &cleanup)) {
			setStatus("EventDock.MediaCleanupFailed");
			return;
		}

		status->setText(T("EventDock.MediaDeleted")
					.arg(cleanup.segments_deleted)
					.arg(cleanup.segments_pinned)
					.arg(cleanup.errors));
		refresh();
	}

	sr_event_controller *controller = nullptr;
	QComboBox *listCombo = nullptr;
	QComboBox *targetCombo = nullptr;
	QComboBox *cameraCombo = nullptr;
	QComboBox *busCombo = nullptr;
	QComboBox *speedCombo = nullptr;
	QComboBox *audioCombo = nullptr;
	QPushButton *reverseButton = nullptr;
	QPushButton *loopButton = nullptr;
	QTableWidget *table = nullptr;
	QLabel *transportStatus = nullptr;
	QLabel *status = nullptr;
	QTimer *refreshTimer = nullptr;
	unsigned cameraRefreshTicks = 0;
};

} // namespace

void sr_event_dock_register(struct sr_event_controller *controller)
{
	auto *dock = new SrEventDock(controller);
	dock->setObjectName(QStringLiteral("SportsReplayEventDock"));
	if (!obs_frontend_add_dock_by_id("sports_replay_event_dock", obs_module_text("EventDock.Title"), dock))
		delete dock;
}
