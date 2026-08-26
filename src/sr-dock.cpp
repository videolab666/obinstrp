/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "sr-dock.h"
#include "sr-config.h"
#include "sr-event-dock.h"
#include "sr-thumb.h"
#include "sr-capture.h"
#include "sr-credit.h"
#include "sr-scene-tracker.h"
#include "sr-session.h"
#include "sr-storage-manager.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <cstring>

#include <QWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QDateTime>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QListWidget>
#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>
#include <QIcon>
#include <QShowEvent>
#include <QSpinBox>
#include <QPainter>
#include <QSet>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>

#define THUMB_W 112
#define THUMB_H 63
#define MAX_ITEMS 10

static QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* A small "Pitel Instant Replay (version) by Systec" clickable credit label, shown
 * at the bottom of the dock and its dialogs, matching the footer convention
 * used by other OBS plugins (e.g. Exeldro's). */
static QLabel *makeCreditLabel(QWidget *parent)
{
	char buf[256];
	auto *label = new QLabel(QString::fromUtf8(sr_plugin_credit_html(buf, sizeof(buf))), parent);
	label->setTextFormat(Qt::RichText);
	label->setTextInteractionFlags(Qt::TextBrowserInteraction);
	label->setOpenExternalLinks(true);
	label->setStyleSheet("color: gray; font-size: 10px;");
	return label;
}

namespace {

QStringList nativeStingerTransitions()
{
	QStringList names;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *transition = transitions.sources.array[i];
		if (strcmp(obs_source_get_unversioned_id(transition), "obs_stinger_transition") != 0)
			continue;
		const QString name = QString::fromUtf8(obs_source_get_name(transition));
		if (!name.isEmpty() && !names.contains(name))
			names.append(name);
	}
	obs_frontend_source_list_free(&transitions);
	names.sort(Qt::CaseInsensitive);
	return names;
}

void populateStingerCombo(QComboBox *combo, const QStringList &names, const QString &saved)
{
	combo->addItem(T("Dock.StingerUseCurrent"), QString());
	for (const QString &name : names)
		combo->addItem(name, name);

	int index = combo->findData(saved);
	if (!saved.isEmpty() && index < 0) {
		combo->addItem(T("Dock.StingerMissing").arg(saved), saved);
		index = combo->count() - 1;
	}
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

QStringList nativeEventTransitions()
{
	QStringList names;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *transition = transitions.sources.array[i];
		const char *transitionId = obs_source_get_unversioned_id(transition);
		if (strcmp(transitionId, "obs_stinger_transition") == 0 || strcmp(transitionId, "cut_transition") == 0)
			continue;
		const QString name = QString::fromUtf8(obs_source_get_name(transition));
		if (!name.isEmpty() && !names.contains(name))
			names.append(name);
	}
	obs_frontend_source_list_free(&transitions);
	names.sort(Qt::CaseInsensitive);
	return names;
}

void populateEventTransitionCombo(QComboBox *combo, const QStringList &names, const QString &saved)
{
	combo->addItem(T("Dock.EventTransitionCut"), QString());
	for (const QString &name : names)
		combo->addItem(name, name);
	int index = combo->findData(saved);
	if (!saved.isEmpty() && index < 0) {
		combo->addItem(T("Dock.StingerMissing").arg(saved), saved);
		index = combo->count() - 1;
	}
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

bool enum_replay_sources(void *param, obs_source_t *source)
{
	auto *names = static_cast<QStringList *>(param);
	if (strcmp(obs_source_get_unversioned_id(source), SR_PLAYBACK_ID) == 0)
		names->append(QString::fromUtf8(obs_source_get_name(source)));
	return true;
}

/* Name of the first Pitel Instant Replay source in the current scene collection. */
QByteArray firstReplaySource()
{
	QStringList names;
	obs_enum_sources(enum_replay_sources, &names);
	return names.isEmpty() ? QByteArray() : names.first().toUtf8();
}

/* Name of the Pitel Instant Replay source whose "capture_source" setting matches
 * cameraName, or empty if none do. */
QByteArray replaySourceForCamera(const QString &cameraName)
{
	QStringList names;
	obs_enum_sources(enum_replay_sources, &names);

	QByteArray cameraUtf8 = cameraName.toUtf8();
	for (const QString &name : names) {
		QByteArray nameUtf8 = name.toUtf8();
		obs_source_t *source = obs_get_source_by_name(nameUtf8.constData());
		if (!source)
			continue;
		obs_data_t *settings = obs_source_get_settings(source);
		const char *capture = obs_data_get_string(settings, S_CAPTURE_SOURCE);
		bool match = capture && cameraUtf8 == capture;
		obs_data_release(settings);
		obs_source_release(source);
		if (match)
			return nameUtf8;
	}
	return QByteArray();
}

/* Extracts the camera name from a saved replay's base filename, of the form
 * "<camera>_<YYYYMMDD-HHMMSS>" (see sr_playback_capture_replay). Empty if the
 * name doesn't match that pattern (e.g. a file dropped in by hand). */
QString cameraNameFromFile(const QString &baseName)
{
	static const QRegularExpression re(QStringLiteral("^(.*)_\\d{8}-\\d{6}$"));
	QRegularExpressionMatch m = re.match(baseName);
	return m.hasMatch() ? m.captured(1) : QString();
}

/* The scene source that contains the given source, or nullptr (ref'd). */
obs_source_t *sceneContaining(const char *sourceName)
{
	if (!sourceName || !*sourceName)
		return nullptr;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	obs_source_t *result = nullptr;
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *sceneSrc = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		if (scene && obs_scene_find_source(scene, sourceName)) {
			result = obs_source_get_ref(sceneSrc);
			break;
		}
	}
	obs_frontend_source_list_free(&scenes);
	return result;
}

/* Stamps a green checkmark badge on the bottom-right corner of a thumbnail
 * pixmap, marking a replay that's already been sent to program so it's not
 * confused with one still waiting to be used. */
void drawPlayedBadge(QPixmap &pixmap)
{
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);

	const int badgeSize = 18;
	const int margin = 3;
	QRect badgeRect(pixmap.width() - badgeSize - margin, pixmap.height() - badgeSize - margin, badgeSize,
			badgeSize);

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(46, 204, 64));
	painter.drawRoundedRect(badgeRect, 4, 4);

	QPen checkPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(checkPen);
	painter.drawLine(badgeRect.left() + 4, badgeRect.center().y() + 1, badgeRect.left() + 7,
			 badgeRect.bottom() - 4);
	painter.drawLine(badgeRect.left() + 7, badgeRect.bottom() - 4, badgeRect.right() - 3, badgeRect.top() + 4);
}

