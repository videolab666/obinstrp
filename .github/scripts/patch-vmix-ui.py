from pathlib import Path
import re

path = Path("src/sr-event-dock.cpp")
s = path.read_text(encoding="utf-8")


def must_replace(old: str, new: str, label: str) -> None:
    global s
    if old not in s:
        raise SystemExit(f"missing replacement anchor: {label}")
    s = s.replace(old, new, 1)


# Qt includes for tabs and the Play Events menu.
must_replace(
    "#include <QAbstractItemDelegate>\n#include <QComboBox>\n",
    "#include <QAbstractItemDelegate>\n#include <QAction>\n#include <QComboBox>\n",
    "QAction include",
)
must_replace(
    "#include <QLabel>\n#include <QMessageBox>\n",
    "#include <QInputDialog>\n#include <QLabel>\n#include <QLineEdit>\n#include <QMenu>\n#include <QMessageBox>\n",
    "menu/input includes",
)
must_replace(
    "#include <QTableWidget>\n#include <QTimer>\n",
    "#include <QTableWidget>\n#include <QTabBar>\n#include <QTimer>\n",
    "QTabBar include",
)

# Denser global operator styling and theme-safe hint panel.
old_style = '''\t\tsetStyleSheet(QStringLiteral("QPushButton { padding: 1px 5px; min-height: 20px; }"
\t\t\t\t\t     "QToolButton { padding: 1px 4px; min-height: 20px; }"
\t\t\t\t\t     "QComboBox { padding: 1px 4px; min-height: 20px; }"
\t\t\t\t\t     "QTableWidget::item { padding-top: 0px; padding-bottom: 0px; }"));'''
new_style = '''\t\tsetStyleSheet(QStringLiteral("QPushButton { padding: 0px 4px; min-height: 18px; }"
\t\t\t\t\t     "QToolButton { padding: 0px 4px; min-height: 18px; }"
\t\t\t\t\t     "QComboBox { padding: 0px 3px; min-height: 18px; }"
\t\t\t\t\t     "QTableWidget::item { padding: 0px 2px; }"
\t\t\t\t\t     "QHeaderView::section { padding: 1px 4px; }"
\t\t\t\t\t     "QTabBar::tab { padding: 2px 9px; min-width: 24px; min-height: 18px; }"));'''
must_replace(old_style, new_style, "compact stylesheet")

must_replace(
    '''\t\toperatorHint->setStyleSheet(
\t\t\tQStringLiteral("QLabel { color: palette(text); background: palette(alternate-base); "
\t\t\t\t       "border: 1px solid palette(mid); border-radius: 3px; padding: 3px; }"));''',
    '''\t\toperatorHint->setStyleSheet(
\t\t\tQStringLiteral("QLabel { color: palette(text); background: transparent; "
\t\t\t\t       "border: 1px solid palette(mid); border-radius: 3px; padding: 2px; }"));''',
    "operator hint theme",
)

