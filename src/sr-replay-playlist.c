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
#include "sr-event-controller.h"
#include "sr-replay-coverage.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <pthread.h>
#include <string.h>

struct sr_playlist_bus {
	bool active;
	unsigned list_id;
	uint64_t *event_ids;
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

static void clear_bus_locked(struct sr_playlist_bus *bus)
{
	if (!bus)
		return;
	bfree(bus->event_ids);
	bfree(bus->preferred_camera);
	memset(bus, 0, sizeof(*bus));
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
	for (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++)
		clear_bus_locked(&g_buses[i]);
	g_events = NULL;
	g_started = false;
	pthread_mutex_unlock(&g_mutex);
	pthread_mutex_destroy(&g_mutex);
}

bool sr_replay_playlist_start(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera)
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
	clear_bus_locked(playlist);

	size_t first = 0;
	bool cued = false;
	for (; first < count; first++) {
		if (cue_best_camera_locked(bus, event_ids[first], preferred_camera) && sr_replay_channel_play(bus)) {
			cued = true;
			break;
		}
	}

	if (!cued) {
		pthread_mutex_unlock(&g_mutex);
		bfree(event_ids);
		return false;
	}

	playlist->active = true;
	playlist->list_id = list_id;
	playlist->event_ids = event_ids;
	playlist->count = count;
	playlist->position = first;
	playlist->event_id = event_ids[first];
	playlist->preferred_camera = bstrdup(preferred_camera ? preferred_camera : "");
	const uint64_t first_event_id = playlist->event_id;
	pthread_mutex_unlock(&g_mutex);

	blog(LOG_INFO,
	     "Pitel Instant Replay: started Event List %u highlight reel on bus %c at item %zu/%zu (Event %llu)",
	     list_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', first + 1, count, (unsigned long long)first_event_id);
	return true;
}

static bool advance_locked(enum sr_replay_bus bus, struct sr_playlist_bus *playlist)
{
	if (!playlist->active || playlist->position >= playlist->count)
		return false;

	struct sr_replay_channel_state current = {0};
	if (!sr_replay_channel_get_state(bus, &current) || !current.cued || current.event_id != playlist->event_id) {
		clear_bus_locked(playlist);
		return false;
	}

	for (size_t next = playlist->position + 1; next < playlist->count; next++) {
		if (!cue_best_camera_locked(bus, playlist->event_ids[next], playlist->preferred_camera))
			continue;
		if (!sr_replay_channel_play(bus))
			continue;

		playlist->position = next;
		playlist->event_id = playlist->event_ids[next];
		if (!sr_event_controller_set_played(g_events, playlist->event_id, true))
			blog(LOG_WARNING, "Pitel Instant Replay: playlist Event %llu could not be marked played",
			     (unsigned long long)playlist->event_id);
		blog(LOG_INFO, "Pitel Instant Replay: Event List %u advanced bus %c to item %zu/%zu (Event %llu)",
		     playlist->list_id, bus == SR_REPLAY_BUS_A ? 'A' : 'B', next + 1, playlist->count,
		     (unsigned long long)playlist->event_id);
		return true;
	}

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
	pthread_mutex_lock(&g_mutex);
	const bool advanced = advance_locked(bus, playlist);
	pthread_mutex_unlock(&g_mutex);
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
	state->list_id = playlist->list_id;
	state->position = playlist->position;
	state->count = playlist->count;
	state->event_id = playlist->event_id;
	pthread_mutex_unlock(&g_mutex);
	return true;
}
