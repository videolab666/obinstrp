/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-channel.h"

#include "sr-disk-player.h"
#include "sr-event-controller.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <float.h>
#include <math.h>
#include <string.h>

struct sr_replay_channel {
	pthread_mutex_t mutex;
	struct sr_disk_player *player;
	char *camera_name;

	uint64_t event_id;
	uint64_t in_ns;
	uint64_t out_ns;
	uint64_t playhead_ns;
	uint64_t last_clock_ns;

	double speed_percent;
	uint32_t width;
	uint32_t height;
	bool cued;
	bool playing;
	bool paused;
	bool backward;
	bool loop;
	bool partial_coverage;
	bool need_frame;
};

struct sr_replay_channels {
	struct sr_event_controller *events;
	struct sr_replay_channel buses[SR_REPLAY_BUS_COUNT];
};

static struct sr_replay_channels *g_channels;

static bool valid_bus(enum sr_replay_bus bus)
{
	return bus >= SR_REPLAY_BUS_A && bus < SR_REPLAY_BUS_COUNT;
}

static struct sr_replay_channel *get_bus(enum sr_replay_bus bus)
{
	if (!g_channels || !valid_bus(bus))
		return NULL;
	return &g_channels->buses[bus];
}

static void clear_locked(struct sr_replay_channel *channel)
{
	sr_disk_player_destroy(channel->player);
	channel->player = NULL;
	bfree(channel->camera_name);
	channel->camera_name = NULL;
	channel->event_id = 0;
	channel->in_ns = 0;
	channel->out_ns = 0;
	channel->playhead_ns = 0;
	channel->last_clock_ns = 0;
	channel->speed_percent = 100.0;
	channel->width = 0;
	channel->height = 0;
	channel->cued = false;
	channel->playing = false;
	channel->paused = false;
	channel->backward = false;
	channel->loop = false;
	channel->partial_coverage = false;
	channel->need_frame = false;
}

bool sr_replay_channels_init(struct sr_event_controller *events)
{
	if (g_channels)
		return true;
	if (!events)
		return false;

	struct sr_replay_channels *channels = bzalloc(sizeof(*channels));
	channels->events = events;
	for (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++) {
		pthread_mutex_init(&channels->buses[i].mutex, NULL);
		channels->buses[i].speed_percent = 100.0;
	}
	g_channels = channels;
	return true;
}

void sr_replay_channels_shutdown(void)
{
	struct sr_replay_channels *channels = g_channels;
	if (!channels)
		return;
	g_channels = NULL;

	for (size_t i = 0; i < SR_REPLAY_BUS_COUNT; i++) {
		pthread_mutex_lock(&channels->buses[i].mutex);
		clear_locked(&channels->buses[i]);
		pthread_mutex_unlock(&channels->buses[i].mutex);
		pthread_mutex_destroy(&channels->buses[i].mutex);
	}
	bfree(channels);
}

bool sr_replay_channel_cue(enum sr_replay_bus bus, uint64_t event_id, const char *camera_name)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel || !event_id || !camera_name || !*camera_name)
		return false;

	struct sr_event_record event;
	if (!sr_event_controller_get_event(g_channels->events, event_id, &event))
		return false;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir) {
		sr_event_controller_free_event(&event);
		return false;
	}

	struct sr_disk_player *player = sr_disk_player_create(session_dir, camera_name);
	bfree(session_dir);
	if (!player) {
		sr_event_controller_free_event(&event);
		return false;
	}

	uint64_t first_ns = 0;
	uint64_t last_ns = 0;
	if (!sr_disk_player_get_bounds(player, &first_ns, &last_ns) || event.out_ns < first_ns ||
	    event.in_ns > last_ns) {
		sr_disk_player_destroy(player);
		sr_event_controller_free_event(&event);
		return false;
	}

	const uint64_t in_ns = event.in_ns < first_ns ? first_ns : event.in_ns;
	const uint64_t out_ns = event.out_ns > last_ns ? last_ns : event.out_ns;
	const bool partial = in_ns != event.in_ns || out_ns != event.out_ns;
	const double speed = event.speed_percent;
	sr_event_controller_free_event(&event);

	pthread_mutex_lock(&channel->mutex);
	clear_locked(channel);
	channel->player = player;
	channel->camera_name = bstrdup(camera_name);
	channel->event_id = event_id;
	channel->in_ns = in_ns;
	channel->out_ns = out_ns;
	channel->playhead_ns = in_ns;
	channel->speed_percent = speed;
	channel->cued = true;
	channel->partial_coverage = partial;
	channel->need_frame = true;
	pthread_mutex_unlock(&channel->mutex);

	blog(LOG_INFO, "Sports Replay: cued Event %llu on bus %c, camera '%s', %.3f s%s", (unsigned long long)event_id,
	     bus == SR_REPLAY_BUS_A ? 'A' : 'B', camera_name, (double)(out_ns - in_ns) / 1e9,
	     partial ? " (partial media coverage)" : "");
	return true;
}

