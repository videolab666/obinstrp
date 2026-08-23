from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if new in text:
        return
    if old not in text:
        raise SystemExit(f"marker not found in {path}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# ---------------------------------------------------------------------------
# Persistent global replay stinger selection.
# ---------------------------------------------------------------------------
config_h = Path("src/sr-config.h")
replace_once(config_h, "#define SR_CONFIG_SCHEMA_VERSION 3\n", "#define SR_CONFIG_SCHEMA_VERSION 4\n")
replace_once(
    config_h,
    "uint32_t sr_config_get_segment_duration_ms(void);\nvoid sr_config_set_segment_duration_ms(uint32_t milliseconds);\n",
    "uint32_t sr_config_get_segment_duration_ms(void);\nvoid sr_config_set_segment_duration_ms(uint32_t milliseconds);\n\n/* Optional names of existing native OBS Stinger transitions used when the\n * operator enters replay from live and explicitly returns to live. An empty\n * string means keep the transition currently selected in OBS. Returned\n * strings are bstrdup allocations owned by the caller. */\nchar *sr_config_get_take_in_transition(void);\nvoid sr_config_set_take_in_transition(const char *transition_name);\nchar *sr_config_get_take_out_transition(void);\nvoid sr_config_set_take_out_transition(const char *transition_name);\n",
)

config_c = Path("src/sr-config.c")
replace_once(
    config_c,
    "static enum sr_storage_low_space_action g_low_space_action;\nstatic uint32_t g_segment_duration_ms;\n",
    "static enum sr_storage_low_space_action g_low_space_action;\nstatic uint32_t g_segment_duration_ms;\nstatic char *g_take_in_transition;\nstatic char *g_take_out_transition;\n",
)
replace_once(
    config_c,
    "\tobs_data_set_int(data, \"segment_duration_ms\", g_segment_duration_ms);\n",
    "\tobs_data_set_int(data, \"segment_duration_ms\", g_segment_duration_ms);\n\tobs_data_set_string(data, \"take_in_transition\", g_take_in_transition ? g_take_in_transition : \"\");\n\tobs_data_set_string(data, \"take_out_transition\", g_take_out_transition ? g_take_out_transition : \"\");\n",
)
replace_once(
    config_c,
    "\tg_segment_duration_ms = segment_ms >= 1000 && segment_ms <= 60000 ? (uint32_t)segment_ms\n\t\t\t\t\t\t\t\t\t  : DEFAULT_SEGMENT_DURATION_MS;\n\n\tos_mkdirs(g_save_dir);\n",
    "\tg_segment_duration_ms = segment_ms >= 1000 && segment_ms <= 60000 ? (uint32_t)segment_ms\n\t\t\t\t\t\t\t\t\t  : DEFAULT_SEGMENT_DURATION_MS;\n\n\tconst char *take_in = data ? obs_data_get_string(data, \"take_in_transition\") : \"\";\n\tconst char *take_out = data ? obs_data_get_string(data, \"take_out_transition\") : \"\";\n\tg_take_in_transition = bstrdup(take_in ? take_in : \"\");\n\tg_take_out_transition = bstrdup(take_out ? take_out : \"\");\n\n\tos_mkdirs(g_save_dir);\n",
)
replace_once(
    config_c,
    "\tbfree(g_session_root);\n\tg_save_dir = NULL;\n\tg_session_root = NULL;\n",
    "\tbfree(g_session_root);\n\tbfree(g_take_in_transition);\n\tbfree(g_take_out_transition);\n\tg_save_dir = NULL;\n\tg_session_root = NULL;\n\tg_take_in_transition = NULL;\n\tg_take_out_transition = NULL;\n",
)
replace_once(
    config_c,
    "void sr_config_set_segment_duration_ms(uint32_t milliseconds)\n{\n\tif (milliseconds < 1000)\n\t\tmilliseconds = 1000;\n\tif (milliseconds > 60000)\n\t\tmilliseconds = 60000;\n\n\tpthread_mutex_lock(&g_mutex);\n\tg_segment_duration_ms = milliseconds;\n\tsave_locked();\n\tpthread_mutex_unlock(&g_mutex);\n}\n",
    "void sr_config_set_segment_duration_ms(uint32_t milliseconds)\n{\n\tif (milliseconds < 1000)\n\t\tmilliseconds = 1000;\n\tif (milliseconds > 60000)\n\t\tmilliseconds = 60000;\n\n\tpthread_mutex_lock(&g_mutex);\n\tg_segment_duration_ms = milliseconds;\n\tsave_locked();\n\tpthread_mutex_unlock(&g_mutex);\n}\n\nstatic char *get_transition_name(char *value)\n{\n\treturn bstrdup(value ? value : \"\");\n}\n\nchar *sr_config_get_take_in_transition(void)\n{\n\tpthread_mutex_lock(&g_mutex);\n\tchar *result = get_transition_name(g_take_in_transition);\n\tpthread_mutex_unlock(&g_mutex);\n\treturn result;\n}\n\nvoid sr_config_set_take_in_transition(const char *transition_name)\n{\n\tpthread_mutex_lock(&g_mutex);\n\tbfree(g_take_in_transition);\n\tg_take_in_transition = bstrdup(transition_name ? transition_name : \"\");\n\tsave_locked();\n\tpthread_mutex_unlock(&g_mutex);\n}\n\nchar *sr_config_get_take_out_transition(void)\n{\n\tpthread_mutex_lock(&g_mutex);\n\tchar *result = get_transition_name(g_take_out_transition);\n\tpthread_mutex_unlock(&g_mutex);\n\treturn result;\n}\n\nvoid sr_config_set_take_out_transition(const char *transition_name)\n{\n\tpthread_mutex_lock(&g_mutex);\n\tbfree(g_take_out_transition);\n\tg_take_out_transition = bstrdup(transition_name ? transition_name : \"\");\n\tsave_locked();\n\tpthread_mutex_unlock(&g_mutex);\n}\n",
)

# ---------------------------------------------------------------------------
# Native OBS stinger selector in the existing Sports Replay settings dialog.
# ---------------------------------------------------------------------------
dock = Path("src/sr-dock.cpp")
replace_once(dock, "#include <QDoubleSpinBox>\n", "#include <QDoubleSpinBox>\n#include <QComboBox>\n")
replace_once(
    dock,
    "namespace {\n\nbool enum_replay_sources",
    "namespace {\n\nQStringList nativeStingerTransitions()\n{\n\tQStringList names;\n\tobs_frontend_source_list transitions = {};\n\tobs_frontend_get_transitions(&transitions);\n\tfor (size_t i = 0; i < transitions.sources.num; i++) {\n\t\tobs_source_t *transition = transitions.sources.array[i];\n\t\tif (strcmp(obs_source_get_unversioned_id(transition), \"obs_stinger_transition\") != 0)\n\t\t\tcontinue;\n\t\tconst QString name = QString::fromUtf8(obs_source_get_name(transition));\n\t\tif (!name.isEmpty() && !names.contains(name))\n\t\t\tnames.append(name);\n\t}\n\tobs_frontend_source_list_free(&transitions);\n\tnames.sort(Qt::CaseInsensitive);\n\treturn names;\n}\n\nvoid populateStingerCombo(QComboBox *combo, const QStringList &names, const QString &saved)\n{\n\tcombo->addItem(T(\"Dock.StingerUseCurrent\"), QString());\n\tfor (const QString &name : names)\n\t\tcombo->addItem(name, name);\n\n\tint index = combo->findData(saved);\n\tif (!saved.isEmpty() && index < 0) {\n\t\tcombo->addItem(T(\"Dock.StingerMissing\").arg(saved), saved);\n\t\tindex = combo->count() - 1;\n\t}\n\tcombo->setCurrentIndex(index >= 0 ? index : 0);\n}\n\nbool enum_replay_sources",
)
replace_once(
    dock,
    "\t\tsegmentSeconds->setValue((double)sr_config_get_segment_duration_ms() / 1000.0);\n\t\tlay->addWidget(new QLabel(T(\"Dock.SegmentDuration\"), &dlg));\n\t\tlay->addWidget(segmentSeconds);\n\n\t\tauto *freeSpace = new QLabel(&dlg);\n",
    "\t\tsegmentSeconds->setValue((double)sr_config_get_segment_duration_ms() / 1000.0);\n\t\tlay->addWidget(new QLabel(T(\"Dock.SegmentDuration\"), &dlg));\n\t\tlay->addWidget(segmentSeconds);\n\n\t\tchar *takeInRaw = sr_config_get_take_in_transition();\n\t\tchar *takeOutRaw = sr_config_get_take_out_transition();\n\t\tconst QString takeIn = QString::fromUtf8(takeInRaw ? takeInRaw : \"\");\n\t\tconst QString takeOut = QString::fromUtf8(takeOutRaw ? takeOutRaw : \"\");\n\t\tbfree(takeInRaw);\n\t\tbfree(takeOutRaw);\n\t\tconst QStringList stingers = nativeStingerTransitions();\n\n\t\tlay->addWidget(new QLabel(T(\"Dock.StingerIn\"), &dlg));\n\t\tauto *stingerIn = new QComboBox(&dlg);\n\t\tpopulateStingerCombo(stingerIn, stingers, takeIn);\n\t\tlay->addWidget(stingerIn);\n\n\t\tlay->addWidget(new QLabel(T(\"Dock.StingerOut\"), &dlg));\n\t\tauto *stingerOut = new QComboBox(&dlg);\n\t\tpopulateStingerCombo(stingerOut, stingers, takeOut);\n\t\tlay->addWidget(stingerOut);\n\n\t\tauto *stingerHint = new QLabel(T(\"Dock.StingerHint\"), &dlg);\n\t\tstingerHint->setWordWrap(true);\n\t\tstingerHint->setStyleSheet(QStringLiteral(\"color: gray;\"));\n\t\tlay->addWidget(stingerHint);\n\n\t\tauto *freeSpace = new QLabel(&dlg);\n",
)
replace_once(
    dock,
    "\t\t\tsr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));\n\t\t\tsr_config_set_segment_duration_ms((uint32_t)(segmentSeconds->value() * 1000.0));\n\n\t\t\twatchFolder();\n",
    "\t\t\tsr_config_set_min_free_bytes((uint64_t)(minFree->value() * gib));\n\t\t\tsr_config_set_segment_duration_ms((uint32_t)(segmentSeconds->value() * 1000.0));\n\n\t\t\tconst QByteArray stingerInName = stingerIn->currentData().toString().toUtf8();\n\t\t\tconst QByteArray stingerOutName = stingerOut->currentData().toString().toUtf8();\n\t\t\tsr_config_set_take_in_transition(stingerInName.constData());\n\t\t\tsr_config_set_take_out_transition(stingerOutName.constData());\n\n\t\t\twatchFolder();\n",
)

# ---------------------------------------------------------------------------
# Scene tracker: temporary native transition override, restored at transition
# stop without overwriting an operator transition change made mid-transition.
# ---------------------------------------------------------------------------
scene_h = Path("src/sr-scene-tracker.h")
replace_once(
    scene_h,
    "void sr_switch_to_scene(const char *scene_name);\n",
    "void sr_switch_to_scene(const char *scene_name);\n\n/* As above, but temporarily selects an existing native OBS Stinger by name\n * for this scene change. The operator's previous transition is restored when\n * OBS reports TRANSITION_STOPPED. Missing/renamed stingers fall back to the\n * transition currently selected in OBS. */\nvoid sr_switch_to_scene_with_transition(const char *scene_name, const char *transition_name);\n",
)
replace_once(
    scene_h,
    "void sr_switch_to_scene_return(const char *scene_name);\n",
    "void sr_switch_to_scene_return(const char *scene_name);\nvoid sr_switch_to_scene_return_with_transition(const char *scene_name, const char *transition_name);\n",
)

scene_c = Path("src/sr-scene-tracker.c")
replace_once(
    scene_c,
    "static bool g_preview_guard;          /* a replay is on air: keep it out of preview */\nstatic uint64_t g_preview_guard_ends; /* 0 = guard runs until the replay leaves program */\n",
    "static bool g_preview_guard;          /* a replay is on air: keep it out of preview */\nstatic uint64_t g_preview_guard_ends; /* 0 = guard runs until the replay leaves program */\nstatic obs_source_t *g_transition_restore;  /* ref held while a one-shot override is active */\nstatic obs_source_t *g_transition_override; /* ref held while a one-shot override is active */\n",
)
replace_once(
    scene_c,
    "static void on_frontend_event(enum obs_frontend_event event, void *data)\n",
    "static void restore_transition_override(void)\n{\n\tobs_source_t *restore = NULL;\n\tobs_source_t *override = NULL;\n\n\tpthread_mutex_lock(&g_mutex);\n\trestore = g_transition_restore;\n\toverride = g_transition_override;\n\tg_transition_restore = NULL;\n\tg_transition_override = NULL;\n\tpthread_mutex_unlock(&g_mutex);\n\n\tif (!override) {\n\t\tobs_source_release(restore);\n\t\treturn;\n\t}\n\n\tobs_source_t *current = obs_frontend_get_current_transition();\n\tif (current == override && restore)\n\t\tobs_frontend_set_current_transition(restore);\n\tobs_source_release(current);\n\tobs_source_release(override);\n\tobs_source_release(restore);\n}\n\nstatic void on_frontend_event(enum obs_frontend_event event, void *data)\n",
)
replace_once(
    scene_c,
    "\tcase OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:\n\t\ton_preview_scene_changed();\n\t\tbreak;\n",
    "\tcase OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:\n\t\ton_preview_scene_changed();\n\t\tbreak;\n\tcase OBS_FRONTEND_EVENT_TRANSITION_STOPPED:\n\t\trestore_transition_override();\n\t\tbreak;\n",
)
replace_once(
    scene_c,
    "\tg_preview_guard = false;\n\tg_preview_guard_ends = 0;\n\tclear_return_mark();\n",
    "\tg_preview_guard = false;\n\tg_preview_guard_ends = 0;\n\tobs_source_release(g_transition_restore);\n\tobs_source_release(g_transition_override);\n\tg_transition_restore = NULL;\n\tg_transition_override = NULL;\n\tclear_return_mark();\n",
)
replace_once(
    scene_c,
    "static void switch_scene_task(void *param)\n",
    "static obs_source_t *find_transition_by_name(const char *name, bool native_stinger_only)\n{\n\tif (!name || !*name)\n\t\treturn NULL;\n\n\tobs_source_t *result = NULL;\n\tstruct obs_frontend_source_list transitions = {0};\n\tobs_frontend_get_transitions(&transitions);\n\tfor (size_t i = 0; i < transitions.sources.num; i++) {\n\t\tobs_source_t *transition = transitions.sources.array[i];\n\t\tif (strcmp(obs_source_get_name(transition), name) != 0)\n\t\t\tcontinue;\n\t\tif (native_stinger_only &&\n\t\t    strcmp(obs_source_get_unversioned_id(transition), \"obs_stinger_transition\") != 0)\n\t\t\tcontinue;\n\t\tresult = obs_source_get_ref(transition);\n\t\tbreak;\n\t}\n\tobs_frontend_source_list_free(&transitions);\n\treturn result;\n}\n\nstatic bool begin_transition_override(const char *transition_name)\n{\n\tif (!transition_name || !*transition_name)\n\t\treturn false;\n\n\tobs_source_t *override = find_transition_by_name(transition_name, true);\n\tif (!override) {\n\t\tblog(LOG_WARNING, \"Sports Replay: native OBS Stinger '%s' was not found; using current transition\",\n\t\t     transition_name);\n\t\treturn false;\n\t}\n\n\tobs_source_t *current = obs_frontend_get_current_transition();\n\tif (current == override) {\n\t\tobs_source_release(current);\n\t\tobs_source_release(override);\n\t\treturn false;\n\t}\n\n\tpthread_mutex_lock(&g_mutex);\n\tconst bool already_pending = g_transition_override != NULL;\n\tif (!already_pending) {\n\t\tg_transition_restore = current;\n\t\tg_transition_override = override;\n\t}\n\tpthread_mutex_unlock(&g_mutex);\n\n\tif (already_pending) {\n\t\tblog(LOG_WARNING, \"Sports Replay: transition override already in flight; keeping current OBS transition\");\n\t\tobs_source_release(current);\n\t\tobs_source_release(override);\n\t\treturn false;\n\t}\n\n\tobs_frontend_set_current_transition(override);\n\treturn true;\n}\n\nstruct sr_scene_switch_request {\n\tchar *scene_name;\n\tchar *transition_name;\n\tbool returning;\n};\n\nstatic void switch_scene_task(void *param)\n",
)
replace_once(
    scene_c,
    "void sr_switch_to_scene(const char *scene_name)\n{\n\tif (!scene_name || !*scene_name)\n\t\treturn;\n\t/* scene switching must happen on the UI thread */\n\tobs_queue_task(OBS_TASK_UI, switch_scene_task, bstrdup(scene_name), false);\n}\n",
    "void sr_switch_to_scene(const char *scene_name)\n{\n\tif (!scene_name || !*scene_name)\n\t\treturn;\n\t/* scene switching must happen on the UI thread */\n\tobs_queue_task(OBS_TASK_UI, switch_scene_task, bstrdup(scene_name), false);\n}\n\nstatic void switch_scene_transition_task(void *param)\n{\n\tstruct sr_scene_switch_request *request = param;\n\tif (!request)\n\t\treturn;\n\n\tobs_source_t *scene = obs_get_source_by_name(request->scene_name);\n\tif (scene) {\n\t\tif (!request->returning) {\n\t\t\tnote_preview_scene();\n\t\t\tsr_scene_tracker_note_replay_launch();\n\t\t} else {\n\t\t\tpthread_mutex_lock(&g_mutex);\n\t\t\tbfree(g_return_target);\n\t\t\tg_return_target = bstrdup(request->scene_name);\n\t\t\tg_return_expires = os_gettime_ns() + SR_RETURN_WINDOW_NS;\n\t\t\tpthread_mutex_unlock(&g_mutex);\n\t\t}\n\n\t\tbegin_transition_override(request->transition_name);\n\t\tobs_frontend_set_current_scene(scene);\n\t\tobs_source_release(scene);\n\t}\n\n\tbfree(request->scene_name);\n\tbfree(request->transition_name);\n\tbfree(request);\n}\n\nstatic void queue_scene_with_transition(const char *scene_name, const char *transition_name, bool returning)\n{\n\tif (!scene_name || !*scene_name)\n\t\treturn;\n\n\tstruct sr_scene_switch_request *request = bzalloc(sizeof(*request));\n\trequest->scene_name = bstrdup(scene_name);\n\trequest->transition_name = bstrdup(transition_name ? transition_name : \"\");\n\trequest->returning = returning;\n\tobs_queue_task(OBS_TASK_UI, switch_scene_transition_task, request, false);\n}\n\nvoid sr_switch_to_scene_with_transition(const char *scene_name, const char *transition_name)\n{\n\tif (!transition_name || !*transition_name) {\n\t\tsr_switch_to_scene(scene_name);\n\t\treturn;\n\t}\n\tqueue_scene_with_transition(scene_name, transition_name, false);\n}\n",
)
replace_once(
    scene_c,
    "void sr_switch_to_scene_return(const char *scene_name)\n{\n\tif (!scene_name || !*scene_name)\n\t\treturn;\n\tobs_queue_task(OBS_TASK_UI, switch_scene_return_task, bstrdup(scene_name), false);\n}\n",
    "void sr_switch_to_scene_return(const char *scene_name)\n{\n\tif (!scene_name || !*scene_name)\n\t\treturn;\n\tobs_queue_task(OBS_TASK_UI, switch_scene_return_task, bstrdup(scene_name), false);\n}\n\nvoid sr_switch_to_scene_return_with_transition(const char *scene_name, const char *transition_name)\n{\n\tif (!transition_name || !*transition_name) {\n\t\tsr_switch_to_scene_return(scene_name);\n\t\treturn;\n\t}\n\tqueue_scene_with_transition(scene_name, transition_name, true);\n}\n",
)

# ---------------------------------------------------------------------------
# Replay TAKE controller: sponsor stinger only when crossing live<->replay;
# A<->B remains a normal operator transition. Track a stable live return scene.
# ---------------------------------------------------------------------------
take_h = Path("src/sr-replay-take.h")
replace_once(
    take_h,
    "bool sr_replay_take_toggle(struct sr_event_controller *events);\n",
    "bool sr_replay_take_toggle(struct sr_event_controller *events);\n\n/* Explicit operator return from either replay bus to the program scene that\n * was live before replay. Uses the configured OUT native Stinger when set. */\nbool sr_replay_take_return(struct sr_event_controller *events);\n\n/* Releases process-local TAKE state during module shutdown. */\nvoid sr_replay_take_reset(void);\n",
)

take_c = Path("src/sr-replay-take.c")
replace_once(take_c, "#include \"sr-event-controller.h\"\n", "#include \"sr-config.h\"\n#include \"sr-event-controller.h\"\n")
replace_once(
    take_c,
    "struct find_output_ctx {\n",
    "static char *g_return_scene;\n\nstruct find_output_ctx {\n",
)
replace_once(
    take_c,
    "bool sr_replay_take_bus(struct sr_event_controller *events, enum sr_replay_bus bus)\n{\n",
    "static bool scene_name_matches(const char *current_name, const char *a, const char *b)\n{\n\treturn current_name && ((a && strcmp(current_name, a) == 0) || (b && strcmp(current_name, b) == 0));\n}\n\nbool sr_replay_take_bus(struct sr_event_controller *events, enum sr_replay_bus bus)\n{\n",
)
replace_once(
    take_c,
    "\tif (!sr_replay_channel_play(bus)) {\n\t\tbfree(scene_name);\n\t\treturn false;\n\t}\n\n\t/* Arm the same Studio Mode preview guard used by the legacy replay path\n\t * before program moves to the Event Output scene. */\n\tsr_scene_tracker_note_replay_launch();\n\tsr_switch_to_scene(scene_name);\n\tbfree(scene_name);\n",
    "\tchar *scene_a = output_scene_name(SR_REPLAY_BUS_A);\n\tchar *scene_b = output_scene_name(SR_REPLAY_BUS_B);\n\tobs_source_t *current = obs_frontend_get_current_scene();\n\tconst char *current_name = current ? obs_source_get_name(current) : NULL;\n\tconst bool already_in_replay = scene_name_matches(current_name, scene_a, scene_b);\n\n\tif (!already_in_replay && current_name && *current_name) {\n\t\tbfree(g_return_scene);\n\t\tg_return_scene = bstrdup(current_name);\n\t}\n\n\tif (!sr_replay_channel_play(bus)) {\n\t\tobs_source_release(current);\n\t\tbfree(scene_a);\n\t\tbfree(scene_b);\n\t\tbfree(scene_name);\n\t\treturn false;\n\t}\n\n\tchar *take_in = already_in_replay ? NULL : sr_config_get_take_in_transition();\n\tif (take_in && *take_in)\n\t\tsr_switch_to_scene_with_transition(scene_name, take_in);\n\telse\n\t\tsr_switch_to_scene(scene_name);\n\tbfree(take_in);\n\tobs_source_release(current);\n\tbfree(scene_a);\n\tbfree(scene_b);\n\tbfree(scene_name);\n",
)
replace_once(
    take_c,
    "bool sr_replay_take_toggle(struct sr_event_controller *events)\n{\n",
    "bool sr_replay_take_return(struct sr_event_controller *events)\n{\n\tif (!events || !g_return_scene || !*g_return_scene)\n\t\treturn false;\n\n\tchar *scene_a = output_scene_name(SR_REPLAY_BUS_A);\n\tchar *scene_b = output_scene_name(SR_REPLAY_BUS_B);\n\tobs_source_t *current = obs_frontend_get_current_scene();\n\tconst char *current_name = current ? obs_source_get_name(current) : NULL;\n\tconst bool in_replay = scene_name_matches(current_name, scene_a, scene_b);\n\tobs_source_release(current);\n\tbfree(scene_a);\n\tbfree(scene_b);\n\tif (!in_replay) {\n\t\tbfree(g_return_scene);\n\t\tg_return_scene = NULL;\n\t\treturn false;\n\t}\n\n\tchar *target = bstrdup(g_return_scene);\n\tbfree(g_return_scene);\n\tg_return_scene = NULL;\n\tif (!target)\n\t\treturn false;\n\n\tsr_replay_channel_stop(SR_REPLAY_BUS_A);\n\tsr_replay_channel_stop(SR_REPLAY_BUS_B);\n\tsr_scene_tracker_end_replay_guard();\n\n\tchar *take_out = sr_config_get_take_out_transition();\n\tif (take_out && *take_out)\n\t\tsr_switch_to_scene_return_with_transition(target, take_out);\n\telse\n\t\tsr_switch_to_scene_return(target);\n\tbfree(take_out);\n\tbfree(target);\n\treturn true;\n}\n\nvoid sr_replay_take_reset(void)\n{\n\tbfree(g_return_scene);\n\tg_return_scene = NULL;\n}\n\nbool sr_replay_take_toggle(struct sr_event_controller *events)\n{\n",
)

# ---------------------------------------------------------------------------
# Event dock: explicit RETURN LIVE operator action.
# ---------------------------------------------------------------------------
event_dock = Path("src/sr-event-dock.cpp")
replace_once(
    event_dock,
    "\t\tauto *takeToggle = new QPushButton(T(\"EventDock.TakeToggle\"), this);\n\t\ttakeBar->addWidget(takeA);\n\t\ttakeBar->addWidget(takeB);\n\t\ttakeBar->addWidget(takeToggle);\n",
    "\t\tauto *takeToggle = new QPushButton(T(\"EventDock.TakeToggle\"), this);\n\t\tauto *returnLive = new QPushButton(T(\"EventDock.ReturnLive\"), this);\n\t\ttakeBar->addWidget(takeA);\n\t\ttakeBar->addWidget(takeB);\n\t\ttakeBar->addWidget(takeToggle);\n\t\ttakeBar->addWidget(returnLive);\n",
)
replace_once(
    event_dock,
    "\t\tconnect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });\n",
    "\t\tconnect(takeToggle, &QPushButton::clicked, this, [this]() { takeToggleBus(); });\n\t\tconnect(returnLive, &QPushButton::clicked, this, [this]() { returnLiveBus(); });\n",
)
replace_once(
    event_dock,
    "\tvoid togglePlayPause()\n",
    "\tvoid returnLiveBus()\n\t{\n\t\tif (!controller || !sr_replay_take_return(controller)) {\n\t\t\tsetStatus(\"EventDock.ReturnFailed\");\n\t\t\treturn;\n\t\t}\n\t\tsetStatus(\"EventDock.Returned\");\n\t\trefreshTransportStatus();\n\t}\n\n\tvoid togglePlayPause()\n",
)

# ---------------------------------------------------------------------------
# Frontend hotkey and shutdown cleanup.
# ---------------------------------------------------------------------------
plugin = Path("src/plugin-main.c")
replace_once(
    plugin,
    "static obs_hotkey_id hk_take_toggle = OBS_INVALID_HOTKEY_ID;\n",
    "static obs_hotkey_id hk_take_toggle = OBS_INVALID_HOTKEY_ID;\nstatic obs_hotkey_id hk_return_live = OBS_INVALID_HOTKEY_ID;\n",
)
replace_once(
    plugin,
    "static void register_event_hotkeys(void)\n",
    "static void return_live_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)\n{\n\tUNUSED_PARAMETER(data);\n\tUNUSED_PARAMETER(id);\n\tUNUSED_PARAMETER(hotkey);\n\tif (pressed && event_controller && !sr_replay_take_return(event_controller))\n\t\tobs_log(LOG_WARNING, \"Sports Replay: RETURN LIVE failed\");\n}\n\nstatic void register_event_hotkeys(void)\n",
)
replace_once(
    plugin,
    "\thk_take_toggle = obs_hotkey_register_frontend(\"SportsReplay.TakeToggle\", obs_module_text(\"Hotkey.TakeToggle\"),\n\t\t\t\t\t\t      take_toggle_cb, NULL);\n",
    "\thk_take_toggle = obs_hotkey_register_frontend(\"SportsReplay.TakeToggle\", obs_module_text(\"Hotkey.TakeToggle\"),\n\t\t\t\t\t\t      take_toggle_cb, NULL);\n\thk_return_live = obs_hotkey_register_frontend(\"SportsReplay.ReturnLive\", obs_module_text(\"Hotkey.ReturnLive\"),\n\t\t\t\t\t\t      return_live_cb, NULL);\n",
)
replace_once(
    plugin,
    "\tif (hk_take_toggle != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_take_toggle);\n",
    "\tif (hk_take_toggle != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_take_toggle);\n\tif (hk_return_live != OBS_INVALID_HOTKEY_ID)\n\t\tobs_hotkey_unregister(hk_return_live);\n",
)
replace_once(
    plugin,
    "\thk_take_toggle = OBS_INVALID_HOTKEY_ID;\n}\n",
    "\thk_take_toggle = OBS_INVALID_HOTKEY_ID;\n\thk_return_live = OBS_INVALID_HOTKEY_ID;\n}\n",
)
replace_once(
    plugin,
    "\tsr_storage_manager_stop();\n\tsr_replay_channels_shutdown();\n",
    "\tsr_storage_manager_stop();\n\tsr_replay_take_reset();\n\tsr_replay_channels_shutdown();\n",
)

# ---------------------------------------------------------------------------
# Locale strings.
# ---------------------------------------------------------------------------
locale = Path("data/locale/en-US.ini")
replace_once(
    locale,
    'Dock.SegmentDuration="Replay segment duration"\n',
    'Dock.SegmentDuration="Replay segment duration"\n'
    'Dock.StingerIn="Native OBS Stinger — TAKE IN"\n'
    'Dock.StingerOut="Native OBS Stinger — RETURN OUT"\n'
    'Dock.StingerUseCurrent="(use current OBS transition)"\n'
    'Dock.StingerMissing="%1 (missing)"\n'
    'Dock.StingerHint="Create and configure Stinger transitions in OBS itself (media, transition point, track matte and audio). Sports Replay only selects an existing native OBS Stinger for entering replay and returning live; A/B angle/bus changes do not replay the sponsor stinger."\n',
)
replace_once(
    locale,
    'EventDock.TakeToggle="TAKE A ↔ B"\n',
    'EventDock.TakeToggle="TAKE A ↔ B"\n'
    'EventDock.ReturnLive="RETURN LIVE"\n',
)
replace_once(
    locale,
    'EventDock.ToggleTaken="TAKE switched to the other replay bus"\n',
    'EventDock.ToggleTaken="TAKE switched to the other replay bus"\n'
    'EventDock.ReturnFailed="RETURN LIVE failed: program is not on a replay bus or the original live scene is unavailable"\n'
    'EventDock.Returned="Returned to live"\n',
)
replace_once(
    locale,
    'Hotkey.TakeToggle="Replay Event: TAKE A/B toggle"\n',
    'Hotkey.TakeToggle="Replay Event: TAKE A/B toggle"\n'
    'Hotkey.ReturnLive="Replay Event: RETURN LIVE"\n',
)
