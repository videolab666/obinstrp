/*
 * Pitel Instant Replay - OBS scene/transition bridge
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include "sr-scene-tracker.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>

#define RETURN_TOKEN_LIFETIME_NS 500000000ULL
#define PREVIEW_RESTORE_TAIL_NS 5000000000ULL

struct scene_bridge_state {
	pthread_mutex_t lock;
	bool initialized;
	char *program_now;
	char *program_before;
	char *preview_wanted;
	char *return_destination;
	uint64_t return_deadline_ns;
	bool preview_protected;
	uint64_t preview_protection_deadline_ns;
	obs_source_t *saved_transition;
	obs_source_t *temporary_transition;
	int saved_transition_duration;
	bool restore_duration;
};

static struct scene_bridge_state g_scene;

static void replace_text(char **slot, const char *value)
{
	char *replacement = value ? bstrdup(value) : NULL;
	bfree(*slot);
	*slot = replacement;
}

static void clear_return_token_locked(void)
{
	replace_text(&g_scene.return_destination, NULL);
	g_scene.return_deadline_ns = 0;
}

static void snapshot_program(void)
{
	obs_source_t *program = obs_frontend_get_current_scene();
	if (!program)
		return;
	const char *name = obs_source_get_name(program);

	pthread_mutex_lock(&g_scene.lock);
	if (!g_scene.program_now || strcmp(g_scene.program_now, name) != 0) {
		bfree(g_scene.program_before);
		g_scene.program_before = g_scene.program_now;
		g_scene.program_now = bstrdup(name);
	}
	if (g_scene.return_destination && strcmp(g_scene.return_destination, name) != 0)
		clear_return_token_locked();
	pthread_mutex_unlock(&g_scene.lock);
	obs_source_release(program);
}

static void snapshot_preview(void)
{
	obs_source_t *preview = obs_frontend_get_current_preview_scene();
	pthread_mutex_lock(&g_scene.lock);
	replace_text(&g_scene.preview_wanted, preview ? obs_source_get_name(preview) : NULL);
	pthread_mutex_unlock(&g_scene.lock);
	if (preview)
		obs_source_release(preview);
}

static bool preview_protection_active_locked(uint64_t now)
{
	if (!g_scene.preview_protected)
		return false;
	if (g_scene.preview_protection_deadline_ns && now > g_scene.preview_protection_deadline_ns) {
		g_scene.preview_protected = false;
		g_scene.preview_protection_deadline_ns = 0;
		return false;
	}
	return true;
}

static void handle_preview_change(void)
{
	if (!obs_frontend_preview_program_mode_active())
		return;

	obs_source_t *preview = obs_frontend_get_current_preview_scene();
	if (!preview)
		return;
	const char *preview_name = obs_source_get_name(preview);
	char *restore_name = NULL;

	pthread_mutex_lock(&g_scene.lock);
	const bool obs_swap_detected = preview_protection_active_locked(os_gettime_ns()) && g_scene.program_now &&
				       strcmp(preview_name, g_scene.program_now) == 0 && g_scene.preview_wanted &&
				       strcmp(g_scene.preview_wanted, preview_name) != 0;
	if (obs_swap_detected)
		restore_name = bstrdup(g_scene.preview_wanted);
	else
		replace_text(&g_scene.preview_wanted, preview_name);
	pthread_mutex_unlock(&g_scene.lock);
	obs_source_release(preview);

	if (!restore_name)
		return;
	obs_source_t *wanted = obs_get_source_by_name(restore_name);
	if (wanted) {
		obs_frontend_set_current_preview_scene(wanted);
		obs_source_release(wanted);
	}
	bfree(restore_name);
}

static void finish_temporary_transition(void)
{
	obs_source_t *saved = NULL;
	obs_source_t *temporary = NULL;
	int saved_duration = 0;
	bool restore_duration = false;

	pthread_mutex_lock(&g_scene.lock);
	saved = g_scene.saved_transition;
	temporary = g_scene.temporary_transition;
	saved_duration = g_scene.saved_transition_duration;
	restore_duration = g_scene.restore_duration;
	g_scene.saved_transition = NULL;
	g_scene.temporary_transition = NULL;
	g_scene.saved_transition_duration = 0;
	g_scene.restore_duration = false;
	pthread_mutex_unlock(&g_scene.lock);

	if (!temporary) {
		if (saved)
			obs_source_release(saved);
		return;
	}

	obs_source_t *current = obs_frontend_get_current_transition();
	if (current == temporary && saved && saved != temporary)
		obs_frontend_set_current_transition(saved);
	if (restore_duration)
		obs_frontend_set_transition_duration(saved_duration);
	if (current)
		obs_source_release(current);
	obs_source_release(temporary);
	if (saved)
		obs_source_release(saved);
}

static void frontend_event(enum obs_frontend_event event, void *unused)
{
	UNUSED_PARAMETER(unused);
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		snapshot_program();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		handle_preview_change();
		break;
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		finish_temporary_transition();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		snapshot_preview();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		pthread_mutex_lock(&g_scene.lock);
		replace_text(&g_scene.preview_wanted, NULL);
		g_scene.preview_protected = false;
		g_scene.preview_protection_deadline_ns = 0;
		pthread_mutex_unlock(&g_scene.lock);
		break;
	default:
		break;
	}
}

void sr_scene_tracker_start(void)
{
	if (g_scene.initialized)
		return;
	memset(&g_scene, 0, sizeof(g_scene));
	pthread_mutex_init(&g_scene.lock, NULL);
	g_scene.initialized = true;
	obs_frontend_add_event_callback(frontend_event, NULL);
	snapshot_program();
	if (obs_frontend_preview_program_mode_active())
		snapshot_preview();
}

void sr_scene_tracker_stop(void)
{
	if (!g_scene.initialized)
		return;
	obs_frontend_remove_event_callback(frontend_event, NULL);
	finish_temporary_transition();

	pthread_mutex_lock(&g_scene.lock);
	bfree(g_scene.program_now);
	bfree(g_scene.program_before);
	bfree(g_scene.preview_wanted);
	bfree(g_scene.return_destination);
	g_scene.program_now = NULL;
	g_scene.program_before = NULL;
	g_scene.preview_wanted = NULL;
	g_scene.return_destination = NULL;
	pthread_mutex_unlock(&g_scene.lock);
	pthread_mutex_destroy(&g_scene.lock);
	memset(&g_scene, 0, sizeof(g_scene));
}

char *sr_scene_tracker_previous(void)
{
	if (!g_scene.initialized)
		return NULL;
	pthread_mutex_lock(&g_scene.lock);
	char *result = g_scene.program_before ? bstrdup(g_scene.program_before) : NULL;
	pthread_mutex_unlock(&g_scene.lock);
	return result;
}

struct scene_search {
	const char *source_name;
	char *scene_name;
};

static bool find_source_scene(void *opaque, obs_source_t *scene_source)
{
	struct scene_search *search = opaque;
	obs_scene_t *scene = obs_scene_from_source(scene_source);
	if (!scene || !obs_scene_find_source_recursive(scene, search->source_name))
		return true;
	search->scene_name = bstrdup(obs_source_get_name(scene_source));
	return false;
}

char *sr_find_scene_with_source(const char *source_name)
{
	if (!source_name || !*source_name)
		return NULL;
	struct scene_search search = {.source_name = source_name};
	obs_enum_scenes(find_source_scene, &search);
	return search.scene_name;
}

static obs_source_t *lookup_transition(const char *name, bool require_stinger)
{
	if (!name || !*name)
		return NULL;

	obs_source_t *result = NULL;
	struct obs_frontend_source_list list = {0};
	obs_frontend_get_transitions(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		obs_source_t *candidate = list.sources.array[i];
		if (strcmp(obs_source_get_name(candidate), name) != 0)
			continue;
		const char *id = obs_source_get_unversioned_id(candidate);
		if (require_stinger && (!id || strcmp(id, "obs_stinger_transition") != 0))
			continue;
		result = obs_source_get_ref(candidate);
		break;
	}
	obs_frontend_source_list_free(&list);
	return result;
}

static bool install_temporary_transition(const char *name, bool require_stinger, uint32_t duration_ms)
{
	obs_source_t *temporary = lookup_transition(name, require_stinger);
	if (!temporary) {
		blog(LOG_WARNING, "Pitel Instant Replay: transition '%s' was not found; current OBS transition will be used",
		     name ? name : "");
		return false;
	}
	obs_source_t *current = obs_frontend_get_current_transition();

	pthread_mutex_lock(&g_scene.lock);
	if (g_scene.temporary_transition) {
		pthread_mutex_unlock(&g_scene.lock);
		if (current)
			obs_source_release(current);
		obs_source_release(temporary);
		blog(LOG_WARNING, "Pitel Instant Replay: another temporary transition is still active");
		return false;
	}
	g_scene.saved_transition = current;
	g_scene.temporary_transition = temporary;
	if (duration_ms) {
		g_scene.saved_transition_duration = obs_frontend_get_transition_duration();
		g_scene.restore_duration = true;
	}
	pthread_mutex_unlock(&g_scene.lock);

	if (current != temporary)
		obs_frontend_set_current_transition(temporary);
	if (duration_ms)
		obs_frontend_set_transition_duration((int)duration_ms);
	return true;
}

struct switch_request {
	char *scene_name;
	char *transition_name;
	uint32_t duration_ms;
	bool is_return;
	bool stinger_only;
};

static void set_return_token(const char *destination)
{
	pthread_mutex_lock(&g_scene.lock);
	replace_text(&g_scene.return_destination, destination);
	g_scene.return_deadline_ns = os_gettime_ns() + RETURN_TOKEN_LIFETIME_NS;
	pthread_mutex_unlock(&g_scene.lock);
}

static void execute_switch(void *opaque)
{
	struct switch_request *request = opaque;
	if (!request)
		return;

	obs_source_t *scene = obs_get_source_by_name(request->scene_name);
	if (scene) {
		if (request->is_return)
			set_return_token(request->scene_name);
		else {
			if (obs_frontend_preview_program_mode_active())
				snapshot_preview();
			sr_scene_tracker_note_replay_launch();
		}

		if (request->transition_name && *request->transition_name)
			install_temporary_transition(request->transition_name, request->stinger_only, request->duration_ms);
		obs_frontend_set_current_scene(scene);
		obs_source_release(scene);
	}

	bfree(request->scene_name);
	bfree(request->transition_name);
	bfree(request);
}

static void queue_switch(const char *scene_name, const char *transition_name, uint32_t duration_ms, bool is_return,
			 bool stinger_only)
{
	if (!scene_name || !*scene_name)
		return;
	struct switch_request *request = bzalloc(sizeof(*request));
	request->scene_name = bstrdup(scene_name);
	request->transition_name = bstrdup(transition_name ? transition_name : "");
	request->duration_ms = duration_ms;
	request->is_return = is_return;
	request->stinger_only = stinger_only;
	obs_queue_task(OBS_TASK_UI, execute_switch, request, false);
}

void sr_switch_to_scene(const char *scene_name)
{
	queue_switch(scene_name, NULL, 0, false, false);
}

void sr_switch_to_scene_with_transition(const char *scene_name, const char *transition_name)
{
	queue_switch(scene_name, transition_name, 0, false, true);
}

void sr_switch_to_scene_with_transition_duration(const char *scene_name, const char *transition_name,
						 uint32_t duration_ms)
{
	queue_switch(scene_name, transition_name, duration_ms, false, false);
}

void sr_switch_to_scene_return(const char *scene_name)
{
	queue_switch(scene_name, NULL, 0, true, false);
}

void sr_switch_to_scene_return_with_transition(const char *scene_name, const char *transition_name)
{
	queue_switch(scene_name, transition_name, 0, true, true);
}

bool sr_scene_tracker_consume_returning(void)
{
	if (!g_scene.initialized)
		return false;
	pthread_mutex_lock(&g_scene.lock);
	const bool valid = g_scene.return_destination && os_gettime_ns() <= g_scene.return_deadline_ns;
	clear_return_token_locked();
	pthread_mutex_unlock(&g_scene.lock);
	return valid;
}

struct source_return_request {
	char *source_name;
};

static void return_to_source_task(void *opaque)
{
	struct source_return_request *request = opaque;
	char *destination = sr_find_scene_with_source(request->source_name);
	if (!destination) {
		blog(LOG_WARNING, "Pitel Instant Replay: source '%s' is not present in a scene; using previous Program scene",
		     request->source_name);
		destination = sr_scene_tracker_previous();
	}
	if (destination) {
		struct switch_request *switcher = bzalloc(sizeof(*switcher));
		switcher->scene_name = destination;
		switcher->transition_name = bstrdup("");
		switcher->is_return = true;
		execute_switch(switcher);
	}
	bfree(request->source_name);
	bfree(request);
}

void sr_switch_to_scene_of_source_return(const char *source_name)
{
	if (!source_name || !*source_name)
		return;
	struct source_return_request *request = bzalloc(sizeof(*request));
	request->source_name = bstrdup(source_name);
	obs_queue_task(OBS_TASK_UI, return_to_source_task, request, false);
}

void sr_scene_tracker_note_replay_launch(void)
{
	if (!g_scene.initialized)
		return;
	pthread_mutex_lock(&g_scene.lock);
	g_scene.preview_protected = true;
	g_scene.preview_protection_deadline_ns = 0;
	pthread_mutex_unlock(&g_scene.lock);
}

void sr_scene_tracker_end_replay_guard(void)
{
	if (!g_scene.initialized)
		return;
	pthread_mutex_lock(&g_scene.lock);
	if (g_scene.preview_protected && !g_scene.preview_protection_deadline_ns)
		g_scene.preview_protection_deadline_ns = os_gettime_ns() + PREVIEW_RESTORE_TAIL_NS;
	pthread_mutex_unlock(&g_scene.lock);
}
