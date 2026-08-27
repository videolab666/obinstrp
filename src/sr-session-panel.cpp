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
#include "sr-storage-manager.h"

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
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
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

QString sessionName(const QString &path)
{
	const QByteArray utf8 = path.toUtf8();
	char *raw = sr_session_get_display_name(utf8.constData());
	const QString result = raw && *raw ? QString::fromUtf8(raw) : QFileInfo(path).fileName();
	bfree(raw);
	return result;
}

class SrSessionPanel final : public QWidget {
public:
	explicit SrSessionPanel(sr_event_controller *eventController, QWidget *parent = nullptr)
		: QWidget(parent),
		  controller(eventController)
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
		table->setHorizontalHeaderLabels({T("Storage.Column.Session"), T("Storage.Column.Created"),
						  T("Storage.Column.Size"), T("Storage.Column.Status")});
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::ExtendedSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->verticalHeader()->setVisible(false);
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		for (int column = 1; column < 4; column++)
			table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
		root->addWidget(table, 1);

		auto *primary = new QHBoxLayout();
		newButton = new QPushButton(T("Session.New"), this);
		openButton = new QPushButton(T("Session.Open"), this);
		resumeButton = new QPushButton(T("Session.Resume"), this);
		renameButton = new QPushButton(T("Session.Rename"), this);
		returnToRecordingButton = new QPushButton(T("Session.ReturnToRecording"), this);
		primary->addWidget(newButton);
		primary->addWidget(openButton);
		primary->addWidget(resumeButton);
		primary->addWidget(renameButton);
		primary->addWidget(returnToRecordingButton);
		primary->addStretch(1);
		root->addLayout(primary);

		auto *secondary = new QHBoxLayout();
		auto *refreshButton = new QPushButton(T("Storage.Refresh"), this);
		auto *clearTargetButton = new QPushButton(T("Session.ClearTarget"), this);
		auto *deleteButton = new QPushButton(T("Storage.DeleteSelected"), this);
		auto *deleteAllButton = new QPushButton(T("Storage.DeleteAll"), this);
		secondary->addWidget(refreshButton);
		secondary->addWidget(clearTargetButton);
		secondary->addStretch(1);
		secondary->addWidget(deleteButton);
		secondary->addWidget(deleteAllButton);
		root->addLayout(secondary);

		gcStatus = new QLabel(this);
		gcStatus->setWordWrap(true);
		gcStatus->setStyleSheet(QStringLiteral("color: gray;"));
		root->addWidget(gcStatus);

		auto *hint = new QLabel(T("Session.Hint"), this);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: gray;"));
		root->addWidget(hint);

		connect(newButton, &QPushButton::clicked, this, [this]() { createSession(); });
		connect(openButton, &QPushButton::clicked, this, [this]() { openSelected(); });
		connect(resumeButton, &QPushButton::clicked, this, [this]() { resumeSelected(); });
		connect(renameButton, &QPushButton::clicked, this, [this]() { renameSelected(); });
		connect(returnToRecordingButton, &QPushButton::clicked, this, [this]() { returnToRecording(); });
		connect(refreshButton, &QPushButton::clicked, this, [this]() { refresh(); });
		connect(clearTargetButton, &QPushButton::clicked, this, [this]() { clearTarget(); });
		connect(deleteButton, &QPushButton::clicked, this, [this]() { deleteSelected(); });
		connect(deleteAllButton, &QPushButton::clicked, this, [this]() { deleteAllSessions(); });
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

	QStringList allSessionPaths() const
	{
		QStringList paths;
		for (int row = 0; row < table->rowCount(); row++) {
			QTableWidgetItem *item = table->item(row, 0);
			if (item)
				paths.append(item->data(Qt::UserRole).toString());
		}
		return paths;
	}

	void updateButtons()
	{
		const bool one = !singleSelectedPath().isEmpty();
		openButton->setEnabled(one);
		resumeButton->setEnabled(one && !sr_session_recording_is_active());
		renameButton->setEnabled(one);

		char *opened = sr_session_get_opened_path();
		char *recording = sr_session_get_recording_path();
		const bool canReturn = opened && recording && strcmp(opened, recording) != 0;
		returnToRecordingButton->setVisible(canReturn);
		bfree(opened);
		bfree(recording);
	}

