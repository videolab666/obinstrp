from pathlib import Path


dock_path = Path("src/sr-dock.cpp")
dock = dock_path.read_text(encoding="utf-8")

if "#include <QDoubleSpinBox>" not in dock:
    dock = dock.replace("#include <QDialogButtonBox>\n", "#include <QDialogButtonBox>\n#include <QDoubleSpinBox>\n", 1)

old = '''\t\tlay->addWidget(new QLabel(T("Dock.Folder"), &dlg));
\t\tauto *row = new QHBoxLayout();
\t\tauto *edit = new QLineEdit(currentFolder, &dlg);
\t\tedit->setReadOnly(true);
\t\tedit->setMinimumWidth(320);
\t\tauto *browse = new QPushButton(QStringLiteral("..."), &dlg);
\t\tbrowse->setMaximumWidth(36);
\t\trow->addWidget(edit, 1);
\t\trow->addWidget(browse);
\t\tlay->addLayout(row);

\t\tauto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
'''
new = '''\t\tlay->addWidget(new QLabel(T("Dock.Folder"), &dlg));
\t\tauto *row = new QHBoxLayout();
\t\tauto *edit = new QLineEdit(currentFolder, &dlg);
\t\tedit->setReadOnly(true);
\t\tedit->setMinimumWidth(320);
\t\tauto *browse = new QPushButton(QStringLiteral("..."), &dlg);
\t\tbrowse->setMaximumWidth(36);
\t\trow->addWidget(edit, 1);
\t\trow->addWidget(browse);
\t\tlay->addLayout(row);

\t\tchar *sessionRootRaw = sr_config_get_session_root();
\t\tconst QString sessionRoot = QString::fromUtf8(sessionRootRaw ? sessionRootRaw : "");
\t\tbfree(sessionRootRaw);

\t\tlay->addWidget(new QLabel(T("Dock.SessionFolder"), &dlg));
\t\tauto *sessionRow = new QHBoxLayout();
\t\tauto *sessionEdit = new QLineEdit(sessionRoot, &dlg);
\t\tsessionEdit->setReadOnly(true);
\t\tauto *sessionBrowse = new QPushButton(QStringLiteral("..."), &dlg);
\t\tsessionBrowse->setMaximumWidth(36);
\t\tsessionRow->addWidget(sessionEdit, 1);
\t\tsessionRow->addWidget(sessionBrowse);
\t\tlay->addLayout(sessionRow);

\t\tconst double gib = 1024.0 * 1024.0 * 1024.0;
\t\tauto *minFree = new QDoubleSpinBox(&dlg);
\t\tminFree->setRange(1.0, 10000.0);
\t\tminFree->setDecimals(1);
\t\tminFree->setSingleStep(10.0);
\t\tminFree->setSuffix(QStringLiteral(" GB"));
\t\tminFree->setValue((double)sr_config_get_min_free_bytes() / gib);
\t\tlay->addWidget(new QLabel(T("Dock.MinFree"), &dlg));
\t\tlay->addWidget(minFree);

\t\tauto *segmentSeconds = new QDoubleSpinBox(&dlg);
\t\tsegmentSeconds->setRange(1.0, 60.0);
\t\tsegmentSeconds->setDecimals(1);
\t\tsegmentSeconds->setSingleStep(0.5);
\t\tsegmentSeconds->setSuffix(QStringLiteral(" s"));
\t\tsegmentSeconds->setValue((double)sr_config_get_segment_duration_ms() / 1000.0);
\t\tlay->addWidget(new QLabel(T("Dock.SegmentDuration"), &dlg));
\t\tlay->addWidget(segmentSeconds);

\t\tauto *freeSpace = new QLabel(&dlg);
\t\tfreeSpace->setStyleSheet(QStringLiteral("color: gray;"));
\t\tlay->addWidget(freeSpace);
\t\tauto updateFreeSpace = [sessionEdit, freeSpace, gib]() {
\t\t\tconst QByteArray path = sessionEdit->text().toUtf8();
\t\t\tconst uint64_t bytes = path.isEmpty() ? 0 : os_get_free_disk_space(path.constData());
\t\t\tfreeSpace->setText(T("Dock.FreeSpace").arg((double)bytes / gib, 0, 'f', 1));
\t\t};
\t\tupdateFreeSpace();

\t\tauto *restartHint = new QLabel(T("Dock.StorageRestartHint"), &dlg);
\t\trestartHint->setWordWrap(true);
\t\trestartHint->setStyleSheet(QStringLiteral("color: gray;"));
\t\tlay->addWidget(restartHint);

\t\tauto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
'''
if old not in dock:
    raise SystemExit("settings layout block not found")
