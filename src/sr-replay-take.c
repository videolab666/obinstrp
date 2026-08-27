/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-take.h"

#include "sr-config.h"
#include "sr-event-controller.h"
#include "sr-event-output.h"
#include "sr-replay-playlist.h"
#include "sr-replay-setup.h"
#include "sr-scene-tracker.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/darray.h>

#include <math.h>
#include <string.h>

static char *g_return_scene;
static char *output_source_name(enum sr_replay_bus bus);
static bool return_live(void);

struct live_audio_snapshot {
	obs_source_t *source;
	float volume;
	bool muted;
};

static DARRAY(struct live_audio_snapshot) g_live_audio_sources;
static bool g_live_audio_active;
static enum sr_replay_bus g_live_audio_bus;

struct live_audio_capture_ctx {
	enum sr_live_audio_policy policy;
	float duck_factor;
};

static bool capture_live_audio_source(void *param, obs_source_t *source)
{
	struct live_audio_capture_ctx *ctx = param;
	if (!ctx || !source || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT ||
	    !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) ||
	    strcmp(obs_source_get_unversioned_id(source), SR_EVENT_OUTPUT_ID) == 0)
		return true;

	struct live_audio_snapshot snapshot = {
		.source = obs_source_get_ref(source),
		.volume = obs_source_get_volume(source),
		.muted = obs_source_muted(source),
	};
	if (!snapshot.source)
		return true;
	da_push_back(g_live_audio_sources, &snapshot);

	if (ctx->policy == SR_LIVE_AUDIO_MUTE)
		obs_source_set_muted(source, true);
	else if (ctx->policy == SR_LIVE_AUDIO_DUCK)
		obs_source_set_volume(source, snapshot.volume * ctx->duck_factor);
	return true;
}

static void restore_live_audio(void)
{
	for (size_t i = 0; i < g_live_audio_sources.num; i++) {
		struct live_audio_snapshot *snapshot = &g_live_audio_sources.array[i];
		obs_source_set_volume(snapshot->source, snapshot->volume);
		obs_source_set_muted(snapshot->source, snapshot->muted);
		obs_source_release(snapshot->source);
	}
	da_free(g_live_audio_sources);
	g_live_audio_active = false;
}

static void apply_live_audio(enum sr_replay_bus bus, enum sr_live_audio_policy policy, double duck_db)
{
	restore_live_audio();
	if (policy == SR_LIVE_AUDIO_KEEP)
		return;

	if (!isfinite(duck_db) || duck_db < -60.0 || duck_db > 0.0)
		duck_db = -12.0;
	struct live_audio_capture_ctx ctx = {
		.policy = policy,
		.duck_factor = (float)pow(10.0, duck_db / 20.0),
	};
	g_live_audio_bus = bus;
	g_live_audio_active = true;
	obs_enum_sources(capture_live_audio_source, &ctx);
}

static void get_live_audio_settings(enum sr_replay_bus bus, enum sr_live_audio_policy *policy, double *duck_db)
{
	*policy = SR_LIVE_AUDIO_KEEP;
	*duck_db = -12.0;
	char *source_name = output_source_name(bus);
	obs_source_t *source = source_name ? obs_get_source_by_name(source_name) : NULL;
	bfree(source_name);
	if (!source)
		return;

	obs_data_t *settings = obs_source_get_settings(source);
	const int configured_policy = (int)obs_data_get_int(settings, SR_EVENT_OUTPUT_SETTING_LIVE_AUDIO_POLICY);
	if (configured_policy >= SR_LIVE_AUDIO_KEEP && configured_policy <= SR_LIVE_AUDIO_MUTE)
		*policy = (enum sr_live_audio_policy)configured_policy;
	*duck_db = obs_data_get_double(settings, SR_EVENT_OUTPUT_SETTING_LIVE_DUCK_DB);
	obs_data_release(settings);
	obs_source_release(source);
}

static char *output_source_name(enum sr_replay_bus bus)
{
	return sr_replay_setup_find_output_source_name(bus);
}

static char *output_scene_name(enum sr_replay_bus bus)
{
	return sr_replay_setup_find_output_scene_name(bus);
}

bool sr_replay_take_event_transition_ready(void)
{
	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	const bool ready = scene_a && *scene_a && scene_b && *scene_b && strcmp(scene_a, scene_b) != 0;
	bfree(scene_a);
	bfree(scene_b);
	return ready;
}

static bool scene_name_matches(const char *current_name, const char *a, const char *b)
{
	return current_name && ((a && strcmp(current_name, a) == 0) || (b && strcmp(current_name, b) == 0));
}

