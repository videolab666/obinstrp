/*
 * Pitel Instant Replay - OBS dock shell
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include "sr-dock.h"

#include "sr-config.h"
#include "sr-event-dock.h"
#include "sr-multiview-dock.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <cstring>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace {

QString trKey(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QStringList transitionNames(bool stingersOnly, bool excludeStingers)
{
	QStringList result;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *source = transitions.sources.array[i];
		const char *id = obs_source_get_unversioned_id(source);
		const bool stinger = id && strcmp(id, "obs_stinger_transition") == 0;
		if (stingersOnly && !stinger)
			continue;
		if (excludeStingers && (stinger || (id && strcmp(id, "cut_transition") == 0)))
			continue;
		const QString name = QString::fromUtf8(obs_source_get_name(source));
		if (!name.isEmpty() && !result.contains(name))
			result.append(name);
	}
	obs_frontend_source_list_free(&transitions);
	result.sort(Qt::CaseInsensitive);
	return result;
}

void fillTransitionCombo(QComboBox *combo, const QStringList &names, const QString &selected, const QString &emptyLabel)
{
	combo->addItem(emptyLabel, QString());
	for (const QString &name : names)
		combo->addItem(name, name);
	int index = combo->findData(selected);
	if (!selected.isEmpty() && index < 0) {
		combo->addItem(selected, selected);
		index = combo->count() - 1;
	}
	combo->setCurrentIndex(index >= 0 ? index : 0);
}

void openSettingsDialog()
{
	QDialog dialog;
	dialog.setWindowTitle(trKey("Dock.SettingsTitle"));
	dialog.setMinimumWidth(520);
	auto *root = new QVBoxLayout(&dialog);
	auto *form = new QFormLayout();
	root->addLayout(form);

	char *sessionRootRaw = sr_config_get_session_root();
	const QString sessionRoot = QString::fromUtf8(sessionRootRaw ? sessionRootRaw : "");
	bfree(sessionRootRaw);

	auto *sessionRow = new QWidget(&dialog);
	auto *sessionLayout = new QHBoxLayout(sessionRow);
	sessionLayout->setContentsMargins(0, 0, 0, 0);
	auto *sessionEdit = new QLineEdit(sessionRoot, sessionRow);
	sessionEdit->setReadOnly(true);
	auto *browse = new QPushButton(QStringLiteral("..."), sessionRow);
	browse->setMaximumWidth(40);
	sessionLayout->addWidget(sessionEdit, 1);
	sessionLayout->addWidget(browse);
	form->addRow(trKey("Dock.SessionFolder"), sessionRow);

	const double gib = 1024.0 * 1024.0 * 1024.0;
	auto *minFree = new QDoubleSpinBox(&dialog);
	minFree->setRange(1.0, 10000.0);
	minFree->setDecimals(1);
	minFree->setSuffix(QStringLiteral(" GB"));
	minFree->setValue((double)sr_config_get_min_free_bytes() / gib);
	form->addRow(trKey("Dock.MinFree"), minFree);

	auto *segment = new QDoubleSpinBox(&dialog);
	segment->setRange(1.0, 60.0);
	segment->setDecimals(1);
	segment->setSingleStep(0.5);
	segment->setSuffix(QStringLiteral(" s"));
	segment->setValue((double)sr_config_get_segment_duration_ms() / 1000.0);
	form->addRow(trKey("Dock.SegmentDuration"), segment);

	char *takeInRaw = sr_config_get_take_in_transition();
	char *takeOutRaw = sr_config_get_take_out_transition();
	char *eventRaw = sr_config_get_event_transition();
	const QString takeIn = QString::fromUtf8(takeInRaw ? takeInRaw : "");
	const QString takeOut = QString::fromUtf8(takeOutRaw ? takeOutRaw : "");
	const QString eventTransition = QString::fromUtf8(eventRaw ? eventRaw : "");
	bfree(takeInRaw);
	bfree(takeOutRaw);
	bfree(eventRaw);

	const QStringList stingers = transitionNames(true, false);
	auto *takeInCombo = new QComboBox(&dialog);
	auto *takeOutCombo = new QComboBox(&dialog);
	fillTransitionCombo(takeInCombo, stingers, takeIn, trKey("Dock.StingerUseCurrent"));
	fillTransitionCombo(takeOutCombo, stingers, takeOut, trKey("Dock.StingerUseCurrent"));
	form->addRow(trKey("Dock.StingerIn"), takeInCombo);
	form->addRow(trKey("Dock.StingerOut"), takeOutCombo);

	auto *eventCombo = new QComboBox(&dialog);
	fillTransitionCombo(eventCombo, transitionNames(false, true), eventTransition, trKey("Dock.EventTransitionCut"));
	form->addRow(trKey("Dock.EventTransition"), eventCombo);

	auto *eventDuration = new QSpinBox(&dialog);
	eventDuration->setRange(50, 10000);
	eventDuration->setSuffix(QStringLiteral(" ms"));
	eventDuration->setValue((int)sr_config_get_event_transition_duration_ms());
	form->addRow(trKey("Dock.EventTransitionMilliseconds"), eventDuration);

	auto *matchSpeed = new QCheckBox(trKey("Dock.EventTransitionMatchReplaySpeed"), &dialog);
	matchSpeed->setChecked(sr_config_get_event_transition_match_replay_speed());
	form->addRow(QString(), matchSpeed);

	auto *speedPolicy = new QComboBox(&dialog);
	speedPolicy->addItem(trKey("Dock.ReplaySpeedPolicy.Global"), SR_REPLAY_SPEED_GLOBAL);
	speedPolicy->addItem(trKey("Dock.ReplaySpeedPolicy.Event"), SR_REPLAY_SPEED_EVENT);
	int speedIndex = speedPolicy->findData((int)sr_config_get_replay_speed_policy());
	speedPolicy->setCurrentIndex(speedIndex >= 0 ? speedIndex : 0);
	form->addRow(trKey("Dock.ReplaySpeedPolicy"), speedPolicy);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	root->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, sessionEdit]() {
		const QString selected = QFileDialog::getExistingDirectory(&dialog, trKey("Dock.PickSessionFolder"),
									    sessionEdit->text());
		if (!selected.isEmpty())
			sessionEdit->setText(selected);
	});

	if (dialog.exec() != QDialog::Accepted)
		return;

	const QByteArray sessionPath = sessionEdit->text().toUtf8();
	sr_config_set_session_root(sessionPath.constData());
	sr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));
	sr_config_set_segment_duration_ms((uint32_t)(segment->value() * 1000.0));

	const QByteArray takeInName = takeInCombo->currentData().toString().toUtf8();
	const QByteArray takeOutName = takeOutCombo->currentData().toString().toUtf8();
	const QByteArray eventName = eventCombo->currentData().toString().toUtf8();
	sr_config_set_take_in_transition(takeInName.constData());
	sr_config_set_take_out_transition(takeOutName.constData());
	sr_config_set_event_transition(eventName.constData());
	sr_config_set_event_transition_duration_ms((uint32_t)eventDuration->value());
	sr_config_set_event_transition_match_replay_speed(matchSpeed->isChecked());
	sr_config_set_replay_speed_policy((enum sr_replay_speed_policy)speedPolicy->currentData().toInt());
}

} // namespace

extern "C" void sr_dock_register_impl(struct sr_event_controller *controller)
{
	auto *tabs = new QTabWidget();
	tabs->setObjectName(QStringLiteral("PitelInstantReplayDock"));
	tabs->setDocumentMode(true);

	auto *operatorScroll = new QScrollArea(tabs);
	operatorScroll->setObjectName(QStringLiteral("PitelInstantReplayOperatorScroll"));
	operatorScroll->setWidgetResizable(true);
	operatorScroll->setFrameShape(QFrame::NoFrame);
	operatorScroll->setWidget(sr_event_dock_create(controller, operatorScroll));

	/* sr-session-panel.cpp replaces this placeholder immediately after this
	 * function returns. Keeping the shell tiny avoids coupling Session Manager
	 * to the OBS dock registration code. */
	auto *sessionPlaceholder = new QWidget(tabs);
	tabs->addTab(operatorScroll, trKey("Dock.TabOperator"));
	tabs->addTab(sessionPlaceholder, trKey("Dock.TabStorage"));
	tabs->setCurrentIndex(0);

	if (!obs_frontend_add_dock_by_id("pitel_instant_replay_dock", obs_module_text("Dock.Title"), tabs)) {
		delete tabs;
		return;
	}

	QWidget *multiview = sr_multiview_dock_create(controller);
	if (!obs_frontend_add_dock_by_id("pitel_instant_replay_multiview_dock", obs_module_text("Multiview.Title"),
					 multiview))
		delete multiview;
}

extern "C" void sr_dock_open_settings(void)
{
	openSettingsDialog();
}
