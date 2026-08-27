/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-playlist.h"

#include "sr-camera-list.h"
#include "sr-camera-identity.h"
#include "sr-event-controller.h"
#include "sr-replay-coverage.h"
#include "sr-replay-take.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <pthread.h>
#include <string.h>

struct sr_playlist_bus {
	bool active;
	bool angle_sequence;
	bool cross_bus_transitions;
	unsigned list_id;
	uint64_t *event_ids;
	char **angle_cameras;
	size_t count;
	size_t position;
	uint64_t event_id;
	char *preferred_camera;
};

static pthread_mutex_t g_mutex;
static bool g_started;
static struct sr_event_controller *g_events;
static struct sr_playlist_bus g_buses[SR_REPLAY_BUS_COUNT];

static struct sr_playlist_bus *get_bus(enum sr_replay_bus bus)
{
	return bus >= SR_REPLAY_BUS_A && bus < SR_REPLAY_BUS_COUNT ? &g_buses[bus] : NULL;
}

static enum sr_replay_bus other_bus(enum sr_replay_bus bus)
{
	return bus == SR_REPLAY_BUS_A ? SR_REPLAY_BUS_B : SR_REPLAY_BUS_A;
}

/* A sequence item must always begin at Event IN. sr_replay_channel_cue() has
 * an intentional same-Event/different-camera fast path for live angle
 * switching that preserves the current playhead; that behavior is wrong for
 * sequential angle playback (and for a repeated Event two A/B hops later).
 * Clear the off-air transport before cueing, while preserving the operator's
 * per-bus replay-audio choice. */
static void reset_bus_for_sequence(enum sr_replay_bus bus)
{
	struct sr_replay_channel_state state = {0};
	const enum sr_replay_audio_mode audio_mode = sr_replay_channel_get_state(bus, &state) ? state.audio_mode
											      : SR_REPLAY_AUDIO_MASTER;
	sr_replay_channel_clear(bus);
	sr_replay_channel_set_audio_mode(bus, audio_mode);
}

static void clear_bus_locked(struct sr_playlist_bus *bus)
{
	if (!bus)
		return;
	if (bus->angle_cameras) {
		for (size_t i = 0; i < bus->count; i++)
			bfree(bus->angle_cameras[i]);
	}
	bfree(bus->angle_cameras);
	bfree(bus->event_ids);
	bfree(bus->preferred_camera);
	memset(bus, 0, sizeof(*bus));
}

static void clear_all_locked(void)
{
	for (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++)
		clear_bus_locked(&g_buses[i]);
}

static bool same_camera(const char *a, const char *b)
{
	return a && *a && b && *b && strcmp(a, b) == 0;
}

static bool try_camera(enum sr_replay_bus bus, uint64_t event_id, const struct sr_event_record *event,
		       const char *camera_name, enum sr_replay_coverage wanted)
{
	if (!camera_name || !*camera_name)
		return false;

	struct sr_replay_coverage_info coverage = {0};
	if (!sr_replay_coverage_query(camera_name, event->in_ns, event->out_ns, &coverage) ||
	    coverage.coverage != wanted)
		return false;
	return sr_replay_channel_cue(bus, event_id, camera_name);
}