# Replace the always-open top diagnostics card with a hidden panel mounted at the bottom.
perf_pattern = re.compile(
    r'''\n\t\tauto \*performanceBox = new QGroupBox\(T\("EventDock\.Performance\.Title"\), this\);.*?\n\t\troot->addWidget\(performanceBox\);\n''',
    re.S,
)
perf_new = '''
\t\tperformancePanel = new QWidget(this);
\t\tauto *performanceLayout = new QVBoxLayout(performancePanel);
\t\tperformanceLayout->setContentsMargins(0, 0, 0, 0);
\t\tperformanceLayout->setSpacing(1);
\t\tperformanceSummary = new QLabel(performancePanel);
\t\tperformanceSummary->setWordWrap(true);
\t\tperformanceSummary->setStyleSheet(QStringLiteral("color: gray;"));
\t\tperformanceLayout->addWidget(performanceSummary);
\t\tperformanceTable = new QTableWidget(performancePanel);
\t\tperformanceTable->setColumnCount(7);
\t\tperformanceTable->setHorizontalHeaderLabels(
\t\t\t{T("EventDock.Performance.Camera"), T("EventDock.Performance.Path"),
\t\t\t T("EventDock.Performance.Video"), T("EventDock.Performance.Gop"),
\t\t\t T("EventDock.Performance.Queue"), T("EventDock.Performance.Drops"),
\t\t\t T("EventDock.Performance.Disk")});
\t\tperformanceTable->setSelectionMode(QAbstractItemView::NoSelection);
\t\tperformanceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
\t\tperformanceTable->setFocusPolicy(Qt::NoFocus);
\t\tperformanceTable->setAlternatingRowColors(true);
\t\tperformanceTable->verticalHeader()->setVisible(false);
\t\tperformanceTable->verticalHeader()->setMinimumSectionSize(16);
\t\tperformanceTable->horizontalHeader()->setStretchLastSection(false);
\t\tperformanceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
\t\tfor (int column = 1; column < performanceTable->columnCount(); column++)
\t\t\tperformanceTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
\t\tperformanceTable->verticalHeader()->setDefaultSectionSize(18);
\t\tperformanceTable->setMinimumHeight(58);
\t\tperformanceTable->setMaximumHeight(108);
\t\tperformanceLayout->addWidget(performanceTable);
\t\tperformancePanel->setVisible(false);
'''
s, count = perf_pattern.subn(perf_new, s, count=1)
if count != 1:
    raise SystemExit(f"performance block replacement count={count}")

# vMix-style 1..20 Event List tabs above the Mark controls.
list_pattern = re.compile(
    r'''\t\tauto \*markBar = new QHBoxLayout\(\);\n\t\tmarkBar->setSpacing\(3\);\n\t\tmarkBar->addWidget\(new QLabel\(T\("EventDock\.List"\), this\)\);\n\t\tlistCombo = new QComboBox\(this\);\n\t\tfor \(unsigned i = 1; i <= SR_EVENT_LIST_COUNT; i\+\+\)\n\t\t\tlistCombo->addItem\(QString::number\(i\), i\);\n\t\tlistCombo->setCurrentIndex\(0\);\n\t\tmarkBar->addWidget\(listCombo\);\n'''
)
list_new = '''\t\tlistTabs = new QTabBar(this);
\t\tlistTabs->setExpanding(false);
\t\tlistTabs->setUsesScrollButtons(true);
\t\tlistTabs->setElideMode(Qt::ElideNone);
\t\tfor (unsigned i = 1; i <= SR_EVENT_LIST_COUNT; i++) {
\t\t\tconst int index = listTabs->addTab(QString::number(i));
\t\t\tlistTabs->setTabData(index, i);
\t\t}
\t\tlistTabs->setCurrentIndex(0);
\t\troot->addWidget(listTabs);

\t\tauto *markBar = new QHBoxLayout();
\t\tmarkBar->setSpacing(3);
'''
s, count = list_pattern.subn(list_new, s, count=1)
if count != 1:
    raise SystemExit(f"list tabs replacement count={count}")

must_replace(
    "\t\ttable->verticalHeader()->setDefaultSectionSize(22);\n",
    "\t\ttable->verticalHeader()->setMinimumSectionSize(16);\n"
    "\t\ttable->verticalHeader()->setDefaultSectionSize(18);\n"
    "\t\ttable->horizontalHeader()->setFixedHeight(21);\n",
    "event row height",
)
s = s.replace("\t\t\tlabel->setMinimumHeight(22);\n", "\t\t\tlabel->setMinimumHeight(20);\n", 1)
s = s.replace("border-radius: 3px; padding: 4px;", "border-radius: 3px; padding: 2px;", 1)

