from pathlib import Path
import re


def replace_between(path, start, end, replacement):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"start marker not found in {path}: {start!r}")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"end marker not found in {path}: {end!r}")
    p.write_text(text[:a] + replacement + text[b:], encoding="utf-8")


new_setup = r'''	bool openReplaySetup()
	{
		class ToggleSwitch final : public QCheckBox {
		public:
			explicit ToggleSwitch(QWidget *parent = nullptr) : QCheckBox(parent)
			{
				setCursor(Qt::PointingHandCursor);
				setFixedSize(44, 24);
				setFocusPolicy(Qt::StrongFocus);
			}

		protected:
			bool hitButton(const QPoint &position) const override { return rect().contains(position); }

			void paintEvent(QPaintEvent *event) override
			{
				Q_UNUSED(event);
				QPainter painter(this);
				painter.setRenderHint(QPainter::Antialiasing, true);
				const QColor off = isEnabled() ? QColor(92, 92, 92) : QColor(68, 68, 68);
				const QColor on = isEnabled() ? QColor(39, 174, 96) : QColor(76, 110, 88);
				const QRectF track(1.0, 3.0, width() - 2.0, height() - 6.0);
				painter.setPen(Qt::NoPen);
				painter.setBrush(isChecked() ? on : off);
				painter.drawRoundedRect(track, track.height() / 2.0, track.height() / 2.0);

				const qreal diameter = track.height() - 4.0;
				const qreal x = isChecked() ? track.right() - diameter - 2.0 : track.left() + 2.0;
				painter.setBrush(isEnabled() ? QColor(250, 250, 250) : QColor(170, 170, 170));
				painter.drawEllipse(QRectF(x, track.top() + 2.0, diameter, diameter));

				if (hasFocus()) {
					QPen focusPen(palette().highlight().color());
					focusPen.setWidth(1);
					painter.setPen(focusPen);
					painter.setBrush(Qt::NoBrush);
					painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 5, 5);
				}
			}
		};

		QDialog dialog(this);
		dialog.setWindowTitle(T("EventDock.Setup.Title"));
		dialog.resize(820, 500);
		auto *layout = new QVBoxLayout(&dialog);
		layout->setContentsMargins(8, 8, 8, 8);
		layout->setSpacing(5);

		auto *summary = new QLabel(&dialog);
		summary->setWordWrap(true);
		layout->addWidget(summary);
		auto *topologyBar = new QHBoxLayout();
		auto *ensureAB = new QPushButton(T("EventDock.Setup.CreateAB"), &dialog);
		topologyBar->addWidget(ensureAB);
		topologyBar->addStretch(1);
		layout->addLayout(topologyBar);

		auto *hint = new QLabel(T("EventDock.Setup.CameraHint"), &dialog);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: gray;"));
		layout->addWidget(hint);

		auto *sources = new QTableWidget(&dialog);
		sources->setColumnCount(4);
		sources->setHorizontalHeaderLabels({T("EventDock.Setup.Use"), T("EventDock.Setup.Source"),
						    T("EventDock.Setup.Compatible"), T("EventDock.Setup.Type")});
		sources->setEditTriggers(QAbstractItemView::NoEditTriggers);
		sources->setSelectionBehavior(QAbstractItemView::SelectRows);
		sources->setSelectionMode(QAbstractItemView::NoSelection);
		sources->setAlternatingRowColors(true);
		sources->verticalHeader()->setVisible(false);
		sources->verticalHeader()->setDefaultSectionSize(30);
		sources->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		sources->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		for (int column = 2; column < 4; column++)
			sources->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
		layout->addWidget(sources, 1);

		auto refreshSummary = [&]() {
			sr_replay_setup_snapshot snapshot = {};
			if (!sr_replay_setup_get_snapshot(&snapshot)) {
				summary->setText(T("EventDock.Setup.Unavailable"));
				return;
			}
			const QString sceneA = snapshot.bus_a_ready ? QString::fromUtf8(snapshot.scene_a)
								    : T("EventDock.Setup.Missing");
			const QString sceneB = snapshot.bus_b_ready ? QString::fromUtf8(snapshot.scene_b)
								    : T("EventDock.Setup.Missing");
			const QString programState =
				!snapshot.program_output_supported ? T("EventDock.Setup.ProgramUnsupportedShort")
				: snapshot.program_output_enabled  ? T("EventDock.Setup.ProgramOn")
								   : T("EventDock.Setup.ProgramOff");
			summary->setText(T("EventDock.Setup.Summary")
						 .arg(snapshot.enabled_capture_source_count)
						 .arg(snapshot.compatible_source_count)
						 .arg(sceneA)
						 .arg(sceneB)
						 .arg(snapshot.event_transition_ready ? T("EventDock.Setup.ReadyAB")
										      : T("EventDock.Setup.CutOnly")) +
					 QStringLiteral(" · PROGRAM: ") + programState);
			sr_replay_setup_free_snapshot(&snapshot);
		};

		auto makeSwitchCell = [sources](ToggleSwitch *toggle) {
			auto *container = new QWidget(sources);
			auto *cellLayout = new QHBoxLayout(container);
			cellLayout->setContentsMargins(5, 0, 5, 0);
			cellLayout->setSpacing(0);
			cellLayout->addWidget(toggle);
			cellLayout->addStretch(1);
			return container;
		};

		auto applyToggle = [&, this](ToggleSwitch *toggle, bool program, const QString &sourceName,
					   bool enabled) {
			sr_capture_recording_summary recording = {};
			if (sr_capture_get_recording_summary(&recording) && recording.requested_count) {
				if (QMessageBox::question(&dialog, T("EventDock.Setup.Title"),
							  T("EventDock.Setup.StopRecordingFirst")) != QMessageBox::Yes) {
					QSignalBlocker blocker(toggle);
					toggle->setChecked(!enabled);
					return;
				}
				setAllRecording(false);
			}

			const QByteArray sourceUtf8 = sourceName.toUtf8();
			const bool ok = program ? sr_replay_setup_set_program_output(enabled)
						: sr_replay_setup_set_capture(sourceUtf8.constData(), enabled);
			if (!ok) {
				QSignalBlocker blocker(toggle);
				toggle->setChecked(!enabled);
				QMessageBox::warning(&dialog, T("EventDock.Setup.Title"), T("EventDock.Setup.ToggleFailed"));
				return;
			}

			refreshSummary();
			refreshSetupStatus();
			refreshRecordingStatus();
			refreshCameras();
			status->setText(T("EventDock.Setup.ToggleApplied")
						.arg(sourceName)
						.arg(enabled ? T("EventDock.Setup.ProgramOn") : T("EventDock.Setup.ProgramOff")));
		};

		auto populateSources = [&]() {
			sr_replay_setup_snapshot snapshot = {};
			if (!sr_replay_setup_get_snapshot(&snapshot)) {
				sources->setRowCount(0);
				return;
			}

			sources->setRowCount((int)snapshot.source_count + 1);

			// PROGRAM is a permanent pseudo-camera row. It is always visible even
			// when the current platform cannot record it.
			auto *programToggle = new ToggleSwitch(sources);
			programToggle->setChecked(snapshot.program_output_enabled);
			programToggle->setEnabled(snapshot.program_output_supported);
			programToggle->setToolTip(snapshot.program_output_supported
							      ? T("EventDock.Setup.ProgramOutput.Tooltip")
							      : T("EventDock.Setup.ProgramOutput.Unsupported"));
			sources->setCellWidget(0, 0, makeSwitchCell(programToggle));
			sources->setItem(0, 1, new QTableWidgetItem(T("EventDock.Setup.ProgramOutputName")));
			sources->setItem(0, 2,
					 new QTableWidgetItem(snapshot.program_output_supported ? T("EventDock.Setup.Yes")
											      : T("EventDock.Setup.No")));
			sources->setItem(0, 3, new QTableWidgetItem(T("EventDock.Setup.ProgramOutputType")));
		connect(programToggle, &QCheckBox::toggled, &dialog,
			[&, programToggle](bool enabled) {
				applyToggle(programToggle, true, T("EventDock.Setup.ProgramOutputName"), enabled);
			});

			for (size_t i = 0; i < snapshot.source_count; i++) {
				const sr_replay_setup_source &entry = snapshot.sources[i];
				const int row = (int)i + 1;
				auto *toggle = new ToggleSwitch(sources);
				const bool configured = entry.has_capture && entry.capture_enabled;
				toggle->setChecked(configured);
				toggle->setEnabled(entry.compatible || entry.has_capture);
				const QString sourceName = QString::fromUtf8(entry.name);
				toggle->setToolTip(entry.compatible ? T("EventDock.Setup.ToggleCameraTooltip").arg(sourceName)
									    : T("EventDock.Setup.IncompatibleTooltip").arg(sourceName));
				sources->setCellWidget(row, 0, makeSwitchCell(toggle));
				sources->setItem(row, 1, new QTableWidgetItem(sourceName));
				sources->setItem(row, 2,
						 new QTableWidgetItem(entry.compatible ? T("EventDock.Setup.Yes")
										       : T("EventDock.Setup.No")));
				sources->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(entry.type_id)));
				connect(toggle, &QCheckBox::toggled, &dialog,
					[&, toggle, sourceName](bool enabled) {
						applyToggle(toggle, false, sourceName, enabled);
					});
			}
			sr_replay_setup_free_snapshot(&snapshot);
		};

		refreshSummary();
		populateSources();

		auto *cameraBar = new QHBoxLayout();
		auto *selectAll = new QPushButton(T("EventDock.Setup.SelectAll"), &dialog);
		auto *selectNone = new QPushButton(T("EventDock.Setup.SelectNone"), &dialog);
		cameraBar->addWidget(selectAll);
		cameraBar->addWidget(selectNone);
		cameraBar->addStretch(1);
		layout->addLayout(cameraBar);

		connect(selectAll, &QPushButton::clicked, &dialog, [sources]() {
			for (int row = 0; row < sources->rowCount(); row++) {
				QWidget *cell = sources->cellWidget(row, 0);
				auto *toggle = cell ? cell->findChild<ToggleSwitch *>() : nullptr;
				if (toggle && toggle->isEnabled())
					toggle->setChecked(true);
			}
		});
		connect(selectNone, &QPushButton::clicked, &dialog, [sources]() {
			for (int row = 0; row < sources->rowCount(); row++) {
				QWidget *cell = sources->cellWidget(row, 0);
				auto *toggle = cell ? cell->findChild<ToggleSwitch *>() : nullptr;
				if (toggle && toggle->isEnabled())
					toggle->setChecked(false);
			}
		});
		connect(ensureAB, &QPushButton::clicked, &dialog, [&, this]() {
			sr_replay_setup_result result = {};
			if (!sr_replay_setup_ensure_event_scenes(&result)) {
				QMessageBox::warning(&dialog, T("EventDock.Setup.Title"), T("EventDock.Setup.ABFailed"));
			} else {
				status->setText(T("EventDock.Setup.ABReady")
							.arg(QString::fromUtf8(result.scene_a))
							.arg(QString::fromUtf8(result.scene_b)));
			}
			refreshSummary();
			refreshSetupStatus();
		});

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
		connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		layout->addWidget(buttons);
		dialog.exec();

		refreshSetupStatus();
		refreshRecordingStatus();
		sr_replay_setup_snapshot finalSnapshot = {};
		const bool ready =
			sr_replay_setup_get_snapshot(&finalSnapshot) &&
			(finalSnapshot.enabled_capture_source_count > 0 || finalSnapshot.program_output_enabled);
		sr_replay_setup_free_snapshot(&finalSnapshot);
		return ready;
	}

'''