class SrDock : public QWidget {
public:
	explicit SrDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(4, 4, 4, 4);

		// slim top bar: just a settings gear on the right
		auto *bar = new QHBoxLayout();
		bar->addStretch(1);
		auto *gear = new QToolButton(this);
		gear->setText(QString::fromUtf8("\xE2\x9A\x99")); // gear glyph
		gear->setToolTip(T("Dock.Settings"));
		gear->setAutoRaise(true);
		bar->addWidget(gear);
		root->addLayout(bar);

		list = new QListWidget(this);
		list->setViewMode(QListView::IconMode);
		list->setIconSize(QSize(THUMB_W, THUMB_H));
		list->setGridSize(QSize(THUMB_W + 20, THUMB_H + 40));
		list->setResizeMode(QListView::Adjust);
		list->setMovement(QListView::Static);
		list->setWordWrap(true);
		list->setSpacing(4);
		root->addWidget(list, 1);

		auto *hint = new QLabel(T("Dock.Hint"), this);
		hint->setWordWrap(true);
		hint->setStyleSheet("color: gray;");
		root->addWidget(hint);

		root->addWidget(makeCreditLabel(this));

		watcher = new QFileSystemWatcher(this);

		/* a new file appears before its mp4 index (moov) is finished, so
		 * debounce the refresh to let the file become readable */
		refreshTimer = new QTimer(this);
		refreshTimer->setSingleShot(true);
		connect(refreshTimer, &QTimer::timeout, this, [this]() { refreshList(); });

		char *dir = sr_config_get_save_dir();
		currentFolder = QString::fromUtf8(dir);
		bfree(dir);

		loadPlayedPaths();
		watchFolder();
		refreshList();