void sr_replay_channel_clear(enum sr_replay_bus bus)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return;
	pthread_mutex_lock(&channel->mutex);
	clear_locked(channel);
	pthread_mutex_unlock(&channel->mutex);
}

bool sr_replay_channel_play(enum sr_replay_bus bus)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return false;

	pthread_mutex_lock(&channel->mutex);
	const bool ok = channel->cued;
	if (ok) {
		channel->playing = true;
		channel->paused = false;
		channel->last_clock_ns = 0;
		channel->need_frame = true;
	}
	pthread_mutex_unlock(&channel->mutex);
	return ok;
}

bool sr_replay_channel_pause(enum sr_replay_bus bus, bool paused)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return false;

	pthread_mutex_lock(&channel->mutex);
	const bool ok = channel->cued;
	if (ok) {
		channel->paused = paused;
		if (!paused)
			channel->playing = true;
		channel->last_clock_ns = 0;
		channel->need_frame = true;
	}
	pthread_mutex_unlock(&channel->mutex);
	return ok;
}

void sr_replay_channel_stop(enum sr_replay_bus bus)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return;
	pthread_mutex_lock(&channel->mutex);
	channel->playing = false;
	channel->paused = false;
	channel->last_clock_ns = 0;
	pthread_mutex_unlock(&channel->mutex);
}

void sr_replay_channel_restart(enum sr_replay_bus bus)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return;
	pthread_mutex_lock(&channel->mutex);
	if (channel->cued) {
		channel->playhead_ns = channel->backward ? channel->out_ns : channel->in_ns;
		channel->playing = true;
		channel->paused = false;
		channel->last_clock_ns = 0;
		channel->need_frame = true;
	}
	pthread_mutex_unlock(&channel->mutex);
}

bool sr_replay_channel_set_speed(enum sr_replay_bus bus, double speed_percent)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel || !isfinite(speed_percent))
		return false;
	if (speed_percent < 10.0)
		speed_percent = 10.0;
	if (speed_percent > 400.0)
		speed_percent = 400.0;

	pthread_mutex_lock(&channel->mutex);
	channel->speed_percent = speed_percent;
	channel->last_clock_ns = 0;
	pthread_mutex_unlock(&channel->mutex);
	return true;
}

bool sr_replay_channel_set_backward(enum sr_replay_bus bus, bool backward)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return false;
	pthread_mutex_lock(&channel->mutex);
	channel->backward = backward;
	channel->last_clock_ns = 0;
	channel->need_frame = channel->cued;
	pthread_mutex_unlock(&channel->mutex);
	return true;
}

bool sr_replay_channel_set_loop(enum sr_replay_bus bus, bool loop)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return false;
	pthread_mutex_lock(&channel->mutex);
	channel->loop = loop;
	pthread_mutex_unlock(&channel->mutex);
	return true;
}

bool sr_replay_channel_seek(enum sr_replay_bus bus, uint64_t timestamp_ns)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return false;

	pthread_mutex_lock(&channel->mutex);
	if (!channel->cued) {
		pthread_mutex_unlock(&channel->mutex);
		return false;
	}
	if (timestamp_ns < channel->in_ns)
		timestamp_ns = channel->in_ns;
	if (timestamp_ns > channel->out_ns)
		timestamp_ns = channel->out_ns;
	channel->playhead_ns = timestamp_ns;
	channel->last_clock_ns = 0;
	channel->need_frame = true;
	pthread_mutex_unlock(&channel->mutex);
	return true;
}

bool sr_replay_channel_seek_relative(enum sr_replay_bus bus, int64_t delta_ns)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel)
		return false;

	pthread_mutex_lock(&channel->mutex);
	if (!channel->cued) {
		pthread_mutex_unlock(&channel->mutex);
		return false;
	}

	uint64_t target = channel->playhead_ns;
	if (delta_ns < 0) {
		const uint64_t magnitude = delta_ns == INT64_MIN ? (uint64_t)INT64_MAX + 1ULL : (uint64_t)(-delta_ns);
		target = magnitude >= target - channel->in_ns ? channel->in_ns : target - magnitude;
	} else {
		const uint64_t magnitude = (uint64_t)delta_ns;
		target = magnitude >= channel->out_ns - target ? channel->out_ns : target + magnitude;
	}
	channel->playhead_ns = target;
	channel->last_clock_ns = 0;
	channel->need_frame = true;
	pthread_mutex_unlock(&channel->mutex);
	return true;
}