replace_between(
    "src/sr-event-dock.cpp",
    "\tbool openReplaySetup()\n\t{",
    "\tbool recordingPreflight()",
    new_setup,
)

new_storage = r'''class SrStoragePanel : public QWidget {
public:
	explicit SrStoragePanel(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *rootLayout = new QVBoxLayout(this);
		rootLabel = new QLabel(this);
		rootLabel->setWordWrap(true);
		rootLayout->addWidget(rootLabel);

		usageLabel = new QLabel(this);
		usageLabel->setWordWrap(true);
		usageLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
		rootLayout->addWidget(usageLabel);

		table = new QTableWidget(this);
		table->setColumnCount(4);
		table->setHorizontalHeaderLabels({T("Storage.Column.Session"), T("Storage.Column.Created"),
						  T("Storage.Column.Size"), T("Storage.Column.Status")});
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::ExtendedSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->verticalHeader()->setVisible(false);
		table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
		table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
		table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
		rootLayout->addWidget(table, 1);

		auto *buttons = new QHBoxLayout();
		auto *refresh = new QPushButton(T("Storage.Refresh"), this);
		auto *remove = new QPushButton(T("Storage.DeleteSelected"), this);
		auto *removeAll = new QPushButton(T("Storage.DeleteAll"), this);
		buttons->addWidget(refresh);
		buttons->addWidget(remove);
		buttons->addWidget(removeAll);
		buttons->addStretch(1);
		rootLayout->addLayout(buttons);

		gcStatus = new QLabel(this);
		gcStatus->setWordWrap(true);
		gcStatus->setStyleSheet(QStringLiteral("color: gray;"));
		rootLayout->addWidget(gcStatus);

		connect(refresh, &QPushButton::clicked, this, [this]() { refreshSessions(); });
		connect(remove, &QPushButton::clicked, this, [this]() { deleteSelectedSessions(); });
		connect(removeAll, &QPushButton::clicked, this, [this]() { deleteAllSessions(); });

		statusTimer = new QTimer(this);
		statusTimer->setInterval(2000);
		connect(statusTimer, &QTimer::timeout, this, [this]() { refreshStatus(); });
		statusTimer->start();
		refreshSessions();
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		refreshSessions();
	}

private:
	void updateUsageLabel()
	{
		usageLabel->setText(T("Storage.UsedSummary").arg(formattedBytes(totalSessionBytes)).arg(sessionCount));
	}

	void refreshStatus()
	{
		char *rootRaw = sr_config_get_session_root();
		const QString root = QString::fromUtf8(rootRaw ? rootRaw : "");
		bfree(rootRaw);
		const QByteArray rootUtf8 = root.toUtf8();
		const quint64 freeBytes = root.isEmpty() ? 0 : os_get_free_disk_space(rootUtf8.constData());
		rootLabel->setText(T("Storage.RootSummary").arg(root).arg(formattedBytes(freeBytes)));
		updateUsageLabel();

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
		char *rootRaw = sr_config_get_session_root();
		const QString root = QString::fromUtf8(rootRaw ? rootRaw : "");
		bfree(rootRaw);
		QDir directory(root);
		const QFileInfoList candidates = directory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
		QFileInfoList sessions;
		for (const QFileInfo &candidate : candidates) {
			const QString metadata = QDir(candidate.absoluteFilePath()).filePath(QStringLiteral("session.json"));
			if (QFileInfo(metadata).isFile())
				sessions.append(candidate);
		}

		totalSessionBytes = 0;
		sessionCount = sessions.size();
		table->setRowCount(sessions.size());
		for (int row = 0; row < sessions.size(); row++) {
			const QFileInfo &session = sessions.at(row);
			const QString path = session.absoluteFilePath();
			const bool active = sr_session_path_is_active(path.toUtf8().constData());
			const quint64 bytes = directoryBytes(path);
			totalSessionBytes += bytes;
			auto *name = new QTableWidgetItem(session.fileName());
			name->setData(Qt::UserRole, path);
			name->setData(Qt::UserRole + 1, QVariant::fromValue<qulonglong>(bytes));
			table->setItem(row, 0, name);
			const qint64 created = sessionCreatedUnix(path);
			table->setItem(row, 1,
				       new QTableWidgetItem(QDateTime::fromSecsSinceEpoch(created).toString(
					       QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
			table->setItem(row, 2, new QTableWidgetItem(formattedBytes(bytes)));
			table->setItem(row, 3,
				       new QTableWidgetItem(active ? T("Storage.Active") : T("Storage.Inactive")));
		}
		refreshStatus();
	}

	QStringList selectedSessionPaths() const
	{
		QStringList paths;
		if (!table->selectionModel())
			return paths;
		const QModelIndexList rows = table->selectionModel()->selectedRows(0);
		for (const QModelIndex &index : rows) {
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

	void deleteSelectedSessions()
	{
		const QStringList paths = selectedSessionPaths();
		if (paths.isEmpty())
			return;
		deleteSessions(paths, false);
	}

	void deleteAllSessions()
	{
		const QStringList paths = allSessionPaths();
		if (paths.isEmpty())
			return;
		deleteSessions(paths, true);
	}

	void deleteSessions(const QStringList &requestedPaths, bool all)
	{
		char *rootRaw = sr_config_get_session_root();
		const QString canonicalRoot = QFileInfo(QString::fromUtf8(rootRaw ? rootRaw : "")).canonicalFilePath();
		bfree(rootRaw);
		if (canonicalRoot.isEmpty()) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"), T("Storage.DeleteInvalid"));
			return;
		}

		QStringList deletable;
		quint64 selectedBytes = 0;
		int activeSkipped = 0;
		int invalidSkipped = 0;
		for (const QString &path : requestedPaths) {
			const QFileInfo selected(path);
			if (selected.dir().canonicalPath() != canonicalRoot) {
				invalidSkipped++;
				continue;
			}
			const QByteArray pathUtf8 = path.toUtf8();
			if (sr_session_path_is_active(pathUtf8.constData())) {
				activeSkipped++;
				continue;
			}
			deletable.append(path);
			selectedBytes += directoryBytes(path);
		}

		if (deletable.isEmpty()) {
			QMessageBox::warning(this, T("Storage.DeleteTitle"),
					     activeSkipped ? T("Storage.DeleteActive") : T("Storage.DeleteInvalid"));
			return;
		}

		const QString question = T(all ? "Storage.DeleteAllConfirm" : "Storage.DeleteManyConfirm")
						 .arg(deletable.size())
						 .arg(formattedBytes(selectedBytes))
						 .arg(activeSkipped);
		if (QMessageBox::question(this, T("Storage.DeleteTitle"), question) != QMessageBox::Yes)
			return;

		int deleted = 0;
		int errors = 0;
		for (const QString &path : deletable) {
			if (QDir(path).removeRecursively())
				deleted++;
			else
				errors++;
		}
		refreshSessions();

		const QString result = T("Storage.DeleteManyResult")
					       .arg(deleted)
					       .arg(activeSkipped)
					       .arg(invalidSkipped)
					       .arg(errors);
		if (errors || activeSkipped || invalidSkipped)
			QMessageBox::information(this, T("Storage.DeleteTitle"), result);
		else
			gcStatus->setText(result);
	}

	QLabel *rootLabel = nullptr;
	QLabel *usageLabel = nullptr;
	QLabel *gcStatus = nullptr;
	QTableWidget *table = nullptr;
	QTimer *statusTimer = nullptr;
	quint64 totalSessionBytes = 0;
	int sessionCount = 0;
};

'''