		connect(gear, &QToolButton::clicked, this, [this]() { openSettings(); });
		connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) { launch(item); });
		connect(watcher, &QFileSystemWatcher::directoryChanged, this, [this]() { refreshTimer->start(700); });
	}

	/* UI thread. Flags a replay as aired and badges it if it is already
	 * on the list; a replay saved seconds ago usually isn't yet, and the
	 * folder watcher's refresh picks the badge up from playedPaths. */
	void showSettings() { openSettings(); }

	void markPlayed(const QString &path)
	{
		QString abs = QFileInfo(path).absoluteFilePath();
		if (abs.isEmpty() || playedPaths.contains(abs))
			return;

		playedPaths.insert(abs);
		savePlayedPaths();

		for (int i = 0; i < list->count(); i++) {
			QListWidgetItem *item = list->item(i);
			if (item->data(Qt::UserRole).toString() != abs)
				continue;
			QPixmap pixmap = item->icon().pixmap(THUMB_W, THUMB_H);
			drawPlayedBadge(pixmap);
			item->setIcon(QIcon(pixmap));
			break;
		}
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		refreshList();
	}

private:
	void openSettings()
	{
		QDialog dlg(this);
		dlg.setWindowTitle(T("Dock.SettingsTitle"));
		auto *lay = new QVBoxLayout(&dlg);

		lay->addWidget(new QLabel(T("Dock.Folder"), &dlg));
		auto *row = new QHBoxLayout();
		auto *edit = new QLineEdit(currentFolder, &dlg);
		edit->setReadOnly(true);
		edit->setMinimumWidth(320);
		auto *browse = new QPushButton(QStringLiteral("..."), &dlg);
		browse->setMaximumWidth(36);
		row->addWidget(edit, 1);
		row->addWidget(browse);
		lay->addLayout(row);

		char *sessionRootRaw = sr_config_get_session_root();
		const QString sessionRoot = QString::fromUtf8(sessionRootRaw ? sessionRootRaw : "");
		bfree(sessionRootRaw);

		lay->addWidget(new QLabel(T("Dock.SessionFolder"), &dlg));
		auto *sessionRow = new QHBoxLayout();
		auto *sessionEdit = new QLineEdit(sessionRoot, &dlg);
		sessionEdit->setReadOnly(true);
		auto *sessionBrowse = new QPushButton(QStringLiteral("..."), &dlg);
		sessionBrowse->setMaximumWidth(36);
		sessionRow->addWidget(sessionEdit, 1);
		sessionRow->addWidget(sessionBrowse);
		lay->addLayout(sessionRow);

		const double gib = 1024.0 * 1024.0 * 1024.0;
		auto *minFree = new QDoubleSpinBox(&dlg);
		minFree->setRange(1.0, 10000.0);
		minFree->setDecimals(1);
		minFree->setSingleStep(10.0);
		minFree->setSuffix(QStringLiteral(" GB"));
		minFree->setValue((double)sr_config_get_min_free_bytes() / gib);
		lay->addWidget(new QLabel(T("Dock.MinFree"), &dlg));
		lay->addWidget(minFree);

		auto *segmentSeconds = new QDoubleSpinBox(&dlg);
		segmentSeconds->setRange(1.0, 60.0);
		segmentSeconds->setDecimals(1);
		segmentSeconds->setSingleStep(0.5);
		segmentSeconds->setSuffix(QStringLiteral(" s"));
		segmentSeconds->setValue((double)sr_config_get_segment_duration_ms() / 1000.0);
		lay->addWidget(new QLabel(T("Dock.SegmentDuration"), &dlg));
		lay->addWidget(segmentSeconds);

		char *takeInRaw = sr_config_get_take_in_transition();
		char *takeOutRaw = sr_config_get_take_out_transition();
		const QString takeIn = QString::fromUtf8(takeInRaw ? takeInRaw : "");
		const QString takeOut = QString::fromUtf8(takeOutRaw ? takeOutRaw : "");
		bfree(takeInRaw);
		bfree(takeOutRaw);
		const QStringList stingers = nativeStingerTransitions();

		lay->addWidget(new QLabel(T("Dock.StingerIn"), &dlg));
		auto *stingerIn = new QComboBox(&dlg);
		populateStingerCombo(stingerIn, stingers, takeIn);
		lay->addWidget(stingerIn);

		lay->addWidget(new QLabel(T("Dock.StingerOut"), &dlg));
		auto *stingerOut = new QComboBox(&dlg);
		populateStingerCombo(stingerOut, stingers, takeOut);
		lay->addWidget(stingerOut);

		char *eventTransitionRaw = sr_config_get_event_transition();
		const QString eventTransition = QString::fromUtf8(eventTransitionRaw ? eventTransitionRaw : "");
		bfree(eventTransitionRaw);
		auto *eventTransitionRow = new QHBoxLayout();
		eventTransitionRow->addWidget(new QLabel(T("Dock.EventTransition"), &dlg));
		auto *eventTransitionCombo = new QComboBox(&dlg);
		populateEventTransitionCombo(eventTransitionCombo, nativeEventTransitions(), eventTransition);
		eventTransitionCombo->setMinimumWidth(180);
		eventTransitionRow->addWidget(eventTransitionCombo, 1);
		auto *eventTransitionMs = new QSpinBox(&dlg);
		eventTransitionMs->setRange(50, 10000);
		eventTransitionMs->setSingleStep(50);
		eventTransitionMs->setValue((int)sr_config_get_event_transition_duration_ms());
		eventTransitionRow->addWidget(eventTransitionMs);
		eventTransitionRow->addWidget(new QLabel(T("Dock.EventTransitionMilliseconds"), &dlg));
		lay->addLayout(eventTransitionRow);
		eventTransitionMs->setEnabled(!eventTransitionCombo->currentData().toString().isEmpty());
		connect(eventTransitionCombo, &QComboBox::currentIndexChanged, &dlg,
			[eventTransitionCombo, eventTransitionMs](int) {
				eventTransitionMs->setEnabled(
					!eventTransitionCombo->currentData().toString().isEmpty());
			});
		auto *eventTransitionHint = new QLabel(T("Dock.EventTransitionHint"), &dlg);
		eventTransitionHint->setWordWrap(true);
		eventTransitionHint->setStyleSheet(QStringLiteral("color: gray;"));
		lay->addWidget(eventTransitionHint);

		auto *stingerHint = new QLabel(T("Dock.StingerHint"), &dlg);
		stingerHint->setWordWrap(true);
		stingerHint->setStyleSheet(QStringLiteral("color: gray;"));
		lay->addWidget(stingerHint);

		auto *freeSpace = new QLabel(&dlg);
		freeSpace->setStyleSheet(QStringLiteral("color: gray;"));
		lay->addWidget(freeSpace);
		auto updateFreeSpace = [sessionEdit, freeSpace, gib]() {
			const QByteArray path = sessionEdit->text().toUtf8();
			const uint64_t bytes = path.isEmpty() ? 0 : os_get_free_disk_space(path.constData());
			freeSpace->setText(T("Dock.FreeSpace").arg((double)bytes / gib, 0, 'f', 1));
		};
		updateFreeSpace();

		auto *restartHint = new QLabel(T("Dock.StorageRestartHint"), &dlg);
		restartHint->setWordWrap(true);
		restartHint->setStyleSheet(QStringLiteral("color: gray;"));
		lay->addWidget(restartHint);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);

		auto *footer = new QHBoxLayout();
		footer->addWidget(makeCreditLabel(&dlg));
		footer->addStretch(1);
		footer->addWidget(buttons);
		lay->addLayout(footer);

		connect(browse, &QPushButton::clicked, &dlg, [&]() {
			QString picked = QFileDialog::getExistingDirectory(&dlg, T("Dock.PickFolder"), edit->text());
			if (!picked.isEmpty())
				edit->setText(picked);
		});
		connect(sessionBrowse, &QPushButton::clicked, &dlg, [&]() {
			QString picked = QFileDialog::getExistingDirectory(&dlg, T("Dock.PickSessionFolder"),
									   sessionEdit->text());
			if (!picked.isEmpty()) {
				sessionEdit->setText(picked);
				updateFreeSpace();
			}
		});
		connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

		if (dlg.exec() == QDialog::Accepted) {
			currentFolder = edit->text();
			QByteArray f = currentFolder.toUtf8();
			sr_config_set_save_dir(f.constData());

			const QByteArray sessionPath = sessionEdit->text().toUtf8();
			sr_config_set_session_root(sessionPath.constData());
			sr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));
			sr_config_set_segment_duration_ms((uint32_t)(segmentSeconds->value() * 1000.0));

			const QByteArray stingerInName = stingerIn->currentData().toString().toUtf8();
			const QByteArray stingerOutName = stingerOut->currentData().toString().toUtf8();
			sr_config_set_take_in_transition(stingerInName.constData());
			sr_config_set_take_out_transition(stingerOutName.constData());
			const QByteArray eventTransitionName = eventTransitionCombo->currentData().toString().toUtf8();
			sr_config_set_event_transition(eventTransitionName.constData());
			sr_config_set_event_transition_duration_ms((uint32_t)eventTransitionMs->value());

			watchFolder();
			refreshList();
		}
	}

	void watchFolder()
	{
		if (!watcher->directories().isEmpty())
			watcher->removePaths(watcher->directories());
		if (!currentFolder.isEmpty() && QDir(currentFolder).exists())
			watcher->addPath(currentFolder);
	}

	static QString playedConfigPath()
	{
		char *path = obs_module_config_path("standalone-v1/played.json");
		QString result = path ? QString::fromUtf8(path) : QString();
		bfree(path);
		return result;
	}

	/* Persisted across OBS restarts so a crash mid-broadcast doesn't lose
	 * track of which replays already went to air. */
	void loadPlayedPaths()
	{
		QString cfgPath = playedConfigPath();
		if (cfgPath.isEmpty())
			return;
		QByteArray cfgUtf8 = cfgPath.toUtf8();
		obs_data_t *data = obs_data_create_from_json_file(cfgUtf8.constData());
		if (!data)
			return;

		obs_data_array_t *arr = obs_data_get_array(data, "played");
		if (arr) {
			const size_t count = obs_data_array_count(arr);
			for (size_t i = 0; i < count; i++) {
				obs_data_t *entry = obs_data_array_item(arr, i);
				const char *p = obs_data_get_string(entry, "path");
				if (p && *p)
					playedPaths.insert(QString::fromUtf8(p));
				obs_data_release(entry);
			}
			obs_data_array_release(arr);
		}
		obs_data_release(data);
	}

	void savePlayedPaths()
	{
		QString cfgPath = playedConfigPath();
		if (cfgPath.isEmpty())
			return;

		char *dir = obs_module_config_path("standalone-v1");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}

		obs_data_array_t *arr = obs_data_array_create();
		for (const QString &p : playedPaths) {
			obs_data_t *entry = obs_data_create();
			QByteArray pUtf8 = p.toUtf8();
			obs_data_set_string(entry, "path", pUtf8.constData());
			obs_data_array_push_back(arr, entry);
			obs_data_release(entry);
		}

		obs_data_t *data = obs_data_create();
		obs_data_set_array(data, "played", arr);
		QByteArray cfgUtf8 = cfgPath.toUtf8();
		obs_data_save_json(data, cfgUtf8.constData());
		obs_data_array_release(arr);
		obs_data_release(data);
	}

	void refreshList()
	{
		list->clear();

		if (currentFolder.isEmpty())
			return;
		QDir dir(currentFolder);
		if (!dir.exists())
			return;

		QStringList filters;
		filters << "*.mp4";
		QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

		/* drop played-markers for replays that no longer exist (deleted
		 * from the folder), so the persisted list doesn't grow stale */
		QSet<QString> existing;
		for (const QFileInfo &fi : files)
			existing.insert(fi.absoluteFilePath());
		bool pruned = false;
		for (auto it = playedPaths.begin(); it != playedPaths.end();) {
			if (!existing.contains(*it)) {
				it = playedPaths.erase(it);
				pruned = true;
			} else {
				++it;
			}
		}
		if (pruned)
			savePlayedPaths();

		int count = 0;
		for (const QFileInfo &fi : files) {
			if (count >= MAX_ITEMS)
				break;
			QByteArray path = fi.absoluteFilePath().toUtf8();

			QIcon icon;
			uint8_t *rgba = nullptr;
			if (sr_thumbnail_rgba(path.constData(), THUMB_W, THUMB_H, &rgba) && rgba) {
				QImage img(rgba, THUMB_W, THUMB_H, THUMB_W * 4, QImage::Format_RGBA8888);
				QPixmap pixmap = QPixmap::fromImage(img.copy());
				if (playedPaths.contains(fi.absoluteFilePath()))
					drawPlayedBadge(pixmap);
				icon = QIcon(pixmap);
				bfree(rgba);
			}

			auto *item = new QListWidgetItem(icon, fi.completeBaseName());
			item->setData(Qt::UserRole, fi.absoluteFilePath());
			item->setToolTip(fi.fileName());
			list->addItem(item);
			count++;
		}
	}

	void launch(QListWidgetItem *item)
	{
		if (!item)
			return;
		QString filePath = item->data(Qt::UserRole).toString();
		QByteArray path = filePath.toUtf8();

		/* route to the playback source for the camera this replay was
		 * captured from, falling back to the first one found if the
		 * filename doesn't match the expected pattern or no source
		 * claims that camera anymore */
		QString cameraName = cameraNameFromFile(QFileInfo(filePath).completeBaseName());
		QByteArray srcName = cameraName.isEmpty() ? QByteArray() : replaySourceForCamera(cameraName);
		if (srcName.isEmpty())
			srcName = firstReplaySource();
		if (srcName.isEmpty())
			return;

		obs_source_t *source = obs_get_source_by_name(srcName.constData());
		if (source) {
			sr_playback_play_file(source, path.constData());
			obs_source_release(source);
		}

		obs_source_t *scene = sceneContaining(srcName.constData());
		if (scene) {
			/* same as the send-to-program hotkey: keep the shot in
			 * preview from being swapped out for the replay */
			sr_scene_tracker_note_replay_launch();
			obs_frontend_set_current_scene(scene);
			obs_source_release(scene);
		}

		if (!playedPaths.contains(filePath)) {
			playedPaths.insert(filePath);
			savePlayedPaths();
			QPixmap pixmap = item->icon().pixmap(THUMB_W, THUMB_H);
			drawPlayedBadge(pixmap);
			item->setIcon(QIcon(pixmap));
		}
	}

	QString currentFolder;
	QListWidget *list = nullptr;
	QFileSystemWatcher *watcher = nullptr;
	QTimer *refreshTimer = nullptr;
	QSet<QString> playedPaths;
};

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

