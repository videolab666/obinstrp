from pathlib import Path
import re


def read(path):
    return Path(path).read_text(encoding="utf-8")


def write(path, text):
    Path(path).write_text(text, encoding="utf-8")


def replace(text, old, new, label):
    if old not in text:
        raise SystemExit(f"missing anchor: {label}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------------------
# Persistent configuration: Event Transition name + duration.
# ---------------------------------------------------------------------------
p = "src/sr-config.h"
s = read(p)
s = replace(s, "#define SR_CONFIG_SCHEMA_VERSION 4", "#define SR_CONFIG_SCHEMA_VERSION 5", "config schema")
s = replace(
    s,
    "char *sr_config_get_take_out_transition(void);\nvoid sr_config_set_take_out_transition(const char *transition_name);\n",
    "char *sr_config_get_take_out_transition(void);\nvoid sr_config_set_take_out_transition(const char *transition_name);\n\n"
    "/* Optional non-Stinger OBS transition used between Events/angles while\n"
    " * replay is already on Program. Empty means a direct same-bus cut. */\n"
    "char *sr_config_get_event_transition(void);\n"
    "void sr_config_set_event_transition(const char *transition_name);\n"
    "uint32_t sr_config_get_event_transition_duration_ms(void);\n"
    "void sr_config_set_event_transition_duration_ms(uint32_t milliseconds);\n",
    "config event transition declarations",
)
write(p, s)

p = "src/sr-config.c"
s = read(p)
s = replace(
    s,
    "#define DEFAULT_SEGMENT_DURATION_MS 4000u\n",
    "#define DEFAULT_SEGMENT_DURATION_MS 4000u\n#define DEFAULT_EVENT_TRANSITION_DURATION_MS 200u\n",
    "event transition default",
)
s = replace(
    s,
    "static char *g_take_in_transition;\nstatic char *g_take_out_transition;\n",
    "static char *g_take_in_transition;\nstatic char *g_take_out_transition;\n"
    "static char *g_event_transition;\nstatic uint32_t g_event_transition_duration_ms;\n",
    "config globals",
)
s = replace(
    s,
    "\tobs_data_set_string(data, \"take_out_transition\", g_take_out_transition ? g_take_out_transition : \"\");\n",
    "\tobs_data_set_string(data, \"take_out_transition\", g_take_out_transition ? g_take_out_transition : \"\");\n"
    "\tobs_data_set_string(data, \"event_transition\", g_event_transition ? g_event_transition : \"\");\n"
    "\tobs_data_set_int(data, \"event_transition_duration_ms\", g_event_transition_duration_ms);\n",
    "save event transition",
)
s = replace(
    s,
    "\tconst char *take_in = data ? obs_data_get_string(data, \"take_in_transition\") : \"\";\n"
    "\tconst char *take_out = data ? obs_data_get_string(data, \"take_out_transition\") : \"\";\n"
    "\tg_take_in_transition = bstrdup(take_in ? take_in : \"\");\n"
    "\tg_take_out_transition = bstrdup(take_out ? take_out : \"\");\n",
    "\tconst char *take_in = data ? obs_data_get_string(data, \"take_in_transition\") : \"\";\n"
    "\tconst char *take_out = data ? obs_data_get_string(data, \"take_out_transition\") : \"\";\n"
    "\tconst char *event_transition = data ? obs_data_get_string(data, \"event_transition\") : \"\";\n"
    "\tconst int64_t event_transition_ms = data ? obs_data_get_int(data, \"event_transition_duration_ms\") : 0;\n"
    "\tg_take_in_transition = bstrdup(take_in ? take_in : \"\");\n"
    "\tg_take_out_transition = bstrdup(take_out ? take_out : \"\");\n"
    "\tg_event_transition = bstrdup(event_transition ? event_transition : \"\");\n"
    "\tg_event_transition_duration_ms = event_transition_ms >= 50 && event_transition_ms <= 10000\n"
    "\t\t\t\t\t ? (uint32_t)event_transition_ms\n"
    "\t\t\t\t\t : DEFAULT_EVENT_TRANSITION_DURATION_MS;\n",
    "load event transition",
)
s = replace(
    s,
    "\tbfree(g_take_in_transition);\n\tbfree(g_take_out_transition);\n"
    "\tg_save_dir = NULL;\n\tg_session_root = NULL;\n\tg_take_in_transition = NULL;\n\tg_take_out_transition = NULL;\n",
    "\tbfree(g_take_in_transition);\n\tbfree(g_take_out_transition);\n\tbfree(g_event_transition);\n"
    "\tg_save_dir = NULL;\n\tg_session_root = NULL;\n\tg_take_in_transition = NULL;\n\tg_take_out_transition = NULL;\n\tg_event_transition = NULL;\n",
    "free event transition",
)
s += """

char *sr_config_get_event_transition(void)
{
	pthread_mutex_lock(&g_mutex);
	char *result = get_transition_name(g_event_transition);
	pthread_mutex_unlock(&g_mutex);
	return result;
}

void sr_config_set_event_transition(const char *transition_name)
{
	pthread_mutex_lock(&g_mutex);
	bfree(g_event_transition);
	g_event_transition = bstrdup(transition_name ? transition_name : "");
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

uint32_t sr_config_get_event_transition_duration_ms(void)
{
	pthread_mutex_lock(&g_mutex);
	const uint32_t result = g_event_transition_duration_ms;
	pthread_mutex_unlock(&g_mutex);
	return result;
}

void sr_config_set_event_transition_duration_ms(uint32_t milliseconds)
{
	if (milliseconds < 50)
		milliseconds = 50;
	if (milliseconds > 10000)
		milliseconds = 10000;
	pthread_mutex_lock(&g_mutex);
	g_event_transition_duration_ms = milliseconds;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}
"""
write(p, s)

# ---------------------------------------------------------------------------
# Scene tracker: one-shot arbitrary OBS transition + temporary duration.
# Stinger IN/OUT keeps its existing native-Stinger-only behavior.
# ---------------------------------------------------------------------------
p = "src/sr-scene-tracker.h"
s = read(p)
s = replace(s, "#include <stdbool.h>\n", "#include <stdbool.h>\n#include <stdint.h>\n", "scene tracker uint32")
s = replace(
    s,
    "void sr_switch_to_scene_with_transition(const char *scene_name, const char *transition_name);\n",
    "void sr_switch_to_scene_with_transition(const char *scene_name, const char *transition_name);\n\n"
    "/* One-shot arbitrary existing OBS transition with a temporary duration.\n"
    " * Used only for replay Event/angle A<->B changes; it does not require a\n"
    " * Stinger source and restores the operator's previous transition/duration. */\n"
    "void sr_switch_to_scene_with_transition_duration(const char *scene_name, const char *transition_name,\n"
    "\t\t\t\t\t     uint32_t duration_ms);\n",
    "scene tracker generic transition declaration",
)
write(p, s)

p = "src/sr-scene-tracker.c"
s = read(p)
s = replace(
    s,
    "static obs_source_t *g_transition_restore;  /* ref held while a one-shot override is active */\n"
    "static obs_source_t *g_transition_override; /* ref held while a one-shot override is active */\n",
    "static obs_source_t *g_transition_restore;  /* ref held while a one-shot override is active */\n"
    "static obs_source_t *g_transition_override; /* ref held while a one-shot override is active */\n"
    "static int g_transition_duration_restore;\n"
    "static bool g_transition_duration_overridden;\n",
    "transition duration globals",
)
restore_pat = re.compile(r"static void restore_transition_override\(void\)\n\{.*?\n\}\n\nstatic void on_frontend_event", re.S)
restore_new = r'''static void restore_transition_override(void)
{
	obs_source_t *restore = NULL;
	obs_source_t *override = NULL;
	int restore_duration = 0;
	bool restore_duration_enabled = false;

	pthread_mutex_lock(&g_mutex);
	restore = g_transition_restore;
	override = g_transition_override;
	restore_duration = g_transition_duration_restore;
	restore_duration_enabled = g_transition_duration_overridden;
	g_transition_restore = NULL;
	g_transition_override = NULL;
	g_transition_duration_restore = 0;
	g_transition_duration_overridden = false;
	pthread_mutex_unlock(&g_mutex);

	if (!override) {
		obs_source_release(restore);
		return;
	}

	obs_source_t *current = obs_frontend_get_current_transition();
	if (current == override && restore && restore != override)
		obs_frontend_set_current_transition(restore);
	if (restore_duration_enabled)
		obs_frontend_set_transition_duration(restore_duration);
	obs_source_release(current);
	obs_source_release(override);
	obs_source_release(restore);
}

static void on_frontend_event'''
if not restore_pat.search(s):
    raise SystemExit("missing anchor: restore transition override")
s = restore_pat.sub(restore_new, s, count=1)

begin_pat = re.compile(r"static bool begin_transition_override\(const char \*transition_name\)\n\{.*?\n\}\n\nstruct sr_scene_switch_request", re.S)
begin_new = r'''static bool begin_transition_override(const char *transition_name, bool native_stinger_only, uint32_t duration_ms)
{
	if (!transition_name || !*transition_name)
		return false;

	obs_source_t *override = find_transition_by_name(transition_name, native_stinger_only);
	if (!override) {
		blog(LOG_WARNING,
		     native_stinger_only
			     ? "Pitel Instant Replay: native OBS Stinger '%s' was not found; using current transition"
			     : "Pitel Instant Replay: OBS Event Transition '%s' was not found; using current transition",
		     transition_name);
		return false;
	}

	obs_source_t *current = obs_frontend_get_current_transition();
	pthread_mutex_lock(&g_mutex);
	const bool already_pending = g_transition_override != NULL;
	if (!already_pending) {
		g_transition_restore = current;
		g_transition_override = override;
		if (duration_ms) {
			g_transition_duration_restore = obs_frontend_get_transition_duration();
			g_transition_duration_overridden = true;
		}
	}
	pthread_mutex_unlock(&g_mutex);

	if (already_pending) {
		blog(LOG_WARNING,
		     "Pitel Instant Replay: transition override already in flight; keeping current OBS transition");
		obs_source_release(current);
		obs_source_release(override);
		return false;
	}

	if (current != override)
		obs_frontend_set_current_transition(override);
	if (duration_ms)
		obs_frontend_set_transition_duration((int)duration_ms);
	return true;
}

struct sr_scene_switch_request'''
if not begin_pat.search(s):
    raise SystemExit("missing anchor: begin transition override")
s = begin_pat.sub(begin_new, s, count=1)
s = replace(
    s,
    "struct sr_scene_switch_request {\n\tchar *scene_name;\n\tchar *transition_name;\n\tbool returning;\n};\n",
    "struct sr_scene_switch_request {\n\tchar *scene_name;\n\tchar *transition_name;\n\tuint32_t transition_duration_ms;\n\tbool returning;\n\tbool native_stinger_only;\n};\n",
    "scene switch request fields",
)
s = replace(
    s,
    "\t\tbegin_transition_override(request->transition_name);\n",
    "\t\tbegin_transition_override(request->transition_name, request->native_stinger_only,\n"
    "\t\t\t\t\t request->transition_duration_ms);\n",
    "begin override call",
)
s = replace(
    s,
    "static void queue_scene_with_transition(const char *scene_name, const char *transition_name, bool returning)\n",
    "static void queue_scene_with_transition(const char *scene_name, const char *transition_name, bool returning,\n"
    "\t\t\t\t     bool native_stinger_only, uint32_t duration_ms)\n",
    "queue transition signature",
)
s = replace(
    s,
    "\trequest->transition_name = bstrdup(transition_name ? transition_name : \"\");\n\trequest->returning = returning;\n",
    "\trequest->transition_name = bstrdup(transition_name ? transition_name : \"\");\n"
    "\trequest->transition_duration_ms = duration_ms;\n"
    "\trequest->returning = returning;\n"
    "\trequest->native_stinger_only = native_stinger_only;\n",
    "queue transition request values",
)
s = replace(
    s,
    "\tqueue_scene_with_transition(scene_name, transition_name, false);\n}\n\nstatic void switch_scene_return_task",
    "\tqueue_scene_with_transition(scene_name, transition_name, false, true, 0);\n}\n\n"
    "void sr_switch_to_scene_with_transition_duration(const char *scene_name, const char *transition_name,\n"
    "\t\t\t\t\t     uint32_t duration_ms)\n"
    "{\n"
    "\tif (!transition_name || !*transition_name) {\n"
    "\t\tsr_switch_to_scene(scene_name);\n"
    "\t\treturn;\n"
    "\t}\n"
    "\tqueue_scene_with_transition(scene_name, transition_name, false, false, duration_ms);\n"
    "}\n\n"
    "static void switch_scene_return_task",
    "generic transition function",
)
s = replace(
    s,
    "\tqueue_scene_with_transition(scene_name, transition_name, true);\n",
    "\tqueue_scene_with_transition(scene_name, transition_name, true, true, 0);\n",
    "return stinger queue",
)
write(p, s)

# ---------------------------------------------------------------------------
# TAKE layer: verify A/B replay scenes and perform Event Transition on UI.
# ---------------------------------------------------------------------------
p = "src/sr-replay-take.c"
s = read(p)
s = replace(
    s,
    "static char *g_return_scene;\nstatic char *output_source_name(enum sr_replay_bus bus);\n",
    "static char *g_return_scene;\nstatic char *output_source_name(enum sr_replay_bus bus);\nstatic bool return_live(void);\n",
    "take forward return_live",
)
anchor = '''static char *output_scene_name(enum sr_replay_bus bus)
{
	char *source_name = output_source_name(bus);
	if (!source_name)
		return NULL;
	char *scene_name = sr_find_scene_with_source(source_name);
	bfree(source_name);
	return scene_name;
}
'''
insert = anchor + '''

bool sr_replay_take_event_transition_ready(void)
{
	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	const bool ready = scene_a && *scene_a && scene_b && *scene_b && strcmp(scene_a, scene_b) != 0;
	bfree(scene_a);
	bfree(scene_b);
	return ready;
}
'''
s = replace(s, anchor, insert, "event transition readiness")

marker = '''	if (!sr_event_controller_set_played(events, state.event_id, true))
		blog(LOG_WARNING, "Pitel Instant Replay: TAKE %c succeeded but Event %llu could not be marked played",
		     bus == SR_REPLAY_BUS_A ? 'A' : 'B', (unsigned long long)state.event_id);
	return true;
}

static bool return_live(void)
'''
addition = '''	if (!sr_event_controller_set_played(events, state.event_id, true))
		blog(LOG_WARNING, "Pitel Instant Replay: TAKE %c succeeded but Event %llu could not be marked played",
		     bus == SR_REPLAY_BUS_A ? 'A' : 'B', (unsigned long long)state.event_id);
	return true;
}

struct sr_event_advance_request {
	struct sr_event_controller *events;
	enum sr_replay_bus bus;
};

static void event_advance_task(void *param)
{
	struct sr_event_advance_request *request = param;
	if (!request)
		return;

	struct sr_replay_channel_state state = {0};
	char *target_scene = NULL;
	char *scene_a = NULL;
	char *scene_b = NULL;
	obs_source_t *current = NULL;
	bool in_replay = false;
	bool success = false;

	if (!sr_replay_channel_get_state(request->bus, &state) || !state.cued || !state.event_id)
		goto cleanup;

	target_scene = output_scene_name(request->bus);
	scene_a = output_scene_name(SR_REPLAY_BUS_A);
	scene_b = output_scene_name(SR_REPLAY_BUS_B);
	current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;
	in_replay = scene_name_matches(current_name, scene_a, scene_b);
	if (!target_scene || !*target_scene || !in_replay || (current_name && strcmp(current_name, target_scene) == 0))
		goto cleanup;

	if (!sr_replay_channel_play(request->bus))
		goto cleanup;

	/* Keep live-audio Duck/Mute active while the old replay source fades/slides
	 * away. Its later deactivation sees a different active bus and therefore
	 * cannot restore live audio underneath the new replay. */
	enum sr_live_audio_policy live_audio_policy;
	double live_duck_db;
	get_live_audio_settings(request->bus, &live_audio_policy, &live_duck_db);
	apply_live_audio(request->bus, live_audio_policy, live_duck_db);

	char *event_transition = sr_config_get_event_transition();
	const uint32_t duration_ms = sr_config_get_event_transition_duration_ms();
	if (event_transition && *event_transition)
		sr_switch_to_scene_with_transition_duration(target_scene, event_transition, duration_ms);
	else
		sr_switch_to_scene(target_scene);
	bfree(event_transition);

	if (!sr_event_controller_set_played(request->events, state.event_id, true))
		blog(LOG_WARNING, "Pitel Instant Replay: Event Transition succeeded but Event %llu was not marked played",
		     (unsigned long long)state.event_id);
	success = true;

cleanup:
	obs_source_release(current);
	bfree(target_scene);
	bfree(scene_a);
	bfree(scene_b);
	if (!success) {
		blog(LOG_WARNING,
		     "Pitel Instant Replay: A/B Event Transition to bus %c failed; stopping the sequence and returning live",
		     request->bus == SR_REPLAY_BUS_A ? 'A' : 'B');
		sr_replay_playlist_stop(request->bus);
		sr_replay_channel_stop(request->bus);
		if (in_replay)
			return_live();
	}
	bfree(request);
}

void sr_replay_take_advance_event_async(struct sr_event_controller *events, enum sr_replay_bus bus)
{
	if (!events || (bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B))
		return;
	struct sr_event_advance_request *request = bzalloc(sizeof(*request));
	request->events = events;
	request->bus = bus;
	obs_queue_task(OBS_TASK_UI, event_advance_task, request, false);
}

static bool return_live(void)
'''
s = replace(s, marker, addition, "take event advance implementation")
write(p, s)

# ---------------------------------------------------------------------------
# Settings UI: Event Transition combo + duration, using existing OBS sources.
# ---------------------------------------------------------------------------
p = "src/sr-dock.cpp"
s = read(p)
s = replace(s, "#include <QShowEvent>\n", "#include <QShowEvent>\n#include <QSpinBox>\n", "QSpinBox include")
anchor = '''void populateStingerCombo(QComboBox *combo, const QStringList &names, const QString &saved)
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
'''
insert = anchor + '''

QStringList nativeEventTransitions()
{
	QStringList names;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; i++) {
		obs_source_t *transition = transitions.sources.array[i];
		if (strcmp(obs_source_get_unversioned_id(transition), "obs_stinger_transition") == 0)
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
'''
s = replace(s, anchor, insert, "event transition combo helpers")
old = '''		lay->addWidget(new QLabel(T("Dock.StingerOut"), &dlg));
		auto *stingerOut = new QComboBox(&dlg);
		populateStingerCombo(stingerOut, stingers, takeOut);
		lay->addWidget(stingerOut);

		auto *stingerHint = new QLabel(T("Dock.StingerHint"), &dlg);
'''
new = '''		lay->addWidget(new QLabel(T("Dock.StingerOut"), &dlg));
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
		connect(eventTransitionCombo, &QComboBox::currentIndexChanged, &dlg, [eventTransitionCombo, eventTransitionMs](int) {
			eventTransitionMs->setEnabled(!eventTransitionCombo->currentData().toString().isEmpty());
		});
		auto *eventTransitionHint = new QLabel(T("Dock.EventTransitionHint"), &dlg);
		eventTransitionHint->setWordWrap(true);
		eventTransitionHint->setStyleSheet(QStringLiteral("color: gray;"));
		lay->addWidget(eventTransitionHint);

		auto *stingerHint = new QLabel(T("Dock.StingerHint"), &dlg);
'''
s = replace(s, old, new, "settings event transition controls")
s = replace(
    s,
    "\t\t\tsr_config_set_take_in_transition(stingerInName.constData());\n"
    "\t\t\tsr_config_set_take_out_transition(stingerOutName.constData());\n",
    "\t\t\tsr_config_set_take_in_transition(stingerInName.constData());\n"
    "\t\t\tsr_config_set_take_out_transition(stingerOutName.constData());\n"
    "\t\t\tconst QByteArray eventTransitionName = eventTransitionCombo->currentData().toString().toUtf8();\n"
    "\t\t\tsr_config_set_event_transition(eventTransitionName.constData());\n"
    "\t\t\tsr_config_set_event_transition_duration_ms((uint32_t)eventTransitionMs->value());\n",
    "save settings event transition",
)
write(p, s)

# ---------------------------------------------------------------------------
# Operator menu: Play Each Angle and transition-enabled list playback.
# ---------------------------------------------------------------------------
p = "src/sr-event-dock.cpp"
s = read(p)
s = replace(s, '#include "sr-capture.h"\n', '#include "sr-capture.h"\n#include "sr-config.h"\n', "event dock config include")
s = replace(
    s,
    "\t\tauto *playSelectedAction = playMenu->addAction(T(\"EventDock.PlaySelected\"));\n"
    "\t\tauto *playLastAction = playMenu->addAction(T(\"EventDock.PlayLast\"));\n",
    "\t\tauto *playSelectedAction = playMenu->addAction(T(\"EventDock.PlaySelected\"));\n"
    "\t\tauto *playEachAngleAction = playMenu->addAction(T(\"EventDock.PlayEachAngle\"));\n"
    "\t\tauto *playLastAction = playMenu->addAction(T(\"EventDock.PlayLast\"));\n",
    "Play Each Angle action",
)
s = replace(
    s,
    "\t\tconnect(playSelectedAction, &QAction::triggered, this, [this]() { playSelectedEvent(); });\n"
    "\t\tconnect(playLastAction, &QAction::triggered, this, [this]() { playLastEvent(); });\n",
    "\t\tconnect(playSelectedAction, &QAction::triggered, this, [this]() { playSelectedEvent(); });\n"
    "\t\tconnect(playEachAngleAction, &QAction::triggered, this, [this]() { playEachAngle(); });\n"
    "\t\tconnect(playLastAction, &QAction::triggered, this, [this]() { playLastEvent(); });\n",
    "Play Each Angle connection",
)
old_summary = '''	QString playlistSummary(enum sr_replay_bus bus) const
	{
		sr_replay_playlist_state state = {};
		if (!sr_replay_playlist_get_state(bus, &state) || !state.active)
			return QString();
		return T("EventDock.PlaylistState").arg(state.list_id).arg(state.position + 1).arg(state.count);
	}
'''
new_summary = '''	QString playlistSummary(enum sr_replay_bus bus) const
	{
		sr_replay_playlist_state state = {};
		if (!sr_replay_playlist_get_state(bus, &state) || !state.active)
			return QString();
		if (state.angle_sequence)
			return T("EventDock.AngleSequenceState").arg(state.event_id).arg(state.position + 1).arg(state.count);
		return T("EventDock.PlaylistState").arg(state.list_id).arg(state.position + 1).arg(state.count);
	}

	enum sr_replay_bus activePlaylistBus() const
	{
		enum sr_replay_bus program;
		sr_replay_playlist_state state = {};
		if (sr_replay_take_program_bus(&program) && sr_replay_playlist_get_state(program, &state) && state.active)
			return program;
		if (sr_replay_playlist_get_state(SR_REPLAY_BUS_A, &state) && state.active)
			return SR_REPLAY_BUS_A;
		if (sr_replay_playlist_get_state(SR_REPLAY_BUS_B, &state) && state.active)
			return SR_REPLAY_BUS_B;
		return transportBus();
	}

	bool eventTransitionCrossBus(bool *requested = nullptr) const
	{
		char *configured = sr_config_get_event_transition();
		const bool wanted = configured && *configured;
		bfree(configured);
		if (requested)
			*requested = wanted;
		return wanted && sr_replay_take_event_transition_ready();
	}
'''
s = replace(s, old_summary, new_summary, "playlist summary and cross-bus helpers")

marker = '''	void playSelectedEvent()
	{
		const enum sr_replay_bus bus = transportBus();
		if (!cueSelected(bus))
			return;
		takeBus(bus);
	}
'''
addition = marker + '''

	void playEachAngle()
	{
		const uint64_t eventId = selectedEventId();
		if (!controller || !eventId) {
			setStatus("EventDock.NoEventSelected");
			return;
		}
		bool transitionRequested = false;
		const bool crossBus = eventTransitionCrossBus(&transitionRequested);
		const enum sr_replay_bus bus = transportBus();
		if (!sr_replay_playlist_start_event_angles(bus, eventId, crossBus)) {
			setStatus("EventDock.AngleSequenceFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}
		sr_replay_playlist_state sequence = {};
		sr_replay_playlist_get_state(bus, &sequence);
		QString message = T("EventDock.AngleSequenceStarted").arg(eventId).arg(sequence.count);
		if (transitionRequested && !crossBus)
			message += QStringLiteral(" · ") + T("EventDock.EventTransitionFallback");
		status->setText(message);
		refreshTransportStatus();
	}
'''
s = replace(s, marker, addition, "play each angle method")

old_start = '''	void startPlaylist(enum sr_replay_bus bus)
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
'''
new_start = '''	void startPlaylist(enum sr_replay_bus bus)
	{
		const QString camera = selectedCamera();
		const QByteArray cameraUtf8 = camera.toUtf8();
		const char *preferred = camera.isEmpty() ? nullptr : cameraUtf8.constData();
		bool transitionRequested = false;
		const bool crossBus = eventTransitionCrossBus(&transitionRequested);
		if (!controller || !sr_replay_playlist_start_with_transitions(bus, currentList(), preferred, crossBus)) {
			setStatus("EventDock.PlaylistFailed");
			return;
		}
		if (!sr_replay_take_bus(controller, bus)) {
			sr_replay_playlist_stop(bus);
			sr_replay_channel_stop(bus);
			setStatus("EventDock.TakeFailed");
			return;
		}
		QString message = T("EventDock.PlaylistStarted")
					  .arg(currentList())
					  .arg(bus == SR_REPLAY_BUS_A ? QStringLiteral("A") : QStringLiteral("B"));
		if (transitionRequested && !crossBus)
			message += QStringLiteral(" · ") + T("EventDock.EventTransitionFallback");
		status->setText(message);
		refresh();
		refreshTransportStatus();
	}
'''
s = replace(s, old_start, new_start, "transition playlist start")
s = replace(
    s,
    "\t\tif (!sr_replay_playlist_next(transportBus())) {\n",
    "\t\tif (!sr_replay_playlist_next(activePlaylistBus())) {\n",
    "next active sequence",
)
s = replace(
    s,
    "\t\tsr_replay_playlist_stop(transportBus());\n\t\tsetStatus(\"EventDock.PlaylistStopped\");\n",
    "\t\tsr_replay_playlist_stop(activePlaylistBus());\n\t\tsetStatus(\"EventDock.PlaylistStopped\");\n",
    "stop active sequence",
)
write(p, s)

# ---------------------------------------------------------------------------
# Locales.
# ---------------------------------------------------------------------------
p = "data/locale/en-US.ini"
s = read(p)
s = replace(
    s,
    'Dock.StingerOut="Native OBS Stinger — RETURN OUT"\n',
    'Dock.StingerOut="Native OBS Stinger — RETURN OUT"\n'
    'Dock.EventTransition="Event Transition"\n'
    'Dock.EventTransitionCut="Cut (no transition)"\n'
    'Dock.EventTransitionMilliseconds="milliseconds"\n'
    'Dock.EventTransitionHint="Between Events/angles, Pitel Instant Replay can pre-cue the next item on the other A/B bus and use any existing non-Stinger OBS transition (Fade, Swipe, Slide, Luma Wipe, Fade to Color, or plugin transitions you added). It requires Event Output A and B in two different scenes. Cut keeps the current bus and switches instantly."\n',
    "English event transition locale",
)
s = replace(
    s,
    'EventDock.PlaySelected="Play Selected"\n',
    'EventDock.PlaySelected="Play Selected"\nEventDock.PlayEachAngle="Play Each Angle of Selected"\n',
    "English play each angle locale",
)
s = replace(
    s,
    'EventDock.PlaylistState="List %1 %2/%3"\n',
    'EventDock.PlaylistState="List %1 %2/%3"\n'
    'EventDock.AngleSequenceState="Event #%1 angles %2/%3"\n'
    'EventDock.AngleSequenceStarted="Event #%1: playing %2 angle(s) in sequence"\n'
    'EventDock.AngleSequenceFailed="No complete playable camera angle is available for this Event"\n'
    'EventDock.EventTransitionFallback="Event Transition disabled: configure separate Event Output A and B scenes"\n',
    "English angle sequence status locale",
)
write(p, s)

p = "data/locale/es-ES.ini"
s = read(p)
# Spanish locale may not have all newer keys in the same order; append safe unique keys.
append = '''
Dock.EventTransition="Transición entre eventos"
Dock.EventTransitionCut="Corte (sin transición)"
Dock.EventTransitionMilliseconds="milisegundos"
Dock.EventTransitionHint="Entre eventos o ángulos, Pitel Instant Replay puede preparar el siguiente elemento en el otro bus A/B y usar cualquier transición OBS existente que no sea Stinger. Requiere Event Output A y B en dos escenas diferentes. Corte mantiene el bus actual y cambia al instante."
EventDock.PlayEachAngle="Reproducir cada ángulo del seleccionado"
EventDock.AngleSequenceState="Evento #%1 ángulos %2/%3"
EventDock.AngleSequenceStarted="Evento #%1: reproduciendo %2 ángulo(s) en secuencia"
EventDock.AngleSequenceFailed="No hay ningún ángulo de cámara reproducible completo para este evento"
EventDock.EventTransitionFallback="Transición entre eventos desactivada: configurá escenas separadas para Event Output A y B"
'''
for key in ["Dock.EventTransition=", "EventDock.PlayEachAngle="]:
    if key in s:
        raise SystemExit(f"unexpected pre-existing locale key: {key}")
s += append
write(p, s)