replace_between(
    "src/sr-dock.cpp",
    "class SrStoragePanel : public QWidget {",
    "/* The live dock, so replays that go to air from a hotkey can be marked",
    new_storage,
)


def replace_locale_line(path, key, value):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    line = f'{key}="{value}"'
    pattern = re.compile(rf'^{re.escape(key)}=.*$', re.MULTILINE)
    if pattern.search(text):
        text = pattern.sub(line, text, count=1)
    else:
        if not text.endswith("\n"):
            text += "\n"
        text += line + "\n"
    p.write_text(text, encoding="utf-8")


english = {
    "EventDock.Setup.CameraHint": "Switch replay sources on or off here. Enabling a camera immediately adds/enables the Pitel Instant Replay Capture filter; disabling it removes that filter. Program output is always listed as a built-in replay source.",
    "EventDock.Setup.Use": "Record",
    "EventDock.Setup.Source": "Replay source",
    "EventDock.Setup.ProgramOutputName": "Program output",
    "EventDock.Setup.ProgramOutputType": "OBS Program/PGM",
    "EventDock.Setup.ToggleCameraTooltip": "Record %1 as a replay ISO angle. Turning this on adds/enables the Pitel Capture filter; turning it off removes the filter.",
    "EventDock.Setup.IncompatibleTooltip": "%1 is not an asynchronous video source and cannot be enabled for replay capture.",
    "EventDock.Setup.ToggleApplied": "%1: %2",
    "EventDock.Setup.ToggleFailed": "The replay source could not be changed. Check the OBS log and source compatibility.",
    "EventDock.Setup.SelectAll": "Enable all available",
    "EventDock.Setup.SelectNone": "Disable all",
    "Storage.UsedSummary": "Pitel recordings: %1 used across %2 session(s)",
    "Storage.DeleteSelected": "Delete selected…",
    "Storage.DeleteAll": "Delete all…",
    "Storage.DeleteManyConfirm": "Permanently delete %1 selected session(s) using %2? Active sessions skipped: %3. This cannot be undone.",
    "Storage.DeleteAllConfirm": "Permanently delete all %1 closed session(s) using %2? Active sessions skipped: %3. This cannot be undone.",
    "Storage.DeleteManyResult": "Deleted %1 session(s); active skipped %2; invalid skipped %3; errors %4",
}

