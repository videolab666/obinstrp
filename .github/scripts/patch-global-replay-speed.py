from pathlib import Path
import re


def replace_once(text, old, new, label):
    if old not in text:
        raise RuntimeError(f"missing pattern: {label}")
    return text.replace(old, new, 1)


def append_locale(path, block):
    text = path.read_text(encoding="utf-8")
    first_key = block.strip().split("=", 1)[0]
    if first_key not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += "\n" + block.strip() + "\n"
        path.write_text(text, encoding="utf-8")

# ---- config API ----------------------------------------------------------
path = Path("src/sr-config.h")
text = path.read_text(encoding="utf-8")
text = replace_once(text, "#include <stdint.h>\n", "#include <stdbool.h>\n#include <stdint.h>\n", "config bool include")
text = replace_once(text, "#define SR_CONFIG_SCHEMA_VERSION 5", "#define SR_CONFIG_SCHEMA_VERSION 6", "config schema")
text = replace_once(
    text,
    "enum sr_storage_low_space_action {\n\tSR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED = 0,\n\tSR_STORAGE_LOW_SPACE_STOP_RECORDING = 1,\n\tSR_STORAGE_LOW_SPACE_WARN_ONLY = 2,\n};\n",
    "enum sr_storage_low_space_action {\n\tSR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED = 0,\n\tSR_STORAGE_LOW_SPACE_STOP_RECORDING = 1,\n\tSR_STORAGE_LOW_SPACE_WARN_ONLY = 2,\n};\n\nenum sr_replay_speed_policy {\n\tSR_REPLAY_SPEED_GLOBAL = 0,\n\tSR_REPLAY_SPEED_EVENT = 1,\n};\n",
    "speed policy enum",
)
text = replace_once(
    text,
    "uint32_t sr_config_get_event_transition_duration_ms(void);\nvoid sr_config_set_event_transition_duration_ms(uint32_t milliseconds);\n",
    "uint32_t sr_config_get_event_transition_duration_ms(void);\nvoid sr_config_set_event_transition_duration_ms(uint32_t milliseconds);\nbool sr_config_get_event_transition_match_replay_speed(void);\nvoid sr_config_set_event_transition_match_replay_speed(bool enabled);\n\n/* Replay transport speed policy. Global is the production default: the operator\n * speed controller drives every current/future replay bus. Event mode restores\n * each Event's saved speed whenever that Event is cued. */\nenum sr_replay_speed_policy sr_config_get_replay_speed_policy(void);\nvoid sr_config_set_replay_speed_policy(enum sr_replay_speed_policy policy);\n",
    "config speed APIs",
)
path.write_text(text, encoding="utf-8")