static bool cue_best_camera_locked(enum sr_replay_bus bus, uint64_t event_id, const char *preferred_camera)
{
	struct sr_event_record event = {0};
	if (!sr_event_controller_get_event(g_events, event_id, &event))
		return false;
	if (event.pending) {
		sr_event_controller_free_event(&event);
		return false;
	}

	char *event_preferred = NULL;
	if (event.preferred_camera_id &&
	    !sr_event_controller_get_camera_name(g_events, event.preferred_camera_id, &event_preferred))
		event_preferred = NULL;

	struct sr_replay_channel_state current = {0};
	const bool have_current = sr_replay_channel_get_state(bus, &current) && current.cued && current.camera_name[0];
	const char *current_camera = have_current ? current.camera_name : NULL;

	struct sr_camera_list cameras = {0};
	if (!sr_camera_list_capture(&cameras)) {
		bfree(event_preferred);
		sr_event_controller_free_event(&event);
		return false;
	}

	bool cued = false;
	const enum sr_replay_coverage passes[] = {SR_REPLAY_COVERAGE_FULL, SR_REPLAY_COVERAGE_PARTIAL};
	for (size_t pass = 0; pass < sizeof(passes) / sizeof(passes[0]) && !cued; pass++) {
		const enum sr_replay_coverage wanted = passes[pass];
		if (event_preferred)
			cued = try_camera(bus, event_id, &event, event_preferred, wanted);
		if (!cued && current_camera && !same_camera(current_camera, event_preferred))
			cued = try_camera(bus, event_id, &event, current_camera, wanted);
		if (!cued && preferred_camera && *preferred_camera && !same_camera(preferred_camera, event_preferred) &&
		    !same_camera(preferred_camera, current_camera))
			cued = try_camera(bus, event_id, &event, preferred_camera, wanted);
		for (size_t i = 0; i < cameras.count && !cued; i++) {
			const char *candidate = cameras.names[i];
			if (same_camera(candidate, event_preferred) || same_camera(candidate, current_camera) ||
			    same_camera(candidate, preferred_camera))
				continue;
			cued = try_camera(bus, event_id, &event, candidate, wanted);
		}
	}

	sr_camera_list_free(&cameras);
	bfree(event_preferred);
	sr_event_controller_free_event(&event);
	return cued;
}

static bool cue_item_locked(enum sr_replay_bus bus, const struct sr_playlist_bus *playlist, size_t index)
{
	if (!playlist || index >= playlist->count || !playlist->event_ids)
		return false;
	if (playlist->angle_sequence) {
		const char *camera = playlist->angle_cameras ? playlist->angle_cameras[index] : NULL;
		return camera && *camera && sr_replay_channel_cue(bus, playlist->event_ids[index], camera);
	}
	return cue_best_camera_locked(bus, playlist->event_ids[index], playlist->preferred_camera);
}

bool sr_replay_playlist_init(struct sr_event_controller *events)
{
	if (g_started)
		return true;
	if (!events)
		return false;
	if (pthread_mutex_init(&g_mutex, NULL) != 0)
		return false;
	memset(g_buses, 0, sizeof(g_buses));
	g_events = events;
	g_started = true;
	return true;
}

void sr_replay_playlist_shutdown(void)
{
	if (!g_started)
		return;
	pthread_mutex_lock(&g_mutex);
	clear_all_locked();
	g_events = NULL;
	g_started = false;
	pthread_mutex_unlock(&g_mutex);
	pthread_mutex_destroy(&g_mutex);
}

bool sr_replay_playlist_start(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera)
{
	return sr_replay_playlist_start_with_transitions(bus, list_id, preferred_camera, false);
}

bool sr_replay_playlist_start_with_transitions(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera,
					       bool cross_bus_transitions)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist || !g_events)
		return false;

	uint64_t *event_ids = NULL;
	size_t count = 0;
	if (!sr_event_controller_get_list_events(g_events, list_id, &event_ids, &count) || !count) {
		bfree(event_ids);
		return false;
	}

	pthread_mutex_lock(&g_mutex);
	clear_all_locked();
	playlist = get_bus(bus);
	playlist->active = true;
	playlist->list_id = list_id;
	playlist->event_ids = event_ids;
	playlist->count = count;
	playlist->preferred_camera = bstrdup(preferred_camera ? preferred_camera : "");
	playlist->cross_bus_transitions = cross_bus_transitions;

	size_t first = 0;
	bool cued = false;
	for (; first < count; first++) {
		if (cue_item_locked(bus, playlist, first) && sr_replay_channel_play(bus)) {
			cued = true;
			break;
		}
	}

	if (!cued) {
		clear_bus_locked(playlist);
		pthread_mutex_unlock(&g_mutex);
		return false;
	}

	playlist->position = first;
	playlist->event_id = event_ids[first];
	const uint64_t first_event_id = playlist->event_id;
	pthread_mutex_unlock(&g_mutex);

	blog(LOG_INFO,
	     "Pitel Instant Replay: started Event List %u highlight reel on bus %c at item %zu/%zu (Event %llu)%s",
	     list_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', first + 1, count, (unsigned long long)first_event_id,
	     cross_bus_transitions ? " with A/B Event Transitions" : "");
	return true;
}