spanish = {
    "EventDock.Setup.CameraHint": "Activa o desactiva aquí las fuentes de replay. Al activar una cámara se agrega/habilita inmediatamente el filtro Pitel Instant Replay Capture; al desactivarla se elimina. La salida de programa siempre aparece como fuente integrada.",
    "EventDock.Setup.Use": "Grabar",
    "EventDock.Setup.Source": "Fuente de replay",
    "EventDock.Setup.ProgramOutputName": "Salida de programa",
    "EventDock.Setup.ProgramOutputType": "Programa/PGM de OBS",
    "EventDock.Setup.ToggleCameraTooltip": "Grabar %1 como ángulo ISO de replay. Activar agrega/habilita el filtro Pitel Capture; desactivar elimina el filtro.",
    "EventDock.Setup.IncompatibleTooltip": "%1 no es una fuente de vídeo asíncrona y no se puede habilitar para replay.",
    "EventDock.Setup.ToggleApplied": "%1: %2",
    "EventDock.Setup.ToggleFailed": "No se pudo cambiar la fuente de replay. Revisa el registro de OBS y la compatibilidad de la fuente.",
    "EventDock.Setup.SelectAll": "Activar todas disponibles",
    "EventDock.Setup.SelectNone": "Desactivar todas",
    "Storage.UsedSummary": "Grabaciones de Pitel: %1 usados en %2 sesión(es)",
    "Storage.DeleteSelected": "Eliminar seleccionadas…",
    "Storage.DeleteAll": "Eliminar todas…",
    "Storage.DeleteManyConfirm": "¿Eliminar permanentemente %1 sesión(es) seleccionada(s) que usan %2? Sesiones activas omitidas: %3. No se puede deshacer.",
    "Storage.DeleteAllConfirm": "¿Eliminar permanentemente las %1 sesión(es) cerrada(s) que usan %2? Sesiones activas omitidas: %3. No se puede deshacer.",
    "Storage.DeleteManyResult": "Eliminadas %1 sesión(es); activas omitidas %2; inválidas omitidas %3; errores %4",
}

for key, value in english.items():
    replace_locale_line("data/locale/en-US.ini", key, value)
for key, value in spanish.items():
    replace_locale_line("data/locale/es-ES.ini", key, value)

print("Applied Replay Setup toggle UI and Storage multi-delete/usage patch")