class SrStoragePanel : public QWidget {
public:
	explicit SrStoragePanel(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *rootLayout = new QVBoxLayout(this);
		rootLabel = new QLabel(this);
		rootLabel->setWordWrap(true);
		rootLayout->addWidget(rootLabel);

		table = new QTableWidget(this);
		table->setColumnCount(4);
		table->setHorizontalHeaderLabels({T("Storage.Column.Session"), T("Storage.Column.Created"),
						  T("Storage.Column.Size"), T("Storage.Column.Status")});
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->verticalHeader()->setVisible(false);
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		rootLayout->addWidget(table, 1);

		auto *buttons = new QHBoxLayout();
		auto *refresh = new QPushButton(T("Storage.Refresh"), this);
		auto *remove = new QPushButton(T("Storage.Delete"), this);
		buttons->addWidget(refresh);
		buttons->addWidget(remove);
		buttons->addStretch(1);
		rootLayout->addLayout(buttons);

		gcStatus = new QLabel(this);
		gcStatus->setWordWrap(true);
		gcStatus->setStyleSheet(QStringLiteral("color: gray;"));
		rootLayout->addWidget(gcStatus);

		connect(refresh, &QPushButton::clicked, this, [this]() { refreshSessions(); });
		connect(remove, &QPushButton::clicked, this, [this]() { deleteSelectedSession(); });

		statusTimer = new QTimer(this);
		statusTimer->setInterval(2000);
		connect(statusTimer, &QTimer::timeout, this, [this]() { refreshStatus(); });
		statusTimer->start();
		refreshSessions();
	}

private:
	void refreshStatus()
	{
		char *rootRaw = sr_config_get_session_root();
		const QString root = QString::fromUtf8(rootRaw ? rootRaw : "");
		bfree(rootRaw);
		const QByteArray rootUtf8 = root.toUtf8();
		const quint64 freeBytes = root.isEmpty() ? 0 : os_get_free_disk_space(rootUtf8.constData());
		rootLabel->setText(T("Storage.RootSummary").arg(root).arg(formattedBytes(freeBytes)));

		sr_storage_manager_status manager = {};
		sr_storage_manager_get_status(&manager);
		if (!manager.cleanup_passes) {
			gcStatus->setText(T("Storage.GcNever"));
			return;
		}
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

	void refreshSessions()
	{
		refreshStatus();
		char *rootRaw = sr_config_get_session_root();
		const QString root = QString::fromUtf8(rootRaw ? rootRaw : "");
		bfree(rootRaw);
		QDir directory(root);
		const QFileInfoList candidates = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
		QFileInfoList sessions;
		for (const QFileInfo &candidate : candidates) {
			const QString metadata =
				QDir(candidate.absoluteFilePath()).filePath(QStringLiteral("session.json"));
			if (QFileInfo(metadata).isFile())
				sessions.append(candidate);
		}
		table->setRowCount(sessions.size());
		for (int row = 0; row < sessions.size(); row++) {
			const QFileInfo &session = sessions.at(row);
			const QString path = session.absoluteFilePath();
			const bool active = sr_session_path_is_active(path.toUtf8().constData());
			auto *name = new QTableWidgetItem(session.fileName());
			name->setData(Qt::UserRole, path);
			table->setItem(row, 0, name);
			const qint64 created = sessionCreatedUnix(path);
			table->setItem(row, 1,
				       new QTableWidgetItem(QDateTime::fromSecsSinceEpoch(created).toString(
					       QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
			table->setItem(row, 2, new QTableWidgetItem(formattedBytes(directoryBytes(path))));
			table->setItem(row, 3,
				       new QTableWidgetItem(active ? T("Storage.Active") : T("Storage.Inactive")));
		}
	}

	void deleteSelectedSession()
	{
		const int row = table->currentRow();
		QTableWidgetItem *item = row >= 0 ? table->item(row, 0) : nullptr;
		if (!item)
			return;
		const QString path = item->data(Qt::UserRole).toString();
		const QByteArray pathUtf8 = path.toUtf8();
		if (sr_session_path_is_active(pathUtf8.constData())) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Storage.DeleteActive"));
			return;
		}

		char *rootRaw = sr_config_get_session_root();
		const QString canonicalRoot = QFileInfo(QString::fromUtf8(rootRaw ? rootRaw : "")).canonicalFilePath();
		bfree(rootRaw);
		const QFileInfo selected(path);
		if (canonicalRoot.isEmpty() || selected.dir().canonicalPath() != canonicalRoot) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Storage.DeleteInvalid"));
			return;
		}

		if (QMessageBox::question(this, T("Storage.DeleteTitle"),
					  T("Storage.DeleteConfirm").arg(selected.fileName())) != QMessageBox::Yes)
			return;
		if (!QDir(path).removeRecursively()) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Storage.DeleteFailed"));
			return;
		}
		refreshSessions();
	}

	QLabel *rootLabel = nullptr;
	QLabel *gcStatus = nullptr;
	QTableWidget *table = nullptr;
	QTimer *statusTimer = nullptr;
};

/* The live dock, so replays that go to air from a hotkey can be marked
 * without a lookup. QPointer so it reads back null once OBS destroys it. */
QPointer<SrDock> g_dock;

void mark_played_task(void *param)
{
	char *path = static_cast<char *>(param);
	if (g_dock)
		g_dock->markPlayed(QString::fromUtf8(path));
	bfree(path);
}

} // namespace