bool sr_replay_channel_get_state(enum sr_replay_bus bus, struct sr_replay_channel_state *state)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel || !state)
		return false;

	memset(state, 0, sizeof(*state));
	pthread_mutex_lock(&channel->mutex);
	state->event_id = channel->event_id;
	state->in_ns = channel->in_ns;
	state->out_ns = channel->out_ns;
	state->playhead_ns = channel->playhead_ns;
	state->speed_percent = channel->speed_percent;
	state->width = channel->width;
	state->height = channel->height;
	state->cued = channel->cued;
	state->playing = channel->playing;
	state->paused = channel->paused;
	state->backward = channel->backward;
	state->loop = channel->loop;
	state->partial_coverage = channel->partial_coverage;
	if (channel->camera_name)
		strncpy(state->camera_name, channel->camera_name, sizeof(state->camera_name) - 1);
	pthread_mutex_unlock(&channel->mutex);
	return true;
}

static uint64_t scaled_delta(uint64_t elapsed_ns, double speed_percent)
{
	const long double scaled = (long double)elapsed_ns * (long double)speed_percent / 100.0L;
	if (scaled <= 0.0L)
		return 0;
	if (scaled >= (long double)UINT64_MAX)
		return UINT64_MAX;
	return (uint64_t)scaled;
}

static bool advance_locked(struct sr_replay_channel *channel, uint64_t clock_ns, bool *ended)
{
	*ended = false;
	if (!channel->playing || channel->paused) {
		channel->last_clock_ns = clock_ns;
		return false;
	}
	if (!channel->last_clock_ns || clock_ns <= channel->last_clock_ns) {
		channel->last_clock_ns = clock_ns;
		return false;
	}

	const uint64_t elapsed = clock_ns - channel->last_clock_ns;
	channel->last_clock_ns = clock_ns;
	const uint64_t delta = scaled_delta(elapsed, channel->speed_percent);
	if (!delta)
		return false;

	if (channel->backward) {
		const uint64_t remaining = channel->playhead_ns - channel->in_ns;
		if (delta >= remaining) {
			channel->playhead_ns = channel->in_ns;
			*ended = true;
		} else {
			channel->playhead_ns -= delta;
		}
	} else {
		const uint64_t remaining = channel->out_ns - channel->playhead_ns;
		if (delta >= remaining) {
			channel->playhead_ns = channel->out_ns;
			*ended = true;
		} else {
			channel->playhead_ns += delta;
		}
	}
	channel->need_frame = true;

	if (*ended && channel->loop) {
		channel->playhead_ns = channel->backward ? channel->out_ns : channel->in_ns;
		channel->last_clock_ns = 0;
		*ended = false;
	} else if (*ended) {
		channel->playing = false;
		channel->paused = false;
		channel->last_clock_ns = 0;
	}
	return true;
}

bool sr_replay_channel_render(enum sr_replay_bus bus, uint64_t clock_ns, AVFrame **frame, uint64_t *media_timestamp_ns,
			      bool *ended)
{
	struct sr_replay_channel *channel = get_bus(bus);
	if (!channel || !frame)
		return false;
	*frame = NULL;
	if (media_timestamp_ns)
		*media_timestamp_ns = 0;
	if (ended)
		*ended = false;

	pthread_mutex_lock(&channel->mutex);
	if (!channel->cued || !channel->player) {
		pthread_mutex_unlock(&channel->mutex);
		return false;
	}

	bool reached_end = false;
	advance_locked(channel, clock_ns, &reached_end);
	if (!channel->need_frame) {
		if (ended)
			*ended = reached_end;
		pthread_mutex_unlock(&channel->mutex);
		return false;
	}

	AVFrame *decoded = NULL;
	uint64_t actual_ns = 0;
	bool ok = sr_disk_player_decode_at(channel->player, channel->playhead_ns, &decoded, &actual_ns);
	if (!ok) {
		sr_disk_player_refresh(channel->player);
		ok = sr_disk_player_decode_at(channel->player, channel->playhead_ns, &decoded, &actual_ns);
	}
	if (ok && decoded) {
		channel->width = (uint32_t)decoded->width;
		channel->height = (uint32_t)decoded->height;
		channel->need_frame = false;
		*frame = decoded;
		if (media_timestamp_ns)
			*media_timestamp_ns = actual_ns;
	}
	if (ended)
		*ended = reached_end;
	pthread_mutex_unlock(&channel->mutex);
	return ok && decoded != NULL;
}