bool sr_replay_take_bus(struct sr_event_controller *events, enum sr_replay_bus bus)
{
	if (!events || (bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B))
		return false;

	struct sr_replay_channel_state state;
	if (!sr_replay_channel_get_state(bus, &state) || !state.cued || !state.event_id)
		return false;

	char *scene_name = output_scene_name(bus);
	if (!scene_name) {
		blog(LOG_WARNING,
		     "Pitel Instant Replay: TAKE %c failed: no scene contains an Event Output configured for bus %c",
		     bus == SR_REPLAY_BUS_A ? 'A' : 'B', bus == SR_REPLAY_BUS_A ? 'A' : 'B');
		return false;
	}

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;
	const bool already_in_replay = scene_name_matches(current_name, scene_a, scene_b);

	if (!already_in_replay && current_name && *current_name) {
		bfree(g_return_scene);
		g_return_scene = bstrdup(current_name);
	}

	if (!sr_replay_channel_play(bus)) {
		obs_source_release(current);
		bfree(scene_a);
		bfree(scene_b);
		bfree(scene_name);
		return false;
	}

	enum sr_live_audio_policy live_audio_policy;
	double live_duck_db;
	get_live_audio_settings(bus, &live_audio_policy, &live_duck_db);
	apply_live_audio(bus, live_audio_policy, live_duck_db);

	char *take_in = already_in_replay ? NULL : sr_config_get_take_in_transition();
	if (take_in && *take_in)
		sr_switch_to_scene_with_transition(scene_name, take_in);
	else
		sr_switch_to_scene(scene_name);
	bfree(take_in);
	obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	bfree(scene_name);

	if (!sr_event_controller_set_played(events, state.event_id, true))
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
	uint32_t duration_ms = sr_config_get_event_transition_duration_ms();
	if (event_transition && *event_transition && sr_config_get_event_transition_match_replay_speed() &&
	    state.speed_percent > 0.0) {
		double scaled_ms = (double)duration_ms * 100.0 / state.speed_percent;
		if (scaled_ms < 50.0)
			scaled_ms = 50.0;
		if (scaled_ms > 10000.0)
			scaled_ms = 10000.0;
		duration_ms = (uint32_t)llround(scaled_ms);
	}
	if (event_transition && *event_transition)
		sr_switch_to_scene_with_transition_duration(target_scene, event_transition, duration_ms);
	else
		sr_switch_to_scene(target_scene);
	bfree(event_transition);

	if (!sr_event_controller_set_played(request->events, state.event_id, true))
		blog(LOG_WARNING,
		     "Pitel Instant Replay: Event Transition succeeded but Event %llu was not marked played",
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
{
	if (!g_return_scene || !*g_return_scene)
		return false;

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;
	const bool in_replay = scene_name_matches(current_name, scene_a, scene_b);
	obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	if (!in_replay) {
		bfree(g_return_scene);
		g_return_scene = NULL;
		return false;
	}

	char *target = bstrdup(g_return_scene);
	bfree(g_return_scene);
	g_return_scene = NULL;
	if (!target)
		return false;

	/* Do not stop the replay bus before the OUT stinger: the native OBS
	 * transition must still be able to mix the replay picture/audio. The Event
	 * Output deactivation stops the bus once it has actually left program. */
	sr_replay_playlist_stop(SR_REPLAY_BUS_A);
	sr_replay_playlist_stop(SR_REPLAY_BUS_B);
	sr_scene_tracker_end_replay_guard();

	char *take_out = sr_config_get_take_out_transition();
	if (take_out && *take_out)
		sr_switch_to_scene_return_with_transition(target, take_out);
	else
		sr_switch_to_scene_return(target);
	bfree(take_out);
	bfree(target);
	return true;
}

bool sr_replay_take_return(struct sr_event_controller *events)
{
	return events && return_live();
}

struct sr_auto_return_request {
	enum sr_replay_bus bus;
	uint64_t event_id;
};

static void auto_return_task(void *param)
{
	struct sr_auto_return_request *request = param;
	if (!request)
		return;

	struct sr_replay_channel_state state = {0};
	const bool same_finished_event = sr_replay_channel_get_state(request->bus, &state) && state.cued &&
					 state.event_id == request->event_id && !state.playing;

	char *replay_scene = same_finished_event ? output_scene_name(request->bus) : NULL;
	obs_source_t *current = replay_scene ? obs_frontend_get_current_scene() : NULL;
	const char *current_name = current ? obs_source_get_name(current) : NULL;
	const bool same_bus_on_program = current_name && strcmp(current_name, replay_scene) == 0;

	if (same_finished_event && same_bus_on_program) {
		if (!return_live())
			blog(LOG_WARNING,
			     "Pitel Instant Replay: Event %llu ended on bus %c, but automatic RETURN LIVE failed",
			     (unsigned long long)request->event_id, request->bus == SR_REPLAY_BUS_A ? 'A' : 'B');
	} else {
		blog(LOG_INFO, "Pitel Instant Replay: ignored stale automatic return for Event %llu on bus %c",
		     (unsigned long long)request->event_id, request->bus == SR_REPLAY_BUS_A ? 'A' : 'B');
	}

	obs_source_release(current);
	bfree(replay_scene);
	bfree(request);
}

void sr_replay_take_return_on_end(enum sr_replay_bus bus, uint64_t event_id)
{
	if ((bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B) || !event_id)
		return;

	struct sr_auto_return_request *request = bzalloc(sizeof(*request));
	request->bus = bus;
	request->event_id = event_id;
	obs_queue_task(OBS_TASK_UI, auto_return_task, request, false);
}

static void release_live_audio_task(void *param)
{
	const enum sr_replay_bus bus = (enum sr_replay_bus)(uintptr_t)param;
	if (g_live_audio_active && g_live_audio_bus == bus)
		restore_live_audio();
}

void sr_replay_take_release_live_audio(enum sr_replay_bus bus)
{
	if (bus != SR_REPLAY_BUS_A && bus != SR_REPLAY_BUS_B)
		return;
	obs_queue_task(OBS_TASK_UI, release_live_audio_task, (void *)(uintptr_t)bus, false);
}

void sr_replay_take_reset(void)
{
	restore_live_audio();
	bfree(g_return_scene);
	g_return_scene = NULL;
}

bool sr_replay_take_current_bus(enum sr_replay_bus *bus)
{
	if (!bus)
		return false;

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;

	bool found = false;
	if (current_name && scene_a && strcmp(current_name, scene_a) == 0) {
		*bus = SR_REPLAY_BUS_A;
		found = true;
	} else if (current_name && scene_b && strcmp(current_name, scene_b) == 0) {
		*bus = SR_REPLAY_BUS_B;
		found = true;
	} else {
		struct sr_replay_channel_state a = {0};
		struct sr_replay_channel_state b = {0};
		const bool have_a = sr_replay_channel_get_state(SR_REPLAY_BUS_A, &a) && a.cued;
		const bool have_b = sr_replay_channel_get_state(SR_REPLAY_BUS_B, &b) && b.cued;
		if (have_a) {
			*bus = SR_REPLAY_BUS_A;
			found = true;
		} else if (have_b) {
			*bus = SR_REPLAY_BUS_B;
			found = true;
		}
	}

	obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	return found;
}

bool sr_replay_take_program_bus(enum sr_replay_bus *bus)
{
	if (!bus)
		return false;

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);
	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;
	bool found = false;
	if (current_name && scene_a && strcmp(current_name, scene_a) == 0) {
		*bus = SR_REPLAY_BUS_A;
		found = true;
	} else if (current_name && scene_b && strcmp(current_name, scene_b) == 0) {
		*bus = SR_REPLAY_BUS_B;
		found = true;
	}

	obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	return found;
}

bool sr_replay_take_toggle(struct sr_event_controller *events)
{
	if (!events)
		return false;

	char *scene_a = output_scene_name(SR_REPLAY_BUS_A);
	char *scene_b = output_scene_name(SR_REPLAY_BUS_B);

	obs_source_t *current = obs_frontend_get_current_scene();
	const char *current_name = current ? obs_source_get_name(current) : NULL;

	enum sr_replay_bus target = SR_REPLAY_BUS_A;
	if (current_name && scene_a && strcmp(current_name, scene_a) == 0)
		target = SR_REPLAY_BUS_B;
	else if (current_name && scene_b && strcmp(current_name, scene_b) == 0)
		target = SR_REPLAY_BUS_A;
	else {
		struct sr_replay_channel_state a;
		struct sr_replay_channel_state b;
		const bool have_a = sr_replay_channel_get_state(SR_REPLAY_BUS_A, &a) && a.cued;
		const bool have_b = sr_replay_channel_get_state(SR_REPLAY_BUS_B, &b) && b.cued;
		if (!have_a && have_b)
			target = SR_REPLAY_BUS_B;
	}

	if (current)
		obs_source_release(current);
	bfree(scene_a);
	bfree(scene_b);
	return sr_replay_take_bus(events, target);
}
