/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-dock.h"

#include "sr-event-controller.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>

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
		actionBar->addWidget(up);
		actionBar->addWidget(down);
		actionBar->addWidget(played);
		actionBar->addWidget(protect);
		actionBar->addWidget(remove);
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
		connect(remove, &QPushButton::clicked, this, [this]() { deleteSelected(); });
		connect(copy, &QPushButton::clicked, this, [this]() { copySelected(); });
		connect(move, &QPushButton::clicked, this, [this]() { moveSelected(); });
		connect(duplicate, &QPushButton::clicked, this, [this]() { duplicateSelected(); });

		refreshTimer = new QTimer(this);
		refreshTimer->setInterval(750);
		connect(refreshTimer, &QTimer::timeout, this, [this]() { refresh(); });
		refreshTimer->start();

		if (controller)
			sr_event_controller_set_current_list(controller, currentList());
		refresh();
	}

private:
	unsigned currentList() const { return listCombo ? listCombo->currentData().toUInt() : 1; }

	unsigned targetList() const { return targetCombo ? targetCombo->currentData().toUInt() : 1; }

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

	void deleteSelected()
	{
		const uint64_t eventId = selectedEventId();
		if (!eventId)
			return;
		if (QMessageBox::question(this, T("EventDock.DeleteTitle"), T("EventDock.DeleteConfirm")) !=
		    QMessageBox::Yes)
			return;
		if (!sr_event_controller_delete_event(controller, eventId)) {
			setStatus("EventDock.Failed");
			return;
		}
		setStatus("EventDock.Deleted");
		refresh();
	}

	sr_event_controller *controller = nullptr;
	QComboBox *listCombo = nullptr;
	QComboBox *targetCombo = nullptr;
	QTableWidget *table = nullptr;
	QLabel *status = nullptr;
	QTimer *refreshTimer = nullptr;
};

} // namespace

void sr_event_dock_register(struct sr_event_controller *controller)
{
	auto *dock = new SrEventDock(controller);
	dock->setObjectName(QStringLiteral("SportsReplayEventDock"));
	if (!obs_frontend_add_dock_by_id("sports_replay_event_dock", obs_module_text("EventDock.Title"), dock))
		delete dock;
}