dock = dock.replace(old, new, 1)

old = '''\t\tconnect(browse, &QPushButton::clicked, &dlg, [&]() {
\t\t\tQString picked = QFileDialog::getExistingDirectory(&dlg, T("Dock.PickFolder"), edit->text());
\t\t\tif (!picked.isEmpty())
\t\t\t\tedit->setText(picked);
\t\t});
\t\tconnect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
'''
new = '''\t\tconnect(browse, &QPushButton::clicked, &dlg, [&]() {
\t\t\tQString picked = QFileDialog::getExistingDirectory(&dlg, T("Dock.PickFolder"), edit->text());
\t\t\tif (!picked.isEmpty())
\t\t\t\tedit->setText(picked);
\t\t});
\t\tconnect(sessionBrowse, &QPushButton::clicked, &dlg, [&]() {
\t\t\tQString picked = QFileDialog::getExistingDirectory(&dlg, T("Dock.PickSessionFolder"), sessionEdit->text());
\t\t\tif (!picked.isEmpty()) {
\t\t\t\tsessionEdit->setText(picked);
\t\t\t\tupdateFreeSpace();
\t\t\t}
\t\t});
\t\tconnect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
'''
if old not in dock:
    raise SystemExit("settings connections block not found")
dock = dock.replace(old, new, 1)

old = '''\t\tif (dlg.exec() == QDialog::Accepted) {
\t\t\tcurrentFolder = edit->text();
\t\t\tQByteArray f = currentFolder.toUtf8();
\t\t\tsr_config_set_save_dir(f.constData());
\t\t\twatchFolder();
\t\t\trefreshList();
\t\t}
'''
new = '''\t\tif (dlg.exec() == QDialog::Accepted) {
\t\t\tcurrentFolder = edit->text();
\t\t\tQByteArray f = currentFolder.toUtf8();
\t\t\tsr_config_set_save_dir(f.constData());

\t\t\tconst QByteArray sessionPath = sessionEdit->text().toUtf8();
\t\t\tsr_config_set_session_root(sessionPath.constData());
\t\t\tsr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));
\t\t\tsr_config_set_segment_duration_ms((uint32_t)(segmentSeconds->value() * 1000.0));

\t\t\twatchFolder();
\t\t\trefreshList();
\t\t}
'''
if old not in dock:
    raise SystemExit("settings save block not found")
dock = dock.replace(old, new, 1)

dock_path.write_text(dock, encoding="utf-8")

locale_path = Path("data/locale/en-US.ini")
locale = locale_path.read_text(encoding="utf-8")
anchor = 'Dock.PickFolder="Choose the replays folder"\n'
extra = (
    'Dock.SessionFolder="Continuous replay recording folder"\n'
    'Dock.PickSessionFolder="Choose the continuous replay recording folder"\n'
    'Dock.MinFree="Minimum free disk space reserve"\n'
    'Dock.SegmentDuration="Replay segment duration"\n'
    'Dock.FreeSpace="Free space on replay disk: %1 GB"\n'
    'Dock.StorageRestartHint="Storage reserve and segment-duration changes apply to newly started continuous recorders. Toggle Continuous replay recording to disk off/on on active capture filters to restart them with the new values."\n'
)
if "Dock.SessionFolder=" not in locale:
    if anchor not in locale:
        raise SystemExit("locale insertion anchor not found")
    locale = locale.replace(anchor, anchor + extra, 1)
locale_path.write_text(locale, encoding="utf-8")