path = Path("src/sr-config.c")
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    "static char *g_event_transition;\nstatic uint32_t g_event_transition_duration_ms;\n",
    "static char *g_event_transition;\nstatic uint32_t g_event_transition_duration_ms;\nstatic bool g_event_transition_match_replay_speed;\nstatic enum sr_replay_speed_policy g_replay_speed_policy;\n",
    "config globals",
)
text = replace_once(
    text,
    "\tobs_data_set_string(data, \"event_transition\", g_event_transition ? g_event_transition : \"\");\n\tobs_data_set_int(data, \"event_transition_duration_ms\", g_event_transition_duration_ms);\n",
    "\tobs_data_set_string(data, \"event_transition\", g_event_transition ? g_event_transition : \"\");\n\tobs_data_set_int(data, \"event_transition_duration_ms\", g_event_transition_duration_ms);\n\tobs_data_set_bool(data, \"event_transition_match_replay_speed\", g_event_transition_match_replay_speed);\n\tobs_data_set_int(data, \"replay_speed_policy\", (long long)g_replay_speed_policy);\n",
    "config save speed",
)
text = replace_once(
    text,
    "\tconst char *event_transition = data ? obs_data_get_string(data, \"event_transition\") : \"\";\n\tconst int64_t event_transition_ms = data ? obs_data_get_int(data, \"event_transition_duration_ms\") : 0;\n",
    "\tconst char *event_transition = data ? obs_data_get_string(data, \"event_transition\") : \"\";\n\tconst int64_t event_transition_ms = data ? obs_data_get_int(data, \"event_transition_duration_ms\") : 0;\n\tconst int64_t replay_speed_policy = data ? obs_data_get_int(data, \"replay_speed_policy\") : 0;\n",
    "config load speed",
)
text = replace_once(
    text,
    "\tg_event_transition_duration_ms = event_transition_ms >= 50 && event_transition_ms <= 10000\n\t\t\t\t\t\t ? (uint32_t)event_transition_ms\n\t\t\t\t\t\t : DEFAULT_EVENT_TRANSITION_DURATION_MS;\n",
    "\tg_event_transition_duration_ms = event_transition_ms >= 50 && event_transition_ms <= 10000\n\t\t\t\t\t\t ? (uint32_t)event_transition_ms\n\t\t\t\t\t\t : DEFAULT_EVENT_TRANSITION_DURATION_MS;\n\tg_event_transition_match_replay_speed =\n\t\tdata ? obs_data_get_bool(data, \"event_transition_match_replay_speed\") : false;\n\tg_replay_speed_policy = replay_speed_policy == SR_REPLAY_SPEED_EVENT ? SR_REPLAY_SPEED_EVENT\n\t\t\t\t\t\t\t\t     : SR_REPLAY_SPEED_GLOBAL;\n",
    "config init speed values",
)
if "sr_config_get_replay_speed_policy" not in text:
    text += """

bool sr_config_get_event_transition_match_replay_speed(void)
{
	pthread_mutex_lock(&g_mutex);
	const bool value = g_event_transition_match_replay_speed;
	pthread_mutex_unlock(&g_mutex);
	return value;
}

void sr_config_set_event_transition_match_replay_speed(bool enabled)
{
	pthread_mutex_lock(&g_mutex);
	g_event_transition_match_replay_speed = enabled;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

enum sr_replay_speed_policy sr_config_get_replay_speed_policy(void)
{
	pthread_mutex_lock(&g_mutex);
	const enum sr_replay_speed_policy value = g_replay_speed_policy;
	pthread_mutex_unlock(&g_mutex);
	return value;
}

void sr_config_set_replay_speed_policy(enum sr_replay_speed_policy policy)
{
	if (policy != SR_REPLAY_SPEED_EVENT)
		policy = SR_REPLAY_SPEED_GLOBAL;
	pthread_mutex_lock(&g_mutex);
	g_replay_speed_policy = policy;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}
"""
path.write_text(text, encoding="utf-8")

# ---- process-wide operator speed ----------------------------------------
path = Path("src/sr-replay-channel.h")
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    "bool sr_replay_channel_set_speed(enum sr_replay_bus bus, double speed_percent);\n",
    "bool sr_replay_channel_set_speed(enum sr_replay_bus bus, double speed_percent);\n\n/* Process-wide operator speed controller. In Global policy it is applied to\n * both buses immediately and is inherited by every newly cued Event/angle. */\nbool sr_replay_channel_set_controller_speed(double speed_percent);\ndouble sr_replay_channel_get_controller_speed(void);\n",
    "channel controller APIs",
)
path.write_text(text, encoding="utf-8")

path = Path("src/sr-replay-channel.c")
text = path.read_text(encoding="utf-8")
text = replace_once(text, '#include "sr-disk-player.h"\n', '#include "sr-disk-player.h"\n#include "sr-config.h"\n', "channel config include")
text = replace_once(
    text,
    "struct sr_replay_channels {\n\tstruct sr_event_controller *events;\n\tstruct sr_replay_channel buses[SR_REPLAY_BUS_COUNT];\n};\n",
    "struct sr_replay_channels {\n\tstruct sr_event_controller *events;\n\tpthread_mutex_t controller_speed_mutex;\n\tdouble controller_speed_percent;\n\tstruct sr_replay_channel buses[SR_REPLAY_BUS_COUNT];\n};\n",
    "channel global speed fields",
)
text = replace_once(
    text,
    "\tstruct sr_replay_channels *channels = bzalloc(sizeof(*channels));\n\tchannels->events = events;\n\tfor (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++) {\n",
    "\tstruct sr_replay_channels *channels = bzalloc(sizeof(*channels));\n\tchannels->events = events;\n\tpthread_mutex_init(&channels->controller_speed_mutex, NULL);\n\tchannels->controller_speed_percent = 100.0;\n\tfor (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++) {\n",
    "channel speed init",
)
text = replace_once(
    text,
    "\t}\n\tbfree(channels);\n}\n\nbool sr_replay_channel_cue",
    "\t}\n\tpthread_mutex_destroy(&channels->controller_speed_mutex);\n\tbfree(channels);\n}\n\nbool sr_replay_channel_cue",
    "channel speed shutdown",
)
text = replace_once(
    text,
    "\tconst bool partial = coverage.coverage != SR_REPLAY_COVERAGE_FULL;\n\tconst double speed = event.speed_percent;\n",
    "\tconst bool partial = coverage.coverage != SR_REPLAY_COVERAGE_FULL;\n\tconst double speed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL\n\t\t\t\t     ? sr_replay_channel_get_controller_speed()\n\t\t\t\t     : event.speed_percent;\n",
    "cue speed policy",
)
needle = "bool sr_replay_channel_set_audio_mode(enum sr_replay_bus bus, enum sr_replay_audio_mode audio_mode)"
if needle not in text:
    raise RuntimeError("missing pattern: set audio anchor")
