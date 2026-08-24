/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "sr-replay-channel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;

struct sr_replay_playlist_state {
	bool active;
	unsigned list_id;
	size_t position; /* zero based */
	size_t count;
	uint64_t event_id;
};

/* Treats one of the existing 20 ordered Event Lists as a highlight reel.
 * Media is never duplicated: the playlist snapshots only Event ids and cues
 * the existing disk-backed A/B channel for each item in order. */
bool sr_replay_playlist_init(struct sr_event_controller *events);
void sr_replay_playlist_shutdown(void);

/* Starts the list at its first currently playable, non-Pending Event and
 * starts that bus. preferred_camera may be NULL/empty; the selector prefers
 * the bus's current camera, then the requested camera, then another camera
 * with FULL coverage before falling back to PARTIAL coverage. */
bool sr_replay_playlist_start(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera);

/* Operator skip and automatic end-of-Event advance. Both skip deleted,
 * Pending or currently unplayable Events. */
bool sr_replay_playlist_next(enum sr_replay_bus bus);
bool sr_replay_playlist_advance_on_end(enum sr_replay_bus bus);

/* Stops only automatic list advancement; it does not clear the currently
 * cued Event. */
void sr_replay_playlist_stop(enum sr_replay_bus bus);
bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state);

#ifdef __cplusplus
}
#endif