	bool stopRecordingForSessionChange()
	{
		if (!sr_session_recording_is_active())
			return true;
		if (QMessageBox::question(this, T("Session.Title"), T("Session.StopRecordingConfirm")) !=
		    QMessageBox::Yes)
			return false;
		size_t count = 0;
		if (!sr_capture_set_all_disk_recording(false, &count)) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.StopFailed"));
			return false;
		}
		return true;
	}

	bool openPath(const QString &path)
	{
		if (path.isEmpty())
			return false;
		const QByteArray utf8 = path.toUtf8();
		return controller ? sr_event_controller_open_session(controller, utf8.constData())
				  : sr_session_open(utf8.constData());
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
		const QByteArray utf8 = name.toUtf8();
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
		if (!openPath(path)) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));
			refresh();
			return;
		}
		if (!sr_session_recording_is_active())
			sr_session_clear_record_target();
		refresh();
	}

	void resumeSelected()
	{
		const QString path = singleSelectedPath();
		if (path.isEmpty() || sr_session_recording_is_active())
			return;
		const QByteArray utf8 = path.toUtf8();
		if (!sr_session_set_record_target(utf8.constData())) {
			QMessageBox::warning(this, T("Session.Title"), T("Session.TargetFailed"));
			return;
		}
		if (!openPath(path)) {
			sr_session_clear_record_target();
			QMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));
		}
		refresh();
	}

	void returnToRecording()
	{
		char *recording = sr_session_get_recording_path();
		if (!recording)
			return;
		const QString path = QString::fromUtf8(recording);
		bfree(recording);
		if (!openPath(path))
			QMessageBox::warning(this, T("Session.Title"), T("Session.OpenFailed"));
		refresh();
	}

	void renameSelected()
	{
		const QString path = singleSelectedPath();
		if (path.isEmpty())
			return;
		const QString current = sessionName(path);
		bool accepted = false;
		const QString name = QInputDialog::getText(this, T("Session.RenameTitle"), T("Session.NamePrompt"),
							   QLineEdit::Normal, current, &accepted)
					     .trimmed();
		if (!accepted || name.isEmpty())
			return;
		const QByteArray pathUtf8 = path.toUtf8();
		const QByteArray nameUtf8 = name.toUtf8();
		if (!sr_session_rename(pathUtf8.constData(), nameUtf8.constData()))
			QMessageBox::warning(this, T("Session.Title"), T("Session.RenameFailed"));
		refresh();
	}

	void clearTarget()
	{
		if (sr_session_recording_is_active()) {
			QMessageBox::information(this, T("Session.Title"), T("Session.StopBeforeTarget"));
			return;
		}
		sr_session_clear_record_target();
		refresh();
	}

	void deleteSelected() { deleteSessions(selectedPaths(), false); }

	void deleteAllSessions() { deleteSessions(allSessionPaths(), true); }

	void deleteSessions(const QStringList &requestedPaths, bool all)
	{
		if (requestedPaths.isEmpty())
			return;

		char *rootRaw = sr_config_get_session_root();
		const QString canonicalRoot = QFileInfo(QString::fromUtf8(rootRaw ? rootRaw : "")).canonicalFilePath();
		bfree(rootRaw);
		if (canonicalRoot.isEmpty()) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Storage.DeleteInvalid"));
			return;
		}

		QStringList deletable;
		quint64 selectedBytes = 0;
		int inUseSkipped = 0;
		int invalidSkipped = 0;
		for (const QString &path : requestedPaths) {
			const QFileInfo selected(path);
			const QString canonicalPath = selected.canonicalFilePath();
			if (canonicalPath.isEmpty() || selected.dir().canonicalPath() != canonicalRoot) {
				invalidSkipped++;
				continue;
			}

			const QByteArray utf8 = canonicalPath.toUtf8();
			if (sr_session_path_is_active(utf8.constData()) ||
			    sr_session_path_is_opened(utf8.constData()) ||
			    sr_session_path_is_record_target(utf8.constData())) {
				inUseSkipped++;
				continue;
			}
			deletable.append(canonicalPath);
			selectedBytes += directoryBytes(canonicalPath);
		}

		if (deletable.isEmpty()) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"),
					     inUseSkipped ? T("Session.DeleteInUse") : T("Storage.DeleteInvalid"));
			return;
		}

		const QString question = T(all ? "Session.DeleteAllConfirm" : "Session.DeleteManyConfirm")
						 .arg(deletable.size())
						 .arg(formattedBytes(selectedBytes))
						 .arg(inUseSkipped);
		if (QMessageBox::question(this, T("Storage.DeleteTitle"), question) != QMessageBox::Yes)
			return;

		int deleted = 0;
		int errors = 0;
		for (const QString &path : deletable) {
			if (QDir(path).removeRecursively()) {
				sessionSizeCache.remove(path);
				deleted++;
			} else {
				errors++;
			}
		}
		refresh();

		const QString result =
			T("Session.DeleteManyResult").arg(deleted).arg(inUseSkipped).arg(invalidSkipped).arg(errors);
		if (errors || inUseSkipped || invalidSkipped)
			QMessageBox::information(this, T("Storage.DeleteTitle"), result);
		else
			gcStatus->setText(result);
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
		const QString preserve = singleSelectedPath();
		char *rootRaw = sr_config_get_session_root();
		const QString rootPath = QString::fromUtf8(rootRaw ? rootRaw : "");
		bfree(rootRaw);
		QDir rootDir(rootPath);
		const QFileInfoList candidates = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
		QFileInfoList sessions;
		for (const QFileInfo &candidate : candidates) {
			const QString metadata =
				QDir(candidate.absoluteFilePath()).filePath(QStringLiteral("session.json"));
			if (QFileInfo(metadata).isFile())
				sessions.append(candidate);
		}

		quint64 totalBytes = 0;
		table->setRowCount(sessions.size());
		int selectedRow = -1;
		for (int row = 0; row < sessions.size(); row++) {
			const QString path = sessions.at(row).absoluteFilePath();
			const QByteArray pathUtf8 = path.toUtf8();
			quint64 bytes = sessionSizeCache.value(path, 0);
			if (rescanSizes || !sessionSizeCache.contains(path)) {
				bytes = directoryBytes(path);
				sessionSizeCache.insert(path, bytes);
			}
			totalBytes += bytes;

			auto *nameItem = new QTableWidgetItem(sessionName(path));
			nameItem->setData(Qt::UserRole, path);
			nameItem->setToolTip(path);
			table->setItem(row, 0, nameItem);
			table->setItem(row, 1,
				       new QTableWidgetItem(QDateTime::fromSecsSinceEpoch(sessionCreatedUnix(path))
								    .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
			table->setItem(row, 2, new QTableWidgetItem(formattedBytes(bytes)));
			table->setItem(row, 3, new QTableWidgetItem(statusForPath(pathUtf8)));
			if (!preserve.isEmpty() && preserve == path)
				selectedRow = row;
		}
		if (selectedRow >= 0)
			table->selectRow(selectedRow);

		const QByteArray rootUtf8 = rootPath.toUtf8();
		const quint64 freeBytes = rootPath.isEmpty() ? 0 : os_get_free_disk_space(rootUtf8.constData());
		diskLabel->setText(T("Session.DiskSummary")
					   .arg(rootPath)
					   .arg(formattedBytes(totalBytes))
					   .arg(formattedBytes(freeBytes)));

		sr_storage_manager_status manager = {};
		sr_storage_manager_get_status(&manager);
		if (!manager.cleanup_passes) {
			gcStatus->setText(T("Storage.GcNever"));
		} else {
			const QString when = QDateTime::fromSecsSinceEpoch((qint64)manager.last_cleanup_unix)
						     .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
			gcStatus->setText(T("Storage.GcSummary")
						  .arg(manager.cleanup_passes)
						  .arg(when)
						  .arg(manager.last_cleanup.segments_deleted)
						  .arg(manager.last_cleanup.segments_pinned)
						  .arg(manager.last_cleanup.errors)
						  .arg(formattedBytes(manager.last_cleanup.free_bytes_before))
						  .arg(formattedBytes(manager.last_cleanup.free_bytes_after)));
		}

		char *openedRaw = sr_session_get_opened_path();
		char *targetRaw = sr_session_get_record_target_path();
		char *recordRaw = sr_session_get_recording_path();
		auto labelFor = [](char *path) {
			return path ? sessionName(QString::fromUtf8(path)) : QStringLiteral("—");
		};
		stateLabel->setText(T("Session.StateSummary")
					    .arg(labelFor(openedRaw))
					    .arg(labelFor(targetRaw))
					    .arg(labelFor(recordRaw)));
		bfree(openedRaw);
		bfree(targetRaw);
		bfree(recordRaw);
		updateButtons();
	}

	sr_event_controller *controller = nullptr;
	QLabel *stateLabel = nullptr;
	QLabel *diskLabel = nullptr;
	QLabel *gcStatus = nullptr;
	QTableWidget *table = nullptr;
	QPushButton *newButton = nullptr;
	QPushButton *openButton = nullptr;
	QPushButton *resumeButton = nullptr;
	QPushButton *renameButton = nullptr;
	QPushButton *returnToRecordingButton = nullptr;
	QTimer *timer = nullptr;
	QHash<QString, quint64> sessionSizeCache;
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
	const QString storageTitle = T("Dock.TabStorage");
	for (int index = 0; index < tabs->count(); index++) {
		if (tabs->tabText(index) != storageTitle)
			continue;
		QWidget *old = tabs->widget(index);
		auto *panel = new SrSessionPanel(controller, tabs);
		tabs->removeTab(index);
		tabs->insertTab(index, panel, storageTitle);
		if (old)
			old->deleteLater();
		return;
	}
}

} // namespace

extern "C" void sr_dock_register(struct sr_event_controller *controller)
{
	sr_dock_register_impl(controller);
	attachSessionManager(controller);
}