bool sr_replay_playlist_start_events_with_transitions(enum sr_replay_bus bus, unsigned list_id,
						      const uint64_t *event_ids, size_t count,
						      const char *preferred_camera, bool cross_bus_transitions)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist || !g_events || !event_ids || !count)
		return false;

	uint64_t *snapshot = bzalloc(count * sizeof(*snapshot));
	if (!snapshot)
		return false;
	memcpy(snapshot, event_ids, count * sizeof(*snapshot));

	pthread_mutex_lock(&g_mutex);
	clear_all_locked();
	playlist = get_bus(bus);
	playlist->active = true;
	playlist->list_id = list_id;
	playlist->event_ids = snapshot;
	playlist->count = count;
	playlist->preferred_camera = bstrdup(preferred_camera ? preferred_camera : "");
	playlist->cross_bus_transitions = cross_bus_transitions;

	size_t first = 0;
	bool cued = false;
	for (; first < count; first++) {
		if (cue_item_locked(bus, playlist, first) && sr_replay_channel_play(bus)) {
			cued = true;
			break;
		}
	}

	if (!cued) {
		clear_bus_locked(playlist);
		pthread_mutex_unlock(&g_mutex);
		return false;
	}

	playlist->position = first;
	playlist->event_id = snapshot[first];
	const uint64_t first_event_id = playlist->event_id;
	pthread_mutex_unlock(&g_mutex);

	blog(LOG_INFO,
	     "Pitel Instant Replay: started selected Event sequence from List %u on bus %c at item %zu/%zu (Event %llu)%s",
	     list_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', first + 1, count, (unsigned long long)first_event_id,
	     cross_bus_transitions ? " with A/B Event Transitions" : "");
	return true;
}

static bool collect_event_angles(uint64_t event_id, uint64_t **event_ids_out, char ***cameras_out, size_t *count_out)
{
	*event_ids_out = NULL;
	*cameras_out = NULL;
	*count_out = 0;

	struct sr_event_record event = {0};
	if (!sr_event_controller_get_event(g_events, event_id, &event) || event.pending) {
		if (event.id)
			sr_event_controller_free_event(&event);
		return false;
	}

	struct sr_camera_list cameras = {0};
	if (!sr_camera_list_capture(&cameras)) {
		sr_event_controller_free_event(&event);
		return false;
	}

	size_t full_count = 0;
	size_t partial_count = 0;
	for (size_t i = 0; i < cameras.count; i++) {
		if (sr_camera_is_program_name(cameras.names[i]))
			continue;
		struct sr_replay_coverage_info coverage = {0};
		if (!sr_replay_coverage_query(cameras.names[i], event.in_ns, event.out_ns, &coverage))
			continue;
		if (coverage.coverage == SR_REPLAY_COVERAGE_FULL)
			full_count++;
		else if (coverage.coverage == SR_REPLAY_COVERAGE_PARTIAL)
			partial_count++;
	}

	const enum sr_replay_coverage wanted = full_count ? SR_REPLAY_COVERAGE_FULL : SR_REPLAY_COVERAGE_PARTIAL;
	const size_t wanted_count = full_count ? full_count : partial_count;
	if (!wanted_count) {
		sr_camera_list_free(&cameras);
		sr_event_controller_free_event(&event);
		return false;
	}

	uint64_t *event_ids = bzalloc(wanted_count * sizeof(*event_ids));
	char **angle_cameras = bzalloc(wanted_count * sizeof(*angle_cameras));
	size_t actual = 0;
	for (size_t i = 0; i < cameras.count && actual < wanted_count; i++) {
		if (sr_camera_is_program_name(cameras.names[i]))
			continue;
		struct sr_replay_coverage_info coverage = {0};
		if (!sr_replay_coverage_query(cameras.names[i], event.in_ns, event.out_ns, &coverage) ||
		    coverage.coverage != wanted)
			continue;
		char *camera = bstrdup(cameras.names[i]);
		if (!camera)
			continue;
		event_ids[actual] = event_id;
		angle_cameras[actual] = camera;
		actual++;
	}

	sr_camera_list_free(&cameras);
	sr_event_controller_free_event(&event);
	if (!actual) {
		bfree(event_ids);
		bfree(angle_cameras);
		return false;
	}

	*event_ids_out = event_ids;
	*cameras_out = angle_cameras;
	*count_out = actual;
	return true;
}

