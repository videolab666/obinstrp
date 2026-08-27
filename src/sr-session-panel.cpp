/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-dock.h"

#include "sr-capture.h"
#include "sr-config.h"
#include "sr-event-controller.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

extern "C" void sr_dock_register_impl(struct sr_event_controller *controller);

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString formattedBytes(quint64 bytes)
{
	const double gib = 1024.0 * 1024.0 * 1024.0;
	const double mib = 1024.0 * 1024.0;
	return bytes >= (quint64)gib ? QStringLiteral("%1 GB").arg((double)bytes / gib, 0, 'f', 2)
				     : QStringLiteral("%1 MB").arg((double)bytes / mib, 0, 'f', 1);
}

quint64 directoryBytes(const QString &path)
{
	quint64 total = 0;
	QDirIterator iterator(path, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
	while (iterator.hasNext()) {
		iterator.next();
		total += (quint64)iterator.fileInfo().size();
	}
	return total;
}

qint64 sessionCreatedUnix(const QString &path)
{
	const QByteArray metadataPath = QDir(path).filePath(QStringLiteral("session.json")).toUtf8();
	obs_data_t *metadata = obs_data_create_from_json_file(metadataPath.constData());
	if (!metadata)
		return QFileInfo(path).lastModified().toSecsSinceEpoch();
	const qint64 created = (qint64)obs_data_get_int(metadata, "created_unix");
	obs_data_release(metadata);
	return created;
}

class SrSessionPanel final : public QWidget {
public:
	explicit SrSessionPanel(sr_event_controller *eventController, QWidget *parent = nullptr)
		: QWidget(parent), controller(eventController)
	{
		auto *root = new QVBoxLayout(this);
		stateLabel = new QLabel(this);
		stateLabel->setWordWrap(true);
		stateLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
		root->addWidget(stateLabel);

		diskLabel = new QLabel(this);
		diskLabel->setWordWrap(true);
		diskLabel->setStyleSheet(QStringLiteral("color: gray;"));
		root->addWidget(diskLabel);

		table = new QTableWidget(this);
		table->setColumnCount(4);
		table->setHorizontalHeaderLabels(
			{T("Storage.Column.Session"), T("Storage.Column.Created"), T("Storage.Column.Size"),
			 T("Storage.Column.Status")});
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::ExtendedSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->verticalHeader()->setVisible(false);
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		root->addWidget(table, 1);

		auto *primary = new QHBoxLayout();
		newButton = new QPushButton(T("Session.New"), this);
		openButton = new QPushButton(T("Session.Open"), this);
		resumeButton = new QPushButton(T("Session.Resume"), this);
		renameButton = new QPushButton(T("Session.Rename"), this);
		primary->addWidget(newButton);
		primary->addWidget(openButton);
		primary->addWidget(resumeButton);
		primary->addWidget(renameButton);
		primary->addStretch(1);
		root->addLayout(primary);

		auto *secondary = new QHBoxLayout();
		auto *refreshButton = new QPushButton(T("Storage.Refresh"), this);
		auto *clearTargetButton = new QPushButton(T("Session.ClearTarget"), this);
		auto *deleteButton = new QPushButton(T("Storage.DeleteSelected"), this);
		secondary->addWidget(refreshButton);
		secondary->addWidget(clearTargetButton);
		secondary->addStretch(1);
		secondary->addWidget(deleteButton);
		root->addLayout(secondary);

		hintLabel = new QLabel(T("Session.Hint"), this);
		hintLabel->setWordWrap(true);
		hintLabel->setStyleSheet(QStringLiteral("color: gray;"));
		root->addWidget(hintLabel);

		connect(newButton, &QPushButton::clicked, this, [this]() { createSession(); });
		connect(openButton, &QPushButton::clicked, this, [this]() { openSelected(); });
		connect(resumeButton, &QPushButton::clicked, this, [this]() { resumeSelected(); });
		connect(renameButton, &QPushButton::clicked, this, [this]() { renameSelected(); });
		connect(refreshButton, &QPushButton::clicked, this, [this]() { refresh(); });
		connect(clearTargetButton, &QPushButton::clicked, this, [this]() {
			if (sr_session_recording_is_active()) {
				QMessageBox::information(this, T("Session.Title"), T("Session.StopBeforeTarget"));
				return;
			}
			sr_session_clear_record_target();
			refresh();
		});
		connect(deleteButton, &QPushButton::clicked, this, [this]() { deleteSelected(); });
		connect(table, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem *) { openSelected(); });
		connect(table, &QTableWidget::itemSelectionChanged, this, [this]() { updateButtons(); });

		timer = new QTimer(this);
		timer->setInterval(2000);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(false); });
		timer->start();
		refresh();
	}