void sr_dock_register(struct sr_event_controller *controller)
{
	auto *tabs = new QTabWidget();
	tabs->setObjectName(QStringLiteral("PitelInstantReplayDock"));
	tabs->setDocumentMode(true);

	auto *operatorScroll = new QScrollArea(tabs);
	operatorScroll->setObjectName(QStringLiteral("PitelInstantReplayOperatorScroll"));
	operatorScroll->setWidgetResizable(true);
	operatorScroll->setFrameShape(QFrame::NoFrame);
	operatorScroll->setWidget(sr_event_dock_create(controller, operatorScroll));

	auto *clips = new SrDock(tabs);
	auto *storage = new SrStoragePanel(tabs);
	tabs->addTab(operatorScroll, T("Dock.TabOperator"));
	tabs->addTab(clips, T("Dock.TabClips"));
	tabs->addTab(storage, T("Dock.TabStorage"));
	tabs->setCurrentIndex(0);

	if (!obs_frontend_add_dock_by_id("pitel_instant_replay_dock", obs_module_text("Dock.Title"), tabs)) {
		delete tabs;
		return;
	}
	g_dock = clips;
}

void sr_dock_open_settings(void)
{
	if (g_dock)
		g_dock->showSettings();
}

void sr_dock_mark_played(const char *path)
{
	if (!path || !*path)
		return;
	/* the dock is a widget: touch it on the UI thread */
	obs_queue_task(OBS_TASK_UI, mark_played_task, bstrdup(path), false);
}