bool sr_replay_playlist_start_event_angles(enum sr_replay_bus bus, uint64_t event_id, bool cross_bus_transitions)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist || !g_events || !event_id)
		return false;

	uint64_t *event_ids = NULL;
	char **angle_cameras = NULL;
	size_t count = 0;
	if (!collect_event_angles(event_id, &event_ids, &angle_cameras, &count))
		return false;

	pthread_mutex_lock(&g_mutex);
	clear_all_locked();
	playlist = get_bus(bus);
	playlist->active = true;
	playlist->angle_sequence = true;
	playlist->event_ids = event_ids;
	playlist->angle_cameras = angle_cameras;
	playlist->count = count;
	playlist->cross_bus_transitions = cross_bus_transitions;

	reset_bus_for_sequence(bus);
	size_t first = 0;
	bool cued = false;
	for (; first < count; first++) {
		if (cue_item_locked(bus, playlist, first) && sr_replay_channel_play(bus)) {
			cued = true;
			break;
		}
	}
	if (!cued) {
		clear_bus_locked(playlist);
		pthread_mutex_unlock(&g_mutex);
		return false;
	}

	playlist->position = first;
	playlist->event_id = event_id;
	const char *first_camera = playlist->angle_cameras[first];
	pthread_mutex_unlock(&g_mutex);

	blog(LOG_INFO, "Pitel Instant Replay: started Event %llu angle sequence on bus %c, angle %zu/%zu '%s'%s",
	     (unsigned long long)event_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', first + 1, count,
	     first_camera ? first_camera : "", cross_bus_transitions ? " with A/B Event Transitions" : "");
	return true;
}

static bool advance_locked(enum sr_replay_bus bus, struct sr_playlist_bus *playlist, bool *queue_take,
			   enum sr_replay_bus *take_bus)
{
	*queue_take = false;
	*take_bus = bus;
	if (!playlist->active || playlist->position >= playlist->count)
		return false;

	struct sr_replay_channel_state current = {0};
	if (!sr_replay_channel_get_state(bus, &current) || !current.cued || current.event_id != playlist->event_id) {
		clear_bus_locked(playlist);
		return false;
	}

	const bool cross_bus = playlist->cross_bus_transitions;
	const enum sr_replay_bus target_bus = cross_bus ? other_bus(bus) : bus;
	if (cross_bus || playlist->angle_sequence)
		reset_bus_for_sequence(target_bus);

