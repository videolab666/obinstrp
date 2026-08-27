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
	bool angle_sequence;
	unsigned list_id;
	size_t position; /* zero based */
	size_t count;
	uint64_t event_id;
};

/* Treats one of the existing 20 ordered Event Lists as a highlight reel.
 * Media is never duplicated: the playlist snapshots only Event ids and cues
 * the existing disk-backed A/B channels for each item in order. */
bool sr_replay_playlist_init(struct sr_event_controller *events);
void sr_replay_playlist_shutdown(void);

/* Portable legacy start: advances on the same bus with a direct Event cut. */
bool sr_replay_playlist_start(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera);

/* vMix-style sequence start. When cross_bus_transitions is true, every later
 * playable item is pre-cued on the opposite A/B bus and the TAKE layer is
 * asked to switch replay scenes with the configured Event Transition. The
 * first item still enters replay through the normal TAKE IN Stinger. */
bool sr_replay_playlist_start_with_transitions(enum sr_replay_bus bus, unsigned list_id, const char *preferred_camera,
					       bool cross_bus_transitions);

/* Plays only the supplied Event ids, preserving their caller-provided order.
 * The ids are snapshotted by the playlist, so the caller may release its
 * selection immediately after this function returns. */
bool sr_replay_playlist_start_events_with_transitions(enum sr_replay_bus bus, unsigned list_id,
						      const uint64_t *event_ids, size_t count,
						      const char *preferred_camera, bool cross_bus_transitions);

/* Plays every usable camera angle of one selected Event in camera order. Full
 * coverage angles are preferred; PARTIAL angles are used only when no camera
 * has FULL coverage. The same cross-bus Event Transition mechanism is used
 * between angles, so the TAKE IN/OUT Stingers are not retriggered. */
bool sr_replay_playlist_start_event_angles(enum sr_replay_bus bus, uint64_t event_id, bool cross_bus_transitions);

/* Operator skip and automatic end-of-item advance. Both skip deleted,
 * Pending or currently unplayable Events/angles. */
bool sr_replay_playlist_next(enum sr_replay_bus bus);
bool sr_replay_playlist_advance_on_end(enum sr_replay_bus bus);

/* Stops only the sequence currently owned by this bus. A sequence that has
 * already migrated A -> B during an Event Transition is intentionally not
 * stopped when the old A Event Output deactivates. */
void sr_replay_playlist_stop(enum sr_replay_bus bus);
bool sr_replay_playlist_get_state(enum sr_replay_bus bus, struct sr_replay_playlist_state *state);

/* Snapshot the current sequence items for UI progress/countdown. The caller owns
 * event_ids_out and must free it with bfree(). */
bool sr_replay_playlist_snapshot_items(enum sr_replay_bus bus, uint64_t **event_ids_out, size_t *count_out,
				       size_t *position_out, bool *angle_sequence_out);

#ifdef __cplusplus
}
#endif