# Replace the wide playlist button cluster with a vMix-like Play Events split/menu button.
take_pattern = re.compile(
    r'''\t\tauto \*takeBar = new QHBoxLayout\(\);.*?\n\t\troot->addLayout\(takeBar\);\n''', re.S
)
take_new = '''\t\tauto *takeBar = new QHBoxLayout();
\t\ttakeBar->setSpacing(3);
\t\ttakeBar->addStretch(1);
\t\tauto *takeA = new QPushButton(T("EventDock.TakeA"), this);
\t\tauto *takeB = new QPushButton(T("EventDock.TakeB"), this);
\t\tauto *takeToggle = new QPushButton(T("EventDock.TakeToggle"), this);
\t\tauto *returnLive = new QPushButton(T("EventDock.ReturnLive"), this);

\t\tplayEventsButton = new QToolButton(this);
\t\tplayEventsButton->setText(T("EventDock.PlayMenu"));
\t\tplayEventsButton->setPopupMode(QToolButton::MenuButtonPopup);
\t\tplayEventsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
\t\tplayEventsButton->setStyleSheet(QStringLiteral("font-weight: bold;"));
\t\tauto *playMenu = new QMenu(playEventsButton);
\t\tauto *playAllAction = playMenu->addAction(T("EventDock.PlayAll"));
\t\tauto *playSelectedAction = playMenu->addAction(T("EventDock.PlaySelected"));
\t\tauto *playLastAction = playMenu->addAction(T("EventDock.PlayLast"));
\t\tauto *playByIdAction = playMenu->addAction(T("EventDock.PlayById"));
\t\tplayMenu->addSeparator();
\t\tauto *playlistAAction = playMenu->addAction(T("EventDock.PlaylistA"));
\t\tauto *playlistBAction = playMenu->addAction(T("EventDock.PlaylistB"));
\t\tauto *playlistNextAction = playMenu->addAction(T("EventDock.PlaylistNext"));
\t\tauto *playlistStopAction = playMenu->addAction(T("EventDock.PlaylistStop"));
\t\tplayEventsButton->setMenu(playMenu);

\t\ttakeBar->addWidget(playEventsButton);
\t\ttakeBar->addWidget(takeA);
\t\ttakeBar->addWidget(takeB);
\t\ttakeBar->addWidget(takeToggle);
\t\ttakeBar->addWidget(returnLive);
\t\troot->addLayout(takeBar);
'''
s, count = take_pattern.subn(take_new, s, count=1)
if count != 1:
    raise SystemExit(f"take bar replacement count={count}")

# Bottom collapsed Hardware / Performance spoiler.
status_anchor = '''\t\tstatus = new QLabel(T("EventDock.Ready"), this);
\t\tstatus->setStyleSheet(QStringLiteral("color: gray;"));
\t\troot->addWidget(status);
'''
status_new = '''\t\tstatus = new QLabel(T("EventDock.Ready"), this);
\t\tstatus->setStyleSheet(QStringLiteral("color: gray;"));
\t\troot->addWidget(status);

\t\tperformanceToggle = new QToolButton(this);
\t\tperformanceToggle->setText(T("EventDock.Performance.Title"));
\t\tperformanceToggle->setCheckable(true);
\t\tperformanceToggle->setChecked(false);
\t\tperformanceToggle->setArrowType(Qt::RightArrow);
\t\tperformanceToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
\t\tperformanceToggle->setAutoRaise(true);
\t\troot->addWidget(performanceToggle);
\t\troot->addWidget(performancePanel);
'''
must_replace(status_anchor, status_new, "performance spoiler placement")

# List tab connection.
list_connect = '''\t\tconnect(listCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
\t\t\tif (!controller || index < 0)
\t\t\t\treturn;
\t\t\tsr_event_controller_set_current_list(controller, currentList());
\t\t\trefresh();
\t\t});'''
list_connect_new = '''\t\tconnect(listTabs, &QTabBar::currentChanged, this, [this](int index) {
\t\t\tif (!controller || index < 0)
\t\t\t\treturn;
\t\t\tsr_event_controller_set_current_list(controller, currentList());
\t\t\trefresh();
\t\t});'''
must_replace(list_connect, list_connect_new, "list tab connection")