	for (size_t next = playlist->position + 1; next < playlist->count; next++) {
		if (!cue_item_locked(target_bus, playlist, next))
			continue;

		struct sr_playlist_bus *active = playlist;
		if (cross_bus) {
			struct sr_playlist_bus moved = *playlist;
			memset(playlist, 0, sizeof(*playlist));
			struct sr_playlist_bus *target = get_bus(target_bus);
			clear_bus_locked(target);
			*target = moved;
			active = target;
			*queue_take = true;
			*take_bus = target_bus;
		} else if (!sr_replay_channel_play(target_bus)) {
			continue;
		}

		active->position = next;
		active->event_id = active->event_ids[next];
		if (!sr_event_controller_set_played(g_events, active->event_id, true))
			blog(LOG_WARNING, "Pitel Instant Replay: sequence Event %llu could not be marked played",
			     (unsigned long long)active->event_id);

		if (active->angle_sequence) {
			blog(LOG_INFO,
			     "Pitel Instant Replay: Event %llu angle sequence advanced to bus %c angle %zu/%zu '%s'",
			     (unsigned long long)active->event_id, target_bus == SR_REPLAY_BUS_A ? 'A' : 'B', next + 1,
			     active->count, active->angle_cameras[next] ? active->angle_cameras[next] : "");
		} else {
			blog(LOG_INFO,
			     "Pitel Instant Replay: Event List %u advanced to bus %c item %zu/%zu (Event %llu)",
			     active->list_id, target_bus == SR_REPLAY_BUS_A ? 'A' : 'B', next + 1, active->count,
			     (unsigned long long)active->event_id);
		}
		return true;
	}

	if (playlist->angle_sequence)
		blog(LOG_INFO, "Pitel Instant Replay: Event %llu angle sequence finished on bus %c",
		     (unsigned long long)playlist->event_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B');
	else
		blog(LOG_INFO, "Pitel Instant Replay: Event List %u finished on bus %c", playlist->list_id,
		     bus == SR_REPLAY_BUS_A ? 'A' : 'B');
	clear_bus_locked(playlist);
	return false;
}

bool sr_replay_playlist_next(enum sr_replay_bus bus)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist)
		return false;

	bool queue_take = false;
	enum sr_replay_bus take_bus = bus;
	pthread_mutex_lock(&g_mutex);
	const bool advanced = advance_locked(bus, playlist, &queue_take, &take_bus);
	pthread_mutex_unlock(&g_mutex);

	if (advanced && queue_take)
		sr_replay_take_advance_event_async(g_events, take_bus);
	return advanced;
}

bool sr_replay_playlist_advance_on_end(enum sr_replay_bus bus)
{
	return sr_replay_playlist_next(bus);
}

void sr_replay_playlist_stop(enum sr_replay_bus bus)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist)
		return;
	pthread_mutex_lock(&g_mutex);
	clear_bus_locked(playlist);
	pthread_mutex_unlock(&g_mutex);
}

bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist || !state)
		return false;
	memset(state, 0, sizeof(*state));
	pthread_mutex_lock(&g_mutex);
	state->active = playlist->active;
	state->angle_sequence = playlist->angle_sequence;
	state->list_id = playlist->list_id;
	state->position = playlist->position;
	state->count = playlist->count;
	state->event_id = playlist->event_id;
	pthread_mutex_unlock(&g_mutex);
	return true;
}

bool sr_replay_playlist_snapshot_items(enum sr_replay_bus bus, uint64_t **event_ids_out, size_t *count_out,
				       size_t *position_out, bool *angle_sequence_out)
{
	struct sr_playlist_bus *playlist = get_bus(bus);
	if (!g_started || !playlist || !event_ids_out || !count_out || !position_out || !angle_sequence_out)
		return false;
	*event_ids_out = NULL;
	*count_out = 0;
	*position_out = 0;
	*angle_sequence_out = false;

	pthread_mutex_lock(&g_mutex);
	if (!playlist->active || !playlist->event_ids || !playlist->count) {
		pthread_mutex_unlock(&g_mutex);
		return false;
	}
	uint64_t *copy = bmalloc(playlist->count * sizeof(*copy));
	if (!copy) {
		pthread_mutex_unlock(&g_mutex);
		return false;
	}
	memcpy(copy, playlist->event_ids, playlist->count * sizeof(*copy));
	*event_ids_out = copy;
	*count_out = playlist->count;
	*position_out = playlist->position;
	*angle_sequence_out = playlist->angle_sequence;
	pthread_mutex_unlock(&g_mutex);
	return true;
}