controller_impl = r'''bool sr_replay_channel_set_controller_speed(double speed_percent)
{
	struct sr_replay_channels *channels = g_channels;
	if (!channels || !isfinite(speed_percent))
		return false;
	if (speed_percent < 10.0)
		speed_percent = 10.0;
	if (speed_percent > 400.0)
		speed_percent = 400.0;

	pthread_mutex_lock(&channels->controller_speed_mutex);
	channels->controller_speed_percent = speed_percent;
	pthread_mutex_unlock(&channels->controller_speed_mutex);

	if (sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL) {
		sr_replay_channel_set_speed(SR_REPLAY_BUS_A, speed_percent);
		sr_replay_channel_set_speed(SR_REPLAY_BUS_B, speed_percent);
	}
	return true;
}

double sr_replay_channel_get_controller_speed(void)
{
	struct sr_replay_channels *channels = g_channels;
	if (!channels)
		return 100.0;
	pthread_mutex_lock(&channels->controller_speed_mutex);
	const double speed = channels->controller_speed_percent;
	pthread_mutex_unlock(&channels->controller_speed_mutex);
	return speed;
}

'''
if "sr_replay_channel_set_controller_speed" not in text:
    text = text.replace(needle, controller_impl + needle, 1)
path.write_text(text, encoding="utf-8")

# ---- operator UI and aggregate timeline ---------------------------------
path = Path("src/sr-event-dock.cpp")
text = path.read_text(encoding="utf-8")
text = replace_once(
    text,
    "\t\tspeedCombo->setCurrentIndex(speedCombo->findData(100));\n\t\tcueBar->addWidget(speedCombo);\n",
    "\t\tspeedCombo->setCurrentIndex(speedCombo->findData(100));\n\t\tspeedCombo->setToolTip(T(\"EventDock.Speed.Tooltip\"));\n\t\tcueBar->addWidget(speedCombo);\n",
    "speed tooltip",
)
# Change both speed-control call sites (combo and shuttle) to the operator policy helper.
text = text.replace("sr_replay_channel_set_speed(transportBus(), speedCombo->itemData(index).toDouble());",
                    "setOperatorSpeed(speedCombo->itemData(index).toDouble());")
text = text.replace("sr_replay_channel_set_speed(transportBus(), speed);", "setOperatorSpeed(speed);")

# Insert helper before syncTransportControls.
anchor = "\tvoid syncTransportControls()\n\t{\n"
helper = r'''	void setOperatorSpeed(double speed)
	{
		if (!sr_replay_channel_set_controller_speed(speed))
			return;
		if (sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_EVENT)
			sr_replay_channel_set_speed(transportBus(), speed);
		syncTimeline();
	}

'''
if helper.strip() not in text:
    text = replace_once(text, anchor, helper + anchor, "operator speed helper")

old_sync = "\t\tconst int speedIndex = speedCombo->findData((int)state.speed_percent);\n\t\tif (speedIndex >= 0)\n\t\t\tspeedCombo->setCurrentIndex(speedIndex);\n"
new_sync = "\t\tconst double displayedSpeed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL\n\t\t\t\t\t      ? sr_replay_channel_get_controller_speed()\n\t\t\t\t\t      : state.speed_percent;\n\t\tconst int speedIndex = speedCombo->findData((int)displayedSpeed);\n\t\tif (speedIndex >= 0) {\n\t\t\tconst QSignalBlocker blocker(speedCombo);\n\t\t\tspeedCombo->setCurrentIndex(speedIndex);\n\t\t}\n"
text = replace_once(text, old_sync, new_sync, "sync displayed speed")