private:
	QString singleSelectedPath() const
	{
		if (!table->selectionModel())
			return QString();
		const QModelIndexList rows = table->selectionModel()->selectedRows(0);
		if (rows.size() != 1)
			return QString();
		QTableWidgetItem *item = table->item(rows.first().row(), 0);
		return item ? item->data(Qt::UserRole).toString() : QString();
	}

	QStringList selectedPaths() const
	{
		QStringList paths;
		if (!table->selectionModel())
			return paths;
		for (const QModelIndex &index : table->selectionModel()->selectedRows(0)) {
			QTableWidgetItem *item = table->item(index.row(), 0);
			if (item)
				paths.append(item->data(Qt::UserRole).toString());
		}
		return paths;
	}

	void updateButtons()
	{
		const QString path = singleSelectedPath();
		const bool one = !path.isEmpty();
		openButton->setEnabled(one);
		resumeButton->setEnabled(one);
		renameButton->setEnabled(one);
	}

	bool stopRecordingForSessionChange()
	{
		if (!sr_session_recording_is_active())
			return true;
		if (QMessageBox::question(this, T("Session.Title"), T("Session.StopRecordingConfirm")) != QMessageBox::Yes)
			return false;
		size_t count = 0;
		if (!sr_capture_set_all_disk_recording(false, &count)) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.StopFailed"));
			return false;
		}
		return true;
	}

	void createSession()
	{
		if (!stopRecordingForSessionChange())
			return;
		bool accepted = false;
		const QString suggested = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
		const QString name = QInputDialog::getText(this, T("Session.NewTitle"), T("Session.NamePrompt"),
							   QLineEdit::Normal, suggested, &accepted)
				     .trimmed();
		if (!accepted)
			return;
		QByteArray utf8 = name.toUtf8();
		char *created = nullptr;
		if (!sr_session_create_new(utf8.constData(), true, true, &created) || !created) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.CreateFailed"));
			bfree(created);
			return;
		}
		if (controller)
			sr_event_controller_open_session(controller, created);
		bfree(created);
		refresh();
	}

	void openSelected()
	{
		const QString path = singleSelectedPath();
		if (path.isEmpty())
			return;
		const QByteArray utf8 = path.toUtf8();
		const bool ok = controller ? sr_event_controller_open_session(controller, utf8.constData())
					   : sr_session_open(utf8.constData());
		if (!ok)
			QMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));
		refresh();
	}

	void resumeSelected()
	{
		const QString path = singleSelectedPath();
		if (path.isEmpty())
			return;
		const QByteArray utf8 = path.toUtf8();
		if (sr_session_recording_is_active() && !sr_session_path_is_active(utf8.constData()) &&
		    !stopRecordingForSessionChange())
			return;
		if (!sr_session_set_record_target(utf8.constData())) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.TargetFailed"));
			return;
		}
		/* Resume is explicit enough to make the target the editor session too.
		 * Recording itself stays STOPPED until the normal START RECORD button. */
		if (controller)
			sr_event_controller_open_session(controller, utf8.constData());
		else
			sr_session_open(utf8.constData());
		refresh();
	}

	void renameSelected()
	{
		const QString path = singleSelectedPath();
		if (path.isEmpty())
			return;
		const QByteArray pathUtf8 = path.toUtf8();
		char *currentRaw = sr_session_get_display_name(pathUtf8.constData());
		const QString current = QString::fromUtf8(currentRaw ? currentRaw : "");
		bfree(currentRaw);
		bool accepted = false;
		const QString name = QInputDialog::getText(this, T("Session.RenameTitle"), T("Session.NamePrompt"),
							   QLineEdit::Normal, current, &accepted)
				     .trimmed();
		if (!accepted || name.isEmpty())
			return;
		const QByteArray nameUtf8 = name.toUtf8();
		if (!sr_session_rename(pathUtf8.constData(), nameUtf8.constData()))
			QMessageBox::warning(this, T("Session.Title"), T("Session.RenameFailed"));
		refresh();
	}

	void deleteSelected()
	{
		const QStringList paths = selectedPaths();
		if (paths.isEmpty())
			return;
		for (const QString &path : paths) {
			const QByteArray utf8 = path.toUtf8();
			if (sr_session_path_is_active(utf8.constData()) || sr_session_path_is_opened(utf8.constData()) ||
			    sr_session_path_is_record_target(utf8.constData())) {
				QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Session.DeleteInUse"));
				return;
			}
		}
		if (QMessageBox::question(this, T("Storage.DeleteTitle"), T("Session.DeleteConfirm").arg(paths.size())) !=
		    QMessageBox::Yes)
			return;
		int failed = 0;
		for (const QString &path : paths) {
			if (!QDir(path).removeRecursively())
				failed++;
		}
		if (failed)
			QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Session.DeleteFailed").arg(failed));
		refresh();
	}

	QString statusForPath(const QByteArray &path) const
	{
		QStringList states;
		if (sr_session_path_is_active(path.constData()))
			states << QStringLiteral("REC");
		if (sr_session_path_is_opened(path.constData()))
			states << QStringLiteral("OPEN");
		if (sr_session_path_is_record_target(path.constData()) && !sr_session_path_is_active(path.constData()))
			states << QStringLiteral("TARGET");
		return states.isEmpty() ? T("Storage.Inactive") : states.join(QStringLiteral(" · "));
	}

	void refresh(bool rescanSizes = true)
	{
		QString preserve = singleSelectedPath();
		char *rootRaw = sr_config_get_session_root();
		const QString rootPath = QString::fromUtf8(rootRaw ? rootRaw : "");
		bfree(rootRaw);
		QDir rootDir(rootPath);
		const QFileInfoList candidates = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
		QFileInfoList sessions;
		for (const QFileInfo &candidate : candidates) {
			if (QFileInfo(QDir(candidate.absoluteFilePath()).filePath(QStringLiteral("session.json"))).isFile())
				sessions.append(candidate);
		}

		quint64 totalBytes = 0;
		table->setRowCount(sessions.size());
		for (int row = 0; row < sessions.size(); row++) {
			const QString path = sessions.at(row).absoluteFilePath();
			const QByteArray pathUtf8 = path.toUtf8();
			char *nameRaw = sr_session_get_display_name(pathUtf8.constData());
			const QString displayName = QString::fromUtf8(nameRaw ? nameRaw : sessions.at(row).fileName().toUtf8());
			bfree(nameRaw);

			quint64 bytes = 0;
			if (!rescanSizes) {
				for (int oldRow = 0; oldRow < table->rowCount(); oldRow++) {
					QTableWidgetItem *old = table->item(oldRow, 0);
					if (old && old->data(Qt::UserRole).toString() == path) {
						bytes = old->data(Qt::UserRole + 1).toULongLong();
						break;
					}
				}
			}
			if (!bytes)
				bytes = directoryBytes(path);
			totalBytes += bytes;
			auto *nameItem = new QTableWidgetItem(displayName);
			nameItem->setData(Qt::UserRole, path);
			nameItem->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(bytes));
			nameItem->setToolTip(path);
			table->setItem(row, 0, nameItem);
			table->setItem(row, 1,
				       new QTableWidgetItem(QDateTime::fromSecsSinceEpoch(sessionCreatedUnix(path)).toString(
					       QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
			table->setItem(row, 2, new QTableWidgetItem(formattedBytes(bytes)));
			table->setItem(row, 3, new QTableWidgetItem(statusForPath(pathUtf8)));
			if (!preserve.isEmpty() && preserve == path)
				table->selectRow(row);
		}

		const QByteArray rootUtf8 = rootPath.toUtf8();
		const quint64 freeBytes = rootPath.isEmpty() ? 0 : os_get_free_disk_space(rootUtf8.constData());
		diskLabel->setText(T("Session.DiskSummary")
					   .arg(rootPath)
					   .arg(formattedBytes(totalBytes))
					   .arg(formattedBytes(freeBytes)));

		char *openRaw = sr_session_get_opened_path();
		char *targetRaw = sr_session_get_record_target_path();
		char *recordRaw = sr_session_get_recording_path();
		auto labelFor = [](char *path) {
			if (!path)
				return QStringLiteral("—");
			char *name = sr_session_get_display_name(path);
			QString result = QString::fromUtf8(name ? name : path);
			bfree(name);
			return result;
		};
		stateLabel->setText(T("Session.StateSummary")
					    .arg(labelFor(openRaw))
					    .arg(labelFor(targetRaw))
					    .arg(labelFor(recordRaw)));
		bfree(openRaw);
		bfree(targetRaw);
		bfree(recordRaw);
		updateButtons();
	}

	sr_event_controller *controller = nullptr;
	QLabel *stateLabel = nullptr;
	QLabel *diskLabel = nullptr;
	QLabel *hintLabel = nullptr;
	QTableWidget *table = nullptr;
	QPushButton *newButton = nullptr;
	QPushButton *openButton = nullptr;
	QPushButton *resumeButton = nullptr;
	QPushButton *renameButton = nullptr;
	QTimer *timer = nullptr;
};

QTabWidget *findUnifiedDockTabs()
{
	for (QWidget *widget : QApplication::allWidgets()) {
		auto *tabs = qobject_cast<QTabWidget *>(widget);
		if (tabs && tabs->objectName() == QStringLiteral("PitelInstantReplayDock"))
			return tabs;
	}
	return nullptr;
}

void attachSessionManager(sr_event_controller *controller)
{
	QTabWidget *tabs = findUnifiedDockTabs();
	if (!tabs)
		return;
	int index = -1;
	const QString storageTitle = T("Dock.TabStorage");
	for (int i = 0; i < tabs->count(); i++) {
		if (tabs->tabText(i) == storageTitle) {
			index = i;
			break;
		}
	}
	if (index < 0)
		return;
	QWidget *old = tabs->widget(index);
	auto *panel = new SrSessionPanel(controller, tabs);
	tabs->removeTab(index);
	tabs->insertTab(index, panel, storageTitle);
	if (old)
		old->deleteLater();
}

} // namespace

extern "C" void sr_dock_register(struct sr_event_controller *controller)
{
	sr_dock_register_impl(controller);
	attachSessionManager(controller);
}