# Existing playlist/play-selected controls remain available through the menu.
old_connections = '''\t\tconnect(playlistA, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_A); });
\t\tconnect(playlistB, &QPushButton::clicked, this, [this]() { startPlaylist(SR_REPLAY_BUS_B); });
\t\tconnect(playlistNext, &QPushButton::clicked, this, [this]() { nextPlaylist(); });
\t\tconnect(playlistStop, &QPushButton::clicked, this, [this]() { stopPlaylist(); });
\t\tconnect(playSelected, &QPushButton::clicked, this, [this]() { playSelectedEvent(); });'''
new_connections = '''\t\tconnect(playEventsButton, &QToolButton::clicked, this, [this]() { playSelectedEvent(); });
\t\tconnect(playAllAction, &QAction::triggered, this, [this]() { startPlaylist(transportBus()); });
\t\tconnect(playSelectedAction, &QAction::triggered, this, [this]() { playSelectedEvent(); });
\t\tconnect(playLastAction, &QAction::triggered, this, [this]() { playLastEvent(); });
\t\tconnect(playByIdAction, &QAction::triggered, this, [this]() { playById(); });
\t\tconnect(playlistAAction, &QAction::triggered, this, [this]() { startPlaylist(SR_REPLAY_BUS_A); });
\t\tconnect(playlistBAction, &QAction::triggered, this, [this]() { startPlaylist(SR_REPLAY_BUS_B); });
\t\tconnect(playlistNextAction, &QAction::triggered, this, [this]() { nextPlaylist(); });
\t\tconnect(playlistStopAction, &QAction::triggered, this, [this]() { stopPlaylist(); });
\t\tconnect(performanceToggle, &QToolButton::toggled, this, [this](bool expanded) {
\t\t\tperformanceToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
\t\t\tperformancePanel->setVisible(expanded);
\t\t\tif (expanded)
\t\t\t\trefreshHardwareStatus();
\t\t});'''
must_replace(old_connections, new_connections, "play menu connections")

must_replace(
    "\tunsigned currentList() const { return listCombo ? listCombo->currentData().toUInt() : 1; }\n",
    "\tunsigned currentList() const\n"
    "\t{\n"
    "\t\treturn listTabs && listTabs->currentIndex() >= 0 ? listTabs->tabData(listTabs->currentIndex()).toUInt() : 1;\n"
    "\t}\n",
    "currentList tabs",
)

# Do not rebuild hidden diagnostics every 750 ms; refresh immediately when expanded instead.
must_replace(
    '''\t\tif (!performanceSummary || !performanceTable)
\t\t\treturn;''',
    '''\t\tif (!performanceSummary || !performanceTable || (performancePanel && !performancePanel->isVisible()))
\t\t\treturn;''',
    "hidden performance refresh guard",
)