old_runtime = "\t\t\tif (sr_event_controller_get_event(controller, eventIds[i], &event)) {\n\t\t\t\tif (!event.pending && event.out_ns > event.in_ns)\n\t\t\t\t\truntime = playbackRuntimeNs(event.out_ns - event.in_ns, event.speed_percent);\n"
new_runtime = "\t\t\tif (sr_event_controller_get_event(controller, eventIds[i], &event)) {\n\t\t\t\tif (!event.pending && event.out_ns > event.in_ns) {\n\t\t\t\t\tconst double plannedSpeed = sr_config_get_replay_speed_policy() == SR_REPLAY_SPEED_GLOBAL\n\t\t\t\t\t\t\t\t? sr_replay_channel_get_controller_speed()\n\t\t\t\t\t\t\t\t: event.speed_percent;\n\t\t\t\t\truntime = playbackRuntimeNs(event.out_ns - event.in_ns, plannedSpeed);\n\t\t\t\t}\n"
text = replace_once(text, old_runtime, new_runtime, "timeline planned speed")
path.write_text(text, encoding="utf-8")

# ---- Settings UI ---------------------------------------------------------
path = Path("src/sr-dock.cpp")
text = path.read_text(encoding="utf-8")
text = replace_once(text, "#include <QComboBox>\n", "#include <QComboBox>\n#include <QCheckBox>\n", "dock checkbox include")
transition_anchor = "\t\tchar *eventTransitionRaw = sr_config_get_event_transition();\n"
speed_ui = r'''		auto *replaySpeedPolicyRow = new QHBoxLayout();
		replaySpeedPolicyRow->addWidget(new QLabel(T("Dock.ReplaySpeedPolicy"), &dlg));
		auto *replaySpeedPolicy = new QComboBox(&dlg);
		replaySpeedPolicy->addItem(T("Dock.ReplaySpeedPolicy.Global"), SR_REPLAY_SPEED_GLOBAL);
		replaySpeedPolicy->addItem(T("Dock.ReplaySpeedPolicy.Event"), SR_REPLAY_SPEED_EVENT);
		replaySpeedPolicy->setCurrentIndex(replaySpeedPolicy->findData((int)sr_config_get_replay_speed_policy()));
		replaySpeedPolicyRow->addWidget(replaySpeedPolicy, 1);
		lay->addLayout(replaySpeedPolicyRow);
		auto *replaySpeedPolicyHint = new QLabel(T("Dock.ReplaySpeedPolicyHint"), &dlg);
		replaySpeedPolicyHint->setWordWrap(true);
		replaySpeedPolicyHint->setStyleSheet(QStringLiteral("color: gray;"));
		lay->addWidget(replaySpeedPolicyHint);

'''
if "Dock.ReplaySpeedPolicy" not in text:
    text = replace_once(text, transition_anchor, speed_ui + transition_anchor, "speed policy settings UI")

old_transition_row = "\t\tlay->addLayout(eventTransitionRow);\n\t\teventTransitionMs->setEnabled(!eventTransitionCombo->currentData().toString().isEmpty());\n\t\tconnect(eventTransitionCombo, &QComboBox::currentIndexChanged, &dlg,\n\t\t\t[eventTransitionCombo, eventTransitionMs](int) {\n\t\t\t\teventTransitionMs->setEnabled(\n\t\t\t\t\t!eventTransitionCombo->currentData().toString().isEmpty());\n\t\t\t});\n"
new_transition_row = "\t\tlay->addLayout(eventTransitionRow);\n\t\tauto *eventTransitionMatchSpeed = new QCheckBox(T(\"Dock.EventTransitionMatchReplaySpeed\"), &dlg);\n\t\teventTransitionMatchSpeed->setChecked(sr_config_get_event_transition_match_replay_speed());\n\t\teventTransitionMatchSpeed->setToolTip(T(\"Dock.EventTransitionMatchReplaySpeedHint\"));\n\t\tlay->addWidget(eventTransitionMatchSpeed);\n\t\tconst bool haveEventTransition = !eventTransitionCombo->currentData().toString().isEmpty();\n\t\teventTransitionMs->setEnabled(haveEventTransition);\n\t\teventTransitionMatchSpeed->setEnabled(haveEventTransition);\n\t\tconnect(eventTransitionCombo, &QComboBox::currentIndexChanged, &dlg,\n\t\t\t[eventTransitionCombo, eventTransitionMs, eventTransitionMatchSpeed](int) {\n\t\t\t\tconst bool enabled = !eventTransitionCombo->currentData().toString().isEmpty();\n\t\t\t\teventTransitionMs->setEnabled(enabled);\n\t\t\t\teventTransitionMatchSpeed->setEnabled(enabled);\n\t\t\t});\n"
text = replace_once(text, old_transition_row, new_transition_row, "match replay transition UI")
old_save = "\t\t\tsr_config_set_event_transition(eventTransitionName.constData());\n\t\t\tsr_config_set_event_transition_duration_ms((uint32_t)eventTransitionMs->value());\n\n\t\t\twatchFolder();\n"
new_save = "\t\t\tsr_config_set_event_transition(eventTransitionName.constData());\n\t\t\tsr_config_set_event_transition_duration_ms((uint32_t)eventTransitionMs->value());\n\t\t\tsr_config_set_event_transition_match_replay_speed(eventTransitionMatchSpeed->isChecked());\n\t\t\tsr_config_set_replay_speed_policy(\n\t\t\t\tstatic_cast<sr_replay_speed_policy>(replaySpeedPolicy->currentData().toInt()));\n\n\t\t\twatchFolder();\n"
text = replace_once(text, old_save, new_save, "save speed settings")
path.write_text(text, encoding="utf-8")

