/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-dock.h"

#include "sr-camera-list.h"
#include "sr-capture.h"
#include "sr-event-controller.h"
#include "sr-event-export.h"
#include "sr-replay-channel.h"
#include "sr-replay-coverage.h"
#include "sr-replay-playlist.h"
#include "sr-replay-take.h"
#include "sr-storage-cleanup.h"
#include "sr-session.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <cstring>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QAbstractItemDelegate>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
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

QString replayClockText(uint64_t ns)
{
	const uint64_t totalMs = ns / 1000000ULL;
	const uint64_t minutes = totalMs / 60000ULL;
	const uint64_t seconds = (totalMs / 1000ULL) % 60ULL;
	const uint64_t millis = totalMs % 1000ULL;
	return QStringLiteral("%1:%2.%3")
		.arg(minutes, 2, 10, QChar('0'))
		.arg(seconds, 2, 10, QChar('0'))
		.arg(millis, 3, 10, QChar('0'));
}

QStringList captureCameraNames()
{
	QStringList names;
	sr_camera_list cameras = {};
	if (!sr_camera_list_capture(&cameras))
		return names;
	for (size_t i = 0; i < cameras.count; i++)
		names.append(QString::fromUtf8(cameras.names[i]));
	sr_camera_list_free(&cameras);
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
	flags += state.audio_mode == SR_REPLAY_AUDIO_MASTER   ? QStringLiteral(" MASTER")
		 : state.audio_mode == SR_REPLAY_AUDIO_CAMERA ? QStringLiteral(" CAMERA")
							      : QStringLiteral(" MUTE");

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

QString safeFilePart(const QString &value)
{
	QString safe;
	safe.reserve(value.size());
	for (const QChar character : value) {
		if (character.isLetterOrNumber() || character == QChar('-') || character == QChar('_'))
			safe.append(character);
		else if (safe.isEmpty() || safe.back() != QChar('_'))
			safe.append(QChar('_'));
	}
	while (safe.endsWith(QChar('_')))
		safe.chop(1);
	return safe.isEmpty() ? QStringLiteral("camera") : safe.left(80);
}

QString unusedAnglePath(const QDir &directory, uint64_t eventId, const QString &camera)
{
	const QString stem = QStringLiteral("Event_%1_%2").arg(eventId, 6, 10, QChar('0')).arg(safeFilePart(camera));
	QString path = directory.filePath(stem + QStringLiteral(".mp4"));
	for (unsigned suffix = 2; QFileInfo::exists(path); suffix++)
		path = directory.filePath(stem + QStringLiteral("-%1.mp4").arg(suffix));
	return path;
}

struct ExportTask {
	std::string sessionDir;
	std::string camera;
	std::string outputPath;
	uint64_t eventInNs = 0;
	uint64_t eventOutNs = 0;
	int64_t syncOffsetNs = 0;
	bool includeMasterAudio = false;
};

struct ExportJob {
	std::vector<ExportTask> tasks;
	std::atomic<bool> cancel{false};
	std::atomic<bool> done{false};
	std::atomic<unsigned> progress{0};
	std::thread worker;
	bool success = false;
	size_t completed = 0;
	std::string failedCamera;
	sr_event_export_result result = {};
};

constexpr int ANGLE_PREVIEW_WIDTH = 128;
constexpr int ANGLE_PREVIEW_HEIGHT = 72;

struct AnglePreviewResult {
	std::string camera;
	std::vector<uint8_t> rgba;
};

struct AnglePreviewTask {
	std::string camera;
	uint64_t timestampNs = 0;
};

struct AnglePreviewJob {
	uint64_t eventId = 0;
	std::string sessionDir;
	std::vector<AnglePreviewTask> tasks;
	std::vector<AnglePreviewResult> results;
	std::atomic<bool> done{false};
	std::thread worker;
};

void runAnglePreviewJob(AnglePreviewJob *job)
{
	for (const AnglePreviewTask &task : job->tasks) {
		uint8_t *rgba = nullptr;
		AnglePreviewResult result;
		result.camera = task.camera;
		if (sr_disk_thumbnail_rgba(job->sessionDir.c_str(), task.camera.c_str(), task.timestampNs,
					   ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT, &rgba) &&
		    rgba) {
			const size_t bytes = (size_t)ANGLE_PREVIEW_WIDTH * ANGLE_PREVIEW_HEIGHT * 4;
			result.rgba.assign(rgba, rgba + bytes);
		}
		bfree(rgba);
		job->results.emplace_back(std::move(result));
	}
	job->done.store(true, std::memory_order_release);
}

bool exportCancelled(void *data)
{
	return static_cast<ExportJob *>(data)->cancel.load(std::memory_order_relaxed);
}

void exportProgress(void *data, unsigned percent)
{
	auto *job = static_cast<ExportJob *>(data);
	const unsigned taskCount = (unsigned)job->tasks.size();
	const unsigned completed = (unsigned)job->completed;
	job->progress.store(taskCount ? (completed * 100U + percent) / taskCount : 0, std::memory_order_relaxed);
}

void runExportJob(ExportJob *job)
{
	job->success = true;
	for (size_t i = 0; i < job->tasks.size(); i++) {
		if (job->cancel.load(std::memory_order_relaxed)) {
			job->success = false;
			job->result.error = SR_EVENT_EXPORT_CANCELLED;
			break;
		}

		const ExportTask &task = job->tasks[i];
		sr_event_export_spec spec = {};
		spec.session_dir = task.sessionDir.c_str();
		spec.camera_name = task.camera.c_str();
		spec.output_path = task.outputPath.c_str();
		spec.event_in_ns = task.eventInNs;
		spec.event_out_ns = task.eventOutNs;
		spec.camera_sync_offset_ns = task.syncOffsetNs;
		spec.include_master_audio = task.includeMasterAudio;
		job->failedCamera = task.camera;
		if (!sr_event_export_fast(&spec, exportCancelled, exportProgress, job, &job->result)) {
			job->success = false;
			break;
		}
		job->completed = i + 1;
		job->progress.store((unsigned)(job->completed * 100 / job->tasks.size()), std::memory_order_relaxed);
	}
	job->done.store(true, std::memory_order_release);
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

		auto *operatorHint = new QLabel(T("EventDock.OperatorHint"), this);
		operatorHint->setWordWrap(true);
		operatorHint->setStyleSheet(
			QStringLiteral("QLabel { color: palette(text); background: palette(alternate-base); "
				       "border: 1px solid palette(mid); border-radius: 3px; padding: 5px; }"));
		root->addWidget(operatorHint);

		auto *recordBar = new QHBoxLayout();
		auto *startRecord = new QPushButton(T("EventDock.RecordStart"), this);
		auto *stopRecord = new QPushButton(T("EventDock.RecordStop"), this);
		startRecord->setStyleSheet(QStringLiteral("font-weight: bold;"));
		stopRecord->setStyleSheet(QStringLiteral("font-weight: bold;"));
		recordStatus = new QLabel(this);
		recordStatus->setWordWrap(true);
		recordBar->addWidget(startRecord);
		recordBar->addWidget(stopRecord);
		recordBar->addWidget(recordStatus, 1);
		root->addLayout(recordBar);

		auto *programBar = new QHBoxLayout();
		programStatus = new QLabel(this);
		cueAStatus = new QLabel(this);
		cueBStatus = new QLabel(this);
		for (QLabel *label : {programStatus, cueAStatus, cueBStatus}) {
			label->setAlignment(Qt::AlignCenter);
			label->setMinimumHeight(28);
			programBar->addWidget(label, 1);
		}
		root->addLayout(programBar);

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
		table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
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

		auto *exportBar = new QHBoxLayout();
		exportBar->addWidget(new QLabel(T("EventDock.Export"), this));
		exportModeCombo = new QComboBox(this);
		exportModeCombo->addItem(T("EventDock.ExportPreferred"), 0);
		exportModeCombo->addItem(T("EventDock.ExportAll"), 1);
		exportBar->addWidget(exportModeCombo);
		auto *exportFast = new QPushButton(T("EventDock.ExportFast"), this);
		exportCancelButton = new QPushButton(T("EventDock.ExportCancel"), this);
		exportCancelButton->setEnabled(false);
		exportProgressBar = new QProgressBar(this);
		exportProgressBar->setRange(0, 100);
		exportProgressBar->setValue(0);
		exportProgressBar->setMinimumWidth(120);
		exportBar->addWidget(exportFast);
		exportBar->addWidget(exportCancelButton);
		exportBar->addWidget(exportProgressBar, 1);
		root->addLayout(exportBar);

		auto *cueBar = new QHBoxLayout();
		cueBar->addWidget(new QLabel(T("EventDock.Camera"), this));
		cameraCombo = new QComboBox(this);
		cameraCombo->setMinimumContentsLength(18);
		cueBar->addWidget(cameraCombo, 1);
		auto *setPreferred = new QPushButton(T("EventDock.SetPreferred"), this);
		auto *clearPreferred = new QPushButton(T("EventDock.ClearPreferred"), this);
		auto *cueA = new QPushButton(T("EventDock.CueA"), this);
		auto *cueB = new QPushButton(T("EventDock.CueB"), this);
		cueBar->addWidget(setPreferred);
		cueBar->addWidget(clearPreferred);
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
		auto *prevFrame = new QPushButton(T("EventDock.PrevFrame"), this);
		auto *nextFrame = new QPushButton(T("EventDock.NextFrame"), this);
		prevFrame->setToolTip(T("EventDock.PrevFrame.Tooltip"));
		nextFrame->setToolTip(T("EventDock.NextFrame.Tooltip"));
		reverseButton = new QPushButton(T("EventDock.Reverse"), this);
		reverseButton->setCheckable(true);
		loopButton = new QPushButton(T("EventDock.Loop"), this);
		loopButton->setCheckable(true);
		cueBar->addWidget(playPause);
		cueBar->addWidget(stop);
		cueBar->addWidget(restart);
		cueBar->addWidget(prevFrame);
		cueBar->addWidget(nextFrame);
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
		audioCombo->addItem(T("EventDock.AudioCamera"), SR_REPLAY_AUDIO_CAMERA);
		audioCombo->addItem(T("EventDock.AudioOff"), SR_REPLAY_AUDIO_OFF);
		cueBar->addWidget(audioCombo);
		root->addLayout(cueBar);

		auto *angleHeader = new QHBoxLayout();
		angleHeader->addWidget(new QLabel(T("EventDock.Angles"), this));
		angleHeader->addStretch(1);
		auto *angleLegend = new QLabel(T("EventDock.AnglesLegend"), this);
		angleLegend->setStyleSheet(QStringLiteral("color: gray;"));
		angleHeader->addWidget(angleLegend);
		root->addLayout(angleHeader);
		angleGrid = new QGridLayout();
		angleGrid->setHorizontalSpacing(4);
		angleGrid->setVerticalSpacing(3);
		root->addLayout(angleGrid);

		auto *timelineBar = new QHBoxLayout();
		timelineBar->addWidget(new QLabel(T("EventDock.Timeline"), this));
		timelineSlider = new QSlider(Qt::Horizontal, this);
		timelineSlider->setRange(0, 10000);
		timelineSlider->setSingleStep(1);
		timelineSlider->setPageStep(100);
		timelineSlider->setEnabled(false);
		timelineSlider->setToolTip(T("EventDock.Timeline.Tooltip"));
		timelineBar->addWidget(timelineSlider, 1);
		timelineTime = new QLabel(QStringLiteral("--:--.--- / --:--.---"), this);
		timelineTime->setMinimumWidth(150);
		timelineBar->addWidget(timelineTime);
		root->addLayout(timelineBar);

		auto *jogShuttleBar = new QHBoxLayout();
		jogShuttleBar->addWidget(new QLabel(T("EventDock.Jog"), this));
		jogSlider = new QSlider(Qt::Horizontal, this);
		jogSlider->setRange(-24, 24);
		jogSlider->setValue(0);
		jogSlider->setSingleStep(1);
		jogSlider->setPageStep(1);
		jogSlider->setToolTip(T("EventDock.Jog.Tooltip"));
		jogShuttleBar->addWidget(jogSlider, 1);
		jogShuttleBar->addSpacing(8);
		jogShuttleBar->addWidget(new QLabel(T("EventDock.Shuttle"), this));
		shuttleSlider = new QSlider(Qt::Horizontal, this);
		shuttleSlider->setRange(-5, 5);
		shuttleSlider->setValue(0);
		shuttleSlider->setSingleStep(1);
		shuttleSlider->setPageStep(1);
		shuttleSlider->setToolTip(T("EventDock.Shuttle.Tooltip"));
		jogShuttleBar->addWidget(shuttleSlider, 1);
		shuttleValue = new QLabel(QStringLiteral("0"), this);
		shuttleValue->setMinimumWidth(48);
		jogShuttleBar->addWidget(shuttleValue);
		root->addLayout(jogShuttleBar);

		auto *takeBar = new QHBoxLayout();
		takeBar->addStretch(1);
		auto *takeA = new QPushButton(T("EventDock.TakeA"), this);
		auto *takeB = new QPushButton(T("EventDock.TakeB"), this);
		auto *takeToggle = new QPushButton(T("EventDock.TakeToggle"), this);
		auto *returnLive = new QPushButton(T("EventDock.ReturnLive"), this);
		auto *playlistA = new QPushButton(T("EventDock.PlaylistA"), this);
		auto *playlistB = new QPushButton(T("EventDock.PlaylistB"), this);
		auto *playlistNext = new QPushButton(T("EventDock.PlaylistNext"), this);
		auto *playlistStop = new QPushButton(T("EventDock.PlaylistStop"), this);
		auto *playSelected = new QPushButton(T("EventDock.PlaySelected"), this);
		playSelected->setStyleSheet(QStringLiteral("font-weight: bold;"));
		takeBar->addWidget(playlistA);
		takeBar->addWidget(playlistB);
		takeBar->addWidget(playlistNext);
		takeBar->addWidget(playlistStop);
		takeBar->addSpacing(10);
		takeBar->addWidget(playSelected);
		takeBar->addWidget(takeA);
		takeBar->addWidget(takeB);
		takeBar->addWidget(takeToggle);
		takeBar->addWidget(returnLive);
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
		connect(startRecord, &QPushButton::clicked, this, [this]() { setAllRecording(true); });
		connect(stopRecord, &QPushButton::clicked, this, [this]() { setAllRecording(false); });
		connect(up, &QPushButton::clicked, this, [this]() { moveRow(-1); });
		connect(down, &QPushButton::clicked, this, [this]() { moveRow(1); });
		connect(played, &QPushButton::clicked, this, [this]() { togglePlayed(); });
		connect(protect, &QPushButton::clicked, this, [this]() { toggleProtected(); });
		connect(remove, &QPushButton::clicked, this, [this]() { deleteSelected(false); });
		connect(removeMedia, &QPushButton::clicked, this, [this]() { deleteSelected(true); });
		connect(copy, &QPushButton::clicked, this, [this]() { copySelected(); });
		connect(move, &QPushButton::clicked, this, [this]() { moveSelected(); });
		connect(duplicate, &QPushButton::clicked, this, [this]() { duplicateSelected(); });
		connect(exportFast, &QPushButton::clicked, this, [this]() { startExport(); });
		connect(exportCancelButton, &QPushButton::clicked, this, [this]() { cancelExport(); });
		connect(setPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(false); });
		connect(clearPreferred, &QPushButton::clicked, this, [this]() { setPreferredCamera(true); });
		connect(cueA, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_A); });
		connect(cueB, &QPushButton::clicked, this, [this]() { cueSelected(SR_REPLAY_BUS_B); });
		connect(busCombo, &QComboBox::currentIndexChanged, this, [this](int) { syncTransportControls(); });
		connect(playPause, &QPushButton::clicked, this, [this]() { togglePlayPause(); });
		connect(stop, &QPushButton::clicked, this, [this]() { stopTransport(); });
		connect(restart, &QPushButton::clicked, this, [this]() { restartTransport(); });
		connect(prevFrame, &QPushButton::clicked, this, [this]() { stepFrame(-1); });
		connect(nextFrame, &QPushButton::clicked, this, [this]() { stepFrame(1); });
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
		connect(timelineSlider, &QSlider::sliderPressed, this, [this]() {
			timelineDragging = true;
			sr_replay_channel_pause(transportBus(), true);
		});
		connect(timelineSlider, &QSlider::sliderMoved, this, [this](int value) { seekTimeline(value); });
		connect(timelineSlider, &QSlider::sliderReleased, this, [this]() {
			seekTimeline(timelineSlider->value());
			timelineDragging = false;
			syncTimeline();
		});
		connect(jogSlider, &QSlider::sliderPressed, this, [this]() { jogLastValue = jogSlider->value(); });
		connect(jogSlider, &QSlider::sliderMoved, this, [this](int value) { jogMoved(value); });
		connect(jogSlider, &QSlider::sliderReleased, this, [this]() {
			const QSignalBlocker blocker(jogSlider);
			jogSlider->setValue(0);
			jogLastValue = 0;
		});
		connect(shuttleSlider, &QSlider::valueChanged, this, [this](int value) { applyShuttle(value); });
		connect(playlistA, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_A); });
		connect(playlistB, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_B); });
		connect(playlistNext, &QPushButton::clicked, this, [this]() { nextPlaylist(); });
		connect(playlistStop, &QPushButton::clicked, this, [this]() { stopPlaylist(); });
		connect(playSelected, &QPushButton::clicked, this, [this]() { playSelectedEvent(); });
		connect(takeA, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_A); });
		connect(takeB, &QPushButton::clicked, this, [this]() { takeBus(SR_REPLAY_BUS_B); });
		connect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });
		connect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });
		connect(table, &QTableWidget::itemSelectionChanged, this, [this]() { refreshAngleCoverage(); });
		connect(table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) { editEvent(item); });
		connect(table->itemDelegate(), &QAbstractItemDelegate::closeEditor, this,
			[this]() { tableEditing = false; });
		connect(table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *item) {
			if (item && (item->column() == 2 || item->column() == 4 || item->column() == 5))
				tableEditing = true;
			else if (item)
				playSelectedEvent();
		});

		refreshTimer = new QTimer(this);
		refreshTimer->setInterval(750);
		connect(refreshTimer, &QTimer::timeout, this, [this]() {
			refresh();
			refreshTransportStatus();
			refreshRecordingStatus();
			if (++cameraRefreshTicks >= 4) {
				cameraRefreshTicks = 0;
				refreshCameras();
			}
		});
		refreshTimer->start();

		transportTimer = new QTimer(this);
		transportTimer->setInterval(100);
		connect(transportTimer, &QTimer::timeout, this, [this]() {
			refreshTransportStatus();
			refreshProgramState();
			syncTimeline();
			syncAngleButtonState();
			pollExport();
			pollAnglePreviews();
		});
		transportTimer->start();

		if (controller)
			sr_event_controller_set_current_list(controller, currentList());
		refreshCameras();
		refresh();
		refreshTransportStatus();
		refreshProgramState();
		refreshRecordingStatus();
		syncTransportControls();
	}

	~SrEventDock() override
	{
		if (exportJob) {
			exportJob->cancel.store(true, std::memory_order_relaxed);
			if (exportJob->worker.joinable())
				exportJob->worker.join();
		}
		if (anglePreviewJob && anglePreviewJob->worker.joinable())
			anglePreviewJob->worker.join();
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

	void setAllRecording(bool enabled)
	{
		size_t cameras = 0;
		if (!sr_capture_set_all_disk_recording(enabled, &cameras) || !cameras) {
			setStatus("EventDock.RecordNoCameras");
			refreshRecordingStatus();
			return;
		}
		status->setText(T(enabled ? "EventDock.RecordStartRequested" : "EventDock.RecordStopped").arg(cameras));
		refreshRecordingStatus();
	}

	void refreshRecordingStatus()
	{
		if (!recordStatus)
			return;
		sr_capture_recording_summary summary = {};
		if (!sr_capture_get_recording_summary(&summary) || !summary.camera_count) {
			recordStatus->setText(T("EventDock.RecordNoCameras"));
			recordStatus->setStyleSheet(QStringLiteral("color: #d8a000;"));
			return;
		}

		if (summary.reserve_blocked_count) {
			recordStatus->setText(T("EventDock.RecordReserve")
						      .arg(summary.reserve_blocked_count)
						      .arg(summary.camera_count));
			recordStatus->setStyleSheet(QStringLiteral("color: #ff5b5b; font-weight: bold;"));
		} else if (summary.failed_count) {
			recordStatus->setText(
				T("EventDock.RecordError").arg(summary.failed_count).arg(summary.camera_count));
			recordStatus->setStyleSheet(QStringLiteral("color: #ff5b5b; font-weight: bold;"));
		} else if (summary.active_count && summary.active_count == summary.requested_count) {
			recordStatus->setText(
				T("EventDock.RecordActive")
					.arg(summary.active_count)
					.arg(summary.camera_count)
					.arg(summary.packets_written)
					.arg((double)summary.bytes_written / (1024.0 * 1024.0), 0, 'f', 1));
			recordStatus->setStyleSheet(QStringLiteral("color: #30c85a; font-weight: bold;"));
		} else if (summary.requested_count) {
			recordStatus->setText(
				T("EventDock.RecordStarting").arg(summary.active_count).arg(summary.requested_count));
			recordStatus->setStyleSheet(QStringLiteral("color: #d8a000;"));
		} else {
			recordStatus->setText(T("EventDock.RecordIdle").arg(summary.camera_count));
			recordStatus->setStyleSheet(QStringLiteral("color: gray;"));
		}
	}

	bool recordingHasMedia()
	{
		sr_capture_recording_summary summary = {};
		return sr_capture_get_recording_summary(&summary) && summary.active_count && summary.packets_written &&
		       summary.reserve_blocked_count < summary.active_count;
	}

	void rebuildAngleButtons(const QStringList &names)
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
			button->setMinimumSize(170, 86);
			button->setIconSize(QSize(ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT));
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
		const bool haveEvent = controller && eventId &&
				       sr_event_controller_get_event(controller, eventId, &event);

		QString preferredCamera;
		if (haveEvent && event.preferred_camera_id) {
			char *preferredName = nullptr;
			if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id,
								&preferredName) &&
			    preferredName)
				preferredCamera = QString::fromUtf8(preferredName);
			bfree(preferredName);
		}

		for (QPushButton *button : angleButtons) {
			const QString camera = button->property("cameraName").toString();
			sr_replay_coverage_info coverage = {};
			if (haveEvent) {
				const QByteArray cameraUtf8 = camera.toUtf8();
				if (!sr_replay_coverage_query(cameraUtf8.constData(), event.in_ns, event.out_ns,
							      &coverage))
					coverage.coverage = SR_REPLAY_COVERAGE_NONE;
			}

			button->setProperty("coverage", (int)coverage.coverage);
			button->setProperty("playableInNs", QVariant::fromValue<qulonglong>(coverage.playable_in_ns));
			button->setProperty("playableOutNs", QVariant::fromValue<qulonglong>(coverage.playable_out_ns));
			button->setProperty("syncOffsetNs", QVariant::fromValue<qlonglong>(coverage.sync_offset_ns));

			QString marker = QStringLiteral("○");
			QString tooltip = haveEvent ? T("EventDock.AngleNone").arg(camera)
						    : T("EventDock.NoEventSelected");
			if (coverage.coverage == SR_REPLAY_COVERAGE_FULL) {
				marker = QStringLiteral("●");
				tooltip = T("EventDock.AngleFull").arg(camera);
			} else if (coverage.coverage == SR_REPLAY_COVERAGE_PARTIAL) {
				marker = QStringLiteral("◐");
				const double eventSeconds =
					event.out_ns >= event.in_ns ? (double)(event.out_ns - event.in_ns) / 1e9 : 0.0;
				const double playableSeconds =
					coverage.playable_out_ns >= coverage.playable_in_ns
						? (double)(coverage.playable_out_ns - coverage.playable_in_ns) / 1e9
						: 0.0;
				tooltip = T("EventDock.AnglePartial")
						  .arg(camera)
						  .arg(playableSeconds, 0, 'f', 2)
						  .arg(eventSeconds, 0, 'f', 2);
			}
			const QString preferredMarker = camera == preferredCamera ? QStringLiteral("★ ") : QString();
			button->setText(QStringLiteral("%1%2 %3").arg(preferredMarker, marker, camera));
			if (camera == preferredCamera)
				tooltip += QStringLiteral(" — ") + T("EventDock.Preferred");
			button->setProperty("coverageTooltip", tooltip);
			button->setToolTip(tooltip);
		}

		if (haveEvent)
			requestAnglePreviews(event.id, event.in_ns + (event.out_ns - event.in_ns) / 2);

		if (haveEvent)
			sr_event_controller_free_event(&event);
		syncAngleButtonState();
	}

	void requestAnglePreviews(uint64_t eventId, uint64_t timestampNs)
	{
		if (previewTargetEventId != eventId) {
			previewTargetEventId = eventId;
			previewLoadedEventId = 0;
			for (QPushButton *button : angleButtons)
				button->setIcon(QIcon());
		}
		if (!eventId || previewLoadedEventId == eventId || anglePreviewJob)
			return;

		char *sessionPath = sr_session_get_or_create_path();
		if (!sessionPath)
			return;
		auto job = std::make_unique<AnglePreviewJob>();
		job->eventId = eventId;
		job->sessionDir = sessionPath;
		bfree(sessionPath);
		for (QPushButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE)
				continue;
			const qint64 offset = button->property("syncOffsetNs").toLongLong();
			uint64_t cameraTimestamp = timestampNs;
			if (offset >= 0 && (uint64_t)offset <= UINT64_MAX - cameraTimestamp)
				cameraTimestamp += (uint64_t)offset;
			else if (offset < 0 && (uint64_t)(-offset) < cameraTimestamp)
				cameraTimestamp -= (uint64_t)(-offset);
			AnglePreviewTask task;
			task.camera = button->property("cameraName").toString().toUtf8().constData();
			task.timestampNs = cameraTimestamp;
			job->tasks.emplace_back(std::move(task));
		}
		if (job->tasks.empty())
			return;
		anglePreviewJob = std::move(job);
		AnglePreviewJob *workerJob = anglePreviewJob.get();
		workerJob->worker = std::thread([workerJob]() { runAnglePreviewJob(workerJob); });
	}

	void pollAnglePreviews()
	{
		if (!anglePreviewJob || !anglePreviewJob->done.load(std::memory_order_acquire))
			return;
		if (anglePreviewJob->worker.joinable())
			anglePreviewJob->worker.join();
		if (anglePreviewJob->eventId == previewTargetEventId && angleEventId() == previewTargetEventId) {
			for (const AnglePreviewResult &result : anglePreviewJob->results) {
				if (result.rgba.empty())
					continue;
				QPushButton *button = angleButton(QString::fromUtf8(result.camera.c_str()));
				if (!button)
					continue;
				const QImage image(result.rgba.data(), ANGLE_PREVIEW_WIDTH, ANGLE_PREVIEW_HEIGHT,
						   ANGLE_PREVIEW_WIDTH * 4, QImage::Format_RGBA8888);
				button->setIcon(QIcon(QPixmap::fromImage(image.copy())));
			}
			previewLoadedEventId = anglePreviewJob->eventId;
		}
		anglePreviewJob.reset();
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
			const bool atPlayhead = !sameEvent ||
						(state.playhead_ns >= playableIn && state.playhead_ns <= playableOut);
			button->setEnabled(eventId && coverage != SR_REPLAY_COVERAGE_NONE && atPlayhead);
			button->setChecked(sameEvent && activeCamera == camera);
			button->setToolTip(button->property("coverageTooltip").toString());
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

	void setPreferredCamera(bool clear)
	{
		const uint64_t eventId = selectedEventId();
		if (!controller || !eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}

		const QString camera = selectedCamera();
		if (!clear && camera.isEmpty()) {
			setStatus("EventDock.NoCameraSelected");
			return;
		}
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *name = clear ? nullptr : cameraUtf8.constData();
		if (!sr_event_controller_set_preferred_camera(controller, eventId, name)) {
			setStatus("EventDock.PreferredFailed");
			return;
		}

		setStatus(clear ? "EventDock.PreferredCleared" : "EventDock.PreferredSet");
		refreshAngleCoverage();
	}

	QString playlistSummary(enum sr_replay_bus bus) const
	{
		sr_replay_playlist_state state = {};
		if (!sr_replay_playlist_get_state(bus, &state) || !state.active)
			return QString();
		return T("EventDock.PlaylistState").arg(state.list_id).arg(state.position + 1).arg(state.count);
	}

	void refreshTransportStatus()
	{
		if (!transportStatus)
			return;
		QString a = channelSummary(SR_REPLAY_BUS_A, QStringLiteral("A"));
		QString b = channelSummary(SR_REPLAY_BUS_B, QStringLiteral("B"));
		const QString pa = playlistSummary(SR_REPLAY_BUS_A);
		const QString pb = playlistSummary(SR_REPLAY_BUS_B);
		if (!pa.isEmpty())
			a += QStringLiteral("  [") + pa + QStringLiteral("]");
		if (!pb.isEmpty())
			b += QStringLiteral("  [") + pb + QStringLiteral("]");
		transportStatus->setText(a + QStringLiteral("    ") + b);
	}

	QString cueStatus(enum sr_replay_bus bus, const QString &label) const
	{
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(bus, &state) || !state.cued)
			return T("EventDock.CueStateEmpty").arg(label);
		return T("EventDock.CueStateEvent")
			.arg(label)
			.arg(state.event_id)
			.arg(QString::fromUtf8(state.camera_name));
	}

	void refreshProgramState()
	{
		if (!programStatus || !cueAStatus || !cueBStatus)
			return;
		enum sr_replay_bus programBus;
		if (sr_replay_take_program_bus(&programBus)) {
			programStatus->setText(T("EventDock.ProgramReplay")
						       .arg(programBus == SR_REPLAY_BUS_A ? QStringLiteral("A")
											  : QStringLiteral("B")));
			programStatus->setStyleSheet(QStringLiteral(
				"font-weight: bold; color: white; background: #b52b2b; border-radius: 3px; padding: 4px;"));
		} else {
			programStatus->setText(T("EventDock.ProgramLive"));
			programStatus->setStyleSheet(QStringLiteral(
				"font-weight: bold; color: white; background: #247a3b; border-radius: 3px; padding: 4px;"));
		}
		cueAStatus->setText(cueStatus(SR_REPLAY_BUS_A, QStringLiteral("A")));
		cueBStatus->setText(cueStatus(SR_REPLAY_BUS_B, QStringLiteral("B")));
		cueAStatus->setStyleSheet(QStringLiteral(
			"font-weight: bold; color: palette(text); background: palette(alternate-base); border: 1px solid #3f78c5; border-radius: 3px; padding: 4px;"));
		cueBStatus->setStyleSheet(QStringLiteral(
			"font-weight: bold; color: palette(text); background: palette(alternate-base); border: 1px solid #d49a2a; border-radius: 3px; padding: 4px;"));
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

		int shuttlePosition = 0;
		if (state.cued && state.playing && !state.paused) {
			const int speed = (int)state.speed_percent;
			if (speed == 25)
				shuttlePosition = 1;
			else if (speed == 50)
				shuttlePosition = 2;
			else if (speed == 100)
				shuttlePosition = 3;
			else if (speed == 200)
				shuttlePosition = 4;
			else if (speed == 400)
				shuttlePosition = 5;
			if (state.backward)
				shuttlePosition = -shuttlePosition;
		}
		if (shuttleSlider) {
			const QSignalBlocker blocker(shuttleSlider);
			shuttleSlider->setValue(shuttlePosition);
		}
		if (shuttleValue) {
			const int speed = shuttleSpeed(shuttlePosition);
			shuttleValue->setText(shuttlePosition
						      ? QStringLiteral("%1%").arg(shuttlePosition < 0 ? -speed : speed)
						      : QStringLiteral("0"));
		}
		syncTimeline();
		refreshTransportStatus();
	}

	void syncTimeline()
	{
		if (!timelineSlider || !timelineTime)
			return;

		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued ||
		    state.out_ns <= state.in_ns) {
			timelineSlider->setEnabled(false);
			if (!timelineDragging)
				timelineSlider->setValue(0);
			timelineTime->setText(QStringLiteral("--:--.--- / --:--.---"));
			return;
		}

		timelineSlider->setEnabled(true);
		const uint64_t duration = state.out_ns - state.in_ns;
		const uint64_t position = state.playhead_ns <= state.in_ns    ? 0
					  : state.playhead_ns >= state.out_ns ? duration
									      : state.playhead_ns - state.in_ns;
		if (!timelineDragging) {
			const int sliderValue = (int)((long double)position * 10000.0L / (long double)duration);
			timelineSlider->setValue(sliderValue);
		}
		timelineTime->setText(replayClockText(position) + QStringLiteral(" / ") + replayClockText(duration));
	}

	void seekTimeline(int value)
	{
		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued || state.out_ns <= state.in_ns)
			return;

		if (value < 0)
			value = 0;
		if (value > 10000)
			value = 10000;
		const uint64_t duration = state.out_ns - state.in_ns;
		const uint64_t offset = (uint64_t)((long double)duration * (long double)value / 10000.0L);
		const uint64_t target = offset >= duration ? state.out_ns : state.in_ns + offset;
		sr_replay_channel_pause(transportBus(), true);
		sr_replay_channel_seek(transportBus(), target);
		timelineTime->setText(replayClockText(offset > duration ? duration : offset) + QStringLiteral(" / ") +
				      replayClockText(duration));
	}

	void jogMoved(int value)
	{
		const int delta = value - jogLastValue;
		jogLastValue = value;
		if (!delta)
			return;

		const int direction = delta > 0 ? 1 : -1;
		if (!sr_replay_channel_step_frames(transportBus(), direction))
			setStatus("EventDock.FrameStepFailed");
		refreshTransportStatus();
		syncTimeline();
	}

	static int shuttleSpeed(int position)
	{
		switch (position < 0 ? -position : position) {
		case 1:
			return 25;
		case 2:
			return 50;
		case 3:
			return 100;
		case 4:
			return 200;
		case 5:
			return 400;
		default:
			return 0;
		}
	}

	void applyShuttle(int position)
	{
		if (!shuttleSlider || !shuttleValue)
			return;

		sr_replay_channel_state state = {};
		if (!sr_replay_channel_get_state(transportBus(), &state) || !state.cued) {
			if (position != 0) {
				const QSignalBlocker blocker(shuttleSlider);
				shuttleSlider->setValue(0);
			}
			shuttleValue->setText(QStringLiteral("0"));
			setStatus("EventDock.NoCue");
			return;
		}

		if (!position) {
			sr_replay_channel_pause(transportBus(), true);
			shuttleValue->setText(QStringLiteral("0"));
			refreshTransportStatus();
			return;
		}

		const int speed = shuttleSpeed(position);
		if (!speed)
			return;
		sr_replay_channel_set_backward(transportBus(), position < 0);
		sr_replay_channel_set_speed(transportBus(), speed);
		if (state.paused)
			sr_replay_channel_pause(transportBus(), false);
		else if (!state.playing)
			sr_replay_channel_play(transportBus());
		shuttleValue->setText(QStringLiteral("%1%").arg(position < 0 ? -speed : speed));
		refreshTransportStatus();
	}

	bool cueSelected(enum sr_replay_bus bus)
	{
		const uint64_t eventId = selectedEventId();
		const QString camera = selectedCamera();
		if (!eventId) {
			setStatus("EventDock.NoEventSelected");
			return false;
		}
		if (camera.isEmpty()) {
			setStatus("EventDock.NoCameraSelected");
			return false;
		}
		QPushButton *angle = angleButton(camera);
		if (angle && angle->property("coverage").toInt() == SR_REPLAY_COVERAGE_NONE) {
			status->setText(T("EventDock.CueNoCoverage").arg(camera));
			return false;
		}
		const QByteArray cameraUtf8 = camera.toUtf8();
		sr_replay_playlist_stop(bus);
		if (!sr_replay_channel_cue(bus, eventId, cameraUtf8.constData())) {
			setStatus("EventDock.CueFailed");
			return false;
		}
		status->setText(T("EventDock.Cued")
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"))
					.arg(eventId));
		if (transportBus() == bus)
			syncTransportControls();
		refreshTransportStatus();
		return true;
	}

	void playSelectedEvent()
	{
		const enum sr_replay_bus bus = transportBus();
		if (!cueSelected(bus))
			return;
		takeBus(bus);
	}

	void startPlaylist(enum sr_replay_bus bus)
	{
		const QString camera = selectedCamera();
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
		if (!controller || !sr_replay_playlist_start(bus, currentList(), preferred)) {
			setStatus("EventDock.PlaylistFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}
		status->setText(T("EventDock.PlaylistStarted")
					.arg(currentList())
					.arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B")));
		refresh();
		refreshTransportStatus();
	}

	void nextPlaylist()
	{
		if (!sr_replay_playlist_next(transportBus())) {
			setStatus("EventDock.PlaylistFinished");
			refreshTransportStatus();
			return;
		}
		setStatus("EventDock.PlaylistAdvanced");
		refresh();
		refreshTransportStatus();
	}

	void stopPlaylist()
	{
		sr_replay_playlist_stop(transportBus());
		setStatus("EventDock.PlaylistStopped");
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

	void returnLiveBus()
	{
		if (!controller || !sr_replay_take_return(controller)) {
			setStatus("EventDock.ReturnFailed");
			return;
		}
		setStatus("EventDock.Returned");
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
		sr_replay_playlist_stop(transportBus());
		sr_replay_channel_stop(transportBus());
		refreshTransportStatus();
	}

	void restartTransport()
	{
		sr_replay_channel_restart(transportBus());
		refreshTransportStatus();
	}

	void stepFrame(int direction)
	{
		if (!sr_replay_channel_step_frames(transportBus(), direction)) {
			setStatus("EventDock.FrameStepFailed");
			return;
		}
		refreshTransportStatus();
	}

	QPushButton *angleButton(const QString &camera) const
	{
		for (QPushButton *button : angleButtons) {
			if (button->property("cameraName").toString() == camera)
				return button;
		}
		return nullptr;
	}

	bool addExportTask(std::vector<ExportTask> &tasks, const std::string &sessionDir, const QString &camera,
			   const QString &outputPath, const sr_event_record &event)
	{
		QPushButton *button = angleButton(camera);
		if (!button || button->property("coverage").toInt() != SR_REPLAY_COVERAGE_FULL)
			return false;

		ExportTask task;
		task.sessionDir = sessionDir;
		task.camera = camera.toUtf8().constData();
		task.outputPath = outputPath.toUtf8().constData();
		task.eventInNs = event.in_ns;
		task.eventOutNs = event.out_ns;
		task.syncOffsetNs = button->property("syncOffsetNs").toLongLong();
		task.includeMasterAudio = event.audio_mode == SR_EVENT_AUDIO_MASTER;
		tasks.emplace_back(std::move(task));
		return true;
	}

	void startExport()
	{
		if (exportJob) {
			setStatus("EventDock.ExportBusy");
			return;
		}

		const uint64_t eventId = selectedEventId();
		sr_event_record event = {};
		if (!controller || !eventId || !sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.NoEventSelected");
			return;
		}
		if (event.pending || event.out_ns <= event.in_ns) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ExportPending");
			return;
		}

		QString preferred;
		if (event.preferred_camera_id) {
			char *name = nullptr;
			if (sr_event_controller_get_camera_name(controller, event.preferred_camera_id, &name) && name)
				preferred = QString::fromUtf8(name);
			bfree(name);
		}

		QStringList fullAngles;
		for (QPushButton *button : angleButtons) {
			if (button->property("coverage").toInt() == SR_REPLAY_COVERAGE_FULL)
				fullAngles.append(button->property("cameraName").toString());
		}
		if (fullAngles.isEmpty()) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ExportNoCoverage");
			return;
		}

		char *sessionPath = sr_session_get_or_create_path();
		if (!sessionPath) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.ExportFailed");
			return;
		}
		const std::string sessionDir(sessionPath);
		bfree(sessionPath);

		std::vector<ExportTask> tasks;
		const bool allAngles = exportModeCombo && exportModeCombo->currentData().toInt() == 1;
		if (allAngles) {
			const QString directoryPath =
				QFileDialog::getExistingDirectory(this, T("EventDock.ExportFolder"));
			if (directoryPath.isEmpty()) {
				sr_event_controller_free_event(&event);
				return;
			}
			const QDir directory(directoryPath);
			for (const QString &camera : fullAngles)
				addExportTask(tasks, sessionDir, camera, unusedAnglePath(directory, eventId, camera),
					      event);
		} else {
			QString camera = preferred;
			if (!fullAngles.contains(camera))
				camera = selectedCamera();
			if (!fullAngles.contains(camera))
				camera = fullAngles.first();

			QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
			if (baseDirectory.isEmpty())
				baseDirectory = QDir::homePath();
			const QString suggested = QDir(baseDirectory)
							  .filePath(QStringLiteral("Event_%1_%2.mp4")
									    .arg(eventId, 6, 10, QChar('0'))
									    .arg(safeFilePart(camera)));
			QString outputPath = QFileDialog::getSaveFileName(this, T("EventDock.ExportFile"), suggested,
									  T("EventDock.ExportFilter"));
			if (outputPath.isEmpty()) {
				sr_event_controller_free_event(&event);
				return;
			}
			if (!outputPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive))
				outputPath += QStringLiteral(".mp4");
			if (QFileInfo::exists(outputPath)) {
				sr_event_controller_free_event(&event);
				setStatus("EventDock.ExportExists");
				return;
			}
			addExportTask(tasks, sessionDir, camera, outputPath, event);
		}
		sr_event_controller_free_event(&event);

		if (tasks.empty()) {
			setStatus("EventDock.ExportNoCoverage");
			return;
		}

		exportJob = std::make_unique<ExportJob>();
		exportJob->tasks = std::move(tasks);
		exportProgressBar->setValue(0);
		exportCancelButton->setEnabled(true);
		exportModeCombo->setEnabled(false);
		status->setText(T("EventDock.ExportStarted").arg(exportJob->tasks.size()));
		ExportJob *job = exportJob.get();
		job->worker = std::thread([job]() { runExportJob(job); });
	}

	void cancelExport()
	{
		if (!exportJob)
			return;
		exportJob->cancel.store(true, std::memory_order_relaxed);
		exportCancelButton->setEnabled(false);
		setStatus("EventDock.ExportCancelling");
	}

	void pollExport()
	{
		if (!exportJob)
			return;
		exportProgressBar->setValue((int)exportJob->progress.load(std::memory_order_relaxed));
		if (!exportJob->done.load(std::memory_order_acquire))
			return;

		if (exportJob->worker.joinable())
			exportJob->worker.join();
		exportProgressBar->setValue(exportJob->success ? 100 : (int)exportJob->progress.load());
		if (exportJob->success) {
			status->setText(T("EventDock.ExportComplete").arg(exportJob->completed));
		} else if (exportJob->result.error == SR_EVENT_EXPORT_CANCELLED) {
			setStatus("EventDock.ExportCancelled");
		} else {
			status->setText(
				T("EventDock.ExportError")
					.arg(QString::fromUtf8(exportJob->failedCamera.c_str()),
					     QString::fromUtf8(sr_event_export_error_text(exportJob->result.error))));
		}
		exportCancelButton->setEnabled(false);
		exportModeCombo->setEnabled(true);
		exportJob.reset();
	}

	void setMarkIn()
	{
		if (!recordingHasMedia()) {
			setStatus("EventDock.RecordBeforeMark");
			return;
		}
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
		if (!recordingHasMedia()) {
			setStatus("EventDock.RecordBeforeMark");
			return;
		}
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
		if (!controller || !table || tableEditing)
			return;
		if (!selectEventId)
			selectEventId = selectedEventId();

		uint64_t *eventIds = nullptr;
		size_t count = 0;
		if (!sr_event_controller_get_list_events(controller, currentList(), &eventIds, &count)) {
			setStatus("EventDock.Failed");
			return;
		}

		tableRefreshing = true;
		table->setUpdatesEnabled(false);
		table->setRowCount((int)count);
		int selectedRow = -1;
		for (size_t i = 0; i < count; i++) {
			sr_event_record event = {};
			if (!sr_event_controller_get_event(controller, eventIds[i], &event))
				continue;

			auto *id = new QTableWidgetItem(QString::number(event.id));
			id->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(event.id));
			id->setFlags(id->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 0, id);
			auto *duration = new QTableWidgetItem(durationText(event));
			duration->setFlags(duration->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 1, duration);
			table->setItem((int)i, 2,
				       new QTableWidgetItem(QString::number(event.speed_percent, 'f', 0) +
							    QStringLiteral("%")));
			auto *state = new QTableWidgetItem(stateText(event));
			state->setFlags(state->flags() & ~Qt::ItemIsEditable);
			table->setItem((int)i, 3, state);
			table->setItem((int)i, 4,
				       new QTableWidgetItem(QString::fromUtf8(event.name ? event.name : "")));
			table->setItem((int)i, 5, new QTableWidgetItem(QString::fromUtf8(event.tag ? event.tag : "")));
			if (event.id == selectEventId)
				selectedRow = (int)i;
			sr_event_controller_free_event(&event);
		}
		bfree(eventIds);
		table->setUpdatesEnabled(true);
		tableRefreshing = false;
		if (selectedRow >= 0)
			table->selectRow(selectedRow);
	}

	void editEvent(QTableWidgetItem *item)
	{
		if (tableRefreshing || !controller || !item ||
		    (item->column() != 2 && item->column() != 4 && item->column() != 5))
			return;
		tableEditing = false;
		QTableWidgetItem *idItem = table->item(item->row(), 0);
		const uint64_t eventId = idItem ? idItem->data(Qt::UserRole).toULongLong() : 0;
		sr_event_record event = {};
		if (!eventId || !sr_event_controller_get_event(controller, eventId, &event)) {
			setStatus("EventDock.Failed");
			return;
		}

		QString speedText = table->item(item->row(), 2)->text();
		speedText.remove(QChar('%'));
		bool speedOk = false;
		const double speed = speedText.trimmed().toDouble(&speedOk);
		const QByteArray name = table->item(item->row(), 4)->text().trimmed().toUtf8();
		const QByteArray tag = table->item(item->row(), 5)->text().trimmed().toUtf8();
		if (!speedOk || speed < 10.0 || speed > 400.0) {
			sr_event_controller_free_event(&event);
			setStatus("EventDock.InvalidSpeed");
			refresh(eventId);
			return;
		}

		const sr_event_write update = {
			.in_ns = event.in_ns,
			.out_ns = event.out_ns,
			.preferred_camera_id = event.preferred_camera_id,
			.speed_percent = speed,
			.audio_mode = event.audio_mode,
			.protected_event = event.protected_event,
			.played = event.played,
			.pending = event.pending,
			.name = name.constData(),
			.tag = tag.constData(),
		};
		const bool ok = sr_event_controller_update_event(controller, eventId, &update);
		sr_event_controller_free_event(&event);
		if (!ok)
			setStatus("EventDock.Failed");
		else
			setStatus("EventDock.Edited");
		refresh(eventId);
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
	QComboBox *exportModeCombo = nullptr;
	QGridLayout *angleGrid = nullptr;
	QVector<QPushButton *> angleButtons;
	QSlider *timelineSlider = nullptr;
	QSlider *jogSlider = nullptr;
	QSlider *shuttleSlider = nullptr;
	QLabel *timelineTime = nullptr;
	QLabel *shuttleValue = nullptr;
	QPushButton *reverseButton = nullptr;
	QPushButton *loopButton = nullptr;
	QPushButton *exportCancelButton = nullptr;
	QProgressBar *exportProgressBar = nullptr;
	QTableWidget *table = nullptr;
	QLabel *recordStatus = nullptr;
	QLabel *programStatus = nullptr;
	QLabel *cueAStatus = nullptr;
	QLabel *cueBStatus = nullptr;
	QLabel *transportStatus = nullptr;
	QLabel *status = nullptr;
	QTimer *refreshTimer = nullptr;
	QTimer *transportTimer = nullptr;
	bool timelineDragging = false;
	bool tableRefreshing = false;
	bool tableEditing = false;
	int jogLastValue = 0;
	unsigned cameraRefreshTicks = 0;
	std::unique_ptr<ExportJob> exportJob;
	std::unique_ptr<AnglePreviewJob> anglePreviewJob;
	uint64_t previewTargetEventId = 0;
	uint64_t previewLoadedEventId = 0;
};

} // namespace

QWidget *sr_event_dock_create(struct sr_event_controller *controller, QWidget *parent)
{
	return new SrEventDock(controller, parent);
}