# Add Play Last and Play by ID helpers beside Play Selected.
play_anchor = '''\tvoid playSelectedEvent()
\t{
\t\tconst enum sr_replay_bus bus = transportBus();
\t\tif (!cueSelected(bus))
\t\t\treturn;
\t\ttakeBus(bus);
\t}

'''
play_helpers = '''\tvoid playSelectedEvent()
\t{
\t\tconst enum sr_replay_bus bus = transportBus();
\t\tif (!cueSelected(bus))
\t\t\treturn;
\t\ttakeBus(bus);
\t}

\tbool selectEventRowById(uint64_t eventId)
\t{
\t\tif (!table || !eventId)
\t\t\treturn false;
\t\tfor (int row = 0; row < table->rowCount(); row++) {
\t\t\tQTableWidgetItem *item = table->item(row, 0);
\t\t\tif (item && item->data(Qt::UserRole).toULongLong() == eventId) {
\t\t\t\ttable->setCurrentCell(row, 0);
\t\t\t\ttable->selectRow(row);
\t\t\t\treturn true;
\t\t\t}
\t\t}
\t\treturn false;
\t}

\tvoid playLastEvent()
\t{
\t\tif (!table || table->rowCount() <= 0) {
\t\t\tsetStatus("EventDock.NoEventSelected");
\t\t\treturn;
\t\t}
\t\ttable->setCurrentCell(table->rowCount() - 1, 0);
\t\ttable->selectRow(table->rowCount() - 1);
\t\tplaySelectedEvent();
\t}

\tvoid playById()
\t{
\t\tbool accepted = false;
\t\tconst QString text = QInputDialog::getText(this, T("EventDock.PlayByIdTitle"), T("EventDock.PlayByIdPrompt"),
\t\t\t\t\t\t       QLineEdit::Normal, QString(), &accepted);
\t\tif (!accepted)
\t\t\treturn;
\t\tbool valid = false;
\t\tconst uint64_t eventId = text.trimmed().toULongLong(&valid);
\t\tif (!valid || !eventId || !selectEventRowById(eventId)) {
\t\t\tstatus->setText(T("EventDock.PlayByIdNotFound").arg(text.trimmed()).arg(currentList()));
\t\t\treturn;
\t\t}
\t\tplaySelectedEvent();
\t}

'''
must_replace(play_anchor, play_helpers, "play helpers")

# Members.
must_replace("\tQComboBox *listCombo = nullptr;\n", "\tQTabBar *listTabs = nullptr;\n", "list member")
must_replace(
    "\tQTableWidget *performanceTable = nullptr;\n\tQToolButton *recordToggle = nullptr;\n",
    "\tQTableWidget *performanceTable = nullptr;\n"
    "\tQWidget *performancePanel = nullptr;\n"
    "\tQToolButton *performanceToggle = nullptr;\n"
    "\tQToolButton *playEventsButton = nullptr;\n"
    "\tQToolButton *recordToggle = nullptr;\n",
    "performance/menu members",
)

path.write_text(s, encoding="utf-8")

# Locale labels for the vMix-like play menu.
en = Path("data/locale/en-US.ini")
e = en.read_text(encoding="utf-8")
old = 'EventDock.PlaySelected="▶ PLAY EVENT"'
new = "\n".join(
    [
        'EventDock.PlayMenu="Play Events"',
        'EventDock.PlayAll="Play All"',
        'EventDock.PlaySelected="Play Selected"',
        'EventDock.PlayLast="Play Last"',
        'EventDock.PlayById="Play by ID…"',
        'EventDock.PlayByIdTitle="Play Event by ID"',
        'EventDock.PlayByIdPrompt="Event ID"',
        'EventDock.PlayByIdNotFound="Event #%1 is not in Event List %2"',
    ]
)
if old not in e:
    raise SystemExit("missing English PlaySelected anchor")
en.write_text(e.replace(old, new, 1), encoding="utf-8")

es = Path("data/locale/es-ES.ini")
e = es.read_text(encoding="utf-8")
old = 'EventDock.PlaySelected="▶ REPRODUCIR EVENTO"'
new = "\n".join(
    [
        'EventDock.PlayMenu="Reproducir eventos"',
        'EventDock.PlayAll="Reproducir todo"',
        'EventDock.PlaySelected="Reproducir seleccionado"',
        'EventDock.PlayLast="Reproducir último"',
        'EventDock.PlayById="Reproducir por ID…"',
        'EventDock.PlayByIdTitle="Reproducir evento por ID"',
        'EventDock.PlayByIdPrompt="ID del evento"',
        'EventDock.PlayByIdNotFound="El evento #%1 no está en la Lista %2"',
    ]
)
if old not in e:
    raise SystemExit("missing Spanish PlaySelected anchor")
es.write_text(e.replace(old, new, 1), encoding="utf-8")