# ---- Match Event Transition to replay speed ------------------------------
path = Path("src/sr-replay-take.c")
text = path.read_text(encoding="utf-8")
old = "\tchar *event_transition = sr_config_get_event_transition();\n\tconst uint32_t duration_ms = sr_config_get_event_transition_duration_ms();\n\tif (event_transition && *event_transition)\n\t\tsr_switch_to_scene_with_transition_duration(target_scene, event_transition, duration_ms);\n"
new = "\tchar *event_transition = sr_config_get_event_transition();\n\tuint32_t duration_ms = sr_config_get_event_transition_duration_ms();\n\tif (event_transition && *event_transition && sr_config_get_event_transition_match_replay_speed() &&\n\t    state.speed_percent > 0.0) {\n\t\tdouble scaled_ms = (double)duration_ms * 100.0 / state.speed_percent;\n\t\tif (scaled_ms < 50.0)\n\t\t\tscaled_ms = 50.0;\n\t\tif (scaled_ms > 10000.0)\n\t\t\tscaled_ms = 10000.0;\n\t\tduration_ms = (uint32_t)llround(scaled_ms);\n\t}\n\tif (event_transition && *event_transition)\n\t\tsr_switch_to_scene_with_transition_duration(target_scene, event_transition, duration_ms);\n"
text = replace_once(text, old, new, "match transition speed")
path.write_text(text, encoding="utf-8")

# ---- Locales -------------------------------------------------------------
append_locale(Path("data/locale/en-US.ini"), r'''
Dock.ReplaySpeedPolicy="Replay speed behavior"
Dock.ReplaySpeedPolicy.Global="Global controller (recommended)"
Dock.ReplaySpeedPolicy.Event="Stored Event speed"
Dock.ReplaySpeedPolicyHint="Global controller applies the operator Speed/Shuttle value to the current replay and all following Events/angles. Stored Event speed lets each Event restore its saved Speed when it starts."
Dock.EventTransitionMatchReplaySpeed="Match Event Transition to replay speed"
Dock.EventTransitionMatchReplaySpeedHint="vMix-style timing: a 200 ms Event Transition becomes 400 ms at 50% replay speed. Event Transitions overlap the start of the next Event, so transition duration is not added to sequence runtime."
EventDock.Speed.Tooltip="Main replay speed controller. Global mode applies it to current and following Events/angles; Stored Event mode changes the current replay but the next Event restores its saved Speed."
''')
append_locale(Path("data/locale/es-ES.ini"), r'''
Dock.ReplaySpeedPolicy="Comportamiento de velocidad de replay"
Dock.ReplaySpeedPolicy.Global="Control global (recomendado)"
Dock.ReplaySpeedPolicy.Event="Velocidad guardada del evento"
Dock.ReplaySpeedPolicyHint="El control global aplica Speed/Shuttle al replay actual y a todos los eventos/ángulos siguientes. Velocidad guardada hace que cada evento restaure su propia velocidad al comenzar."
Dock.EventTransitionMatchReplaySpeed="Adaptar transición de eventos a la velocidad de replay"
Dock.EventTransitionMatchReplaySpeedHint="Temporización estilo vMix: una transición de 200 ms pasa a 400 ms con replay al 50%. La transición se superpone al inicio del siguiente evento, por lo que no se suma a la duración de la secuencia."
EventDock.Speed.Tooltip="Control principal de velocidad. En modo Global se aplica al replay actual y a los eventos/ángulos siguientes; en modo Evento el siguiente evento restaura su velocidad guardada."
''')

print("global replay speed policy patch applied")
