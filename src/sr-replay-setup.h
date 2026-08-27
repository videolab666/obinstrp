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

#define SR_REPLAY_SETUP_SCENE_A "Pitel Instant Replay A"
#define SR_REPLAY_SETUP_SCENE_B "Pitel Instant Replay B"
#define SR_REPLAY_SETUP_OUTPUT_A "Pitel Replay Output A"
#define SR_REPLAY_SETUP_OUTPUT_B "Pitel Replay Output B"
#define SR_REPLAY_SETUP_NAME_MAX 256
#define SR_REPLAY_SETUP_ID_MAX 128

struct sr_replay_setup_source {
	char name[SR_REPLAY_SETUP_NAME_MAX];
	char type_id[SR_REPLAY_SETUP_ID_MAX];
	bool compatible;
	bool has_capture;
	bool capture_enabled;
};

struct sr_replay_setup_snapshot {
	struct sr_replay_setup_source *sources;
	size_t source_count;
	size_t compatible_source_count;
	size_t capture_source_count;
	size_t enabled_capture_source_count;
	bool bus_a_ready;
	bool bus_b_ready;
	bool event_transition_ready;
	bool program_output_supported;
	bool program_output_enabled;
	char scene_a[SR_REPLAY_SETUP_NAME_MAX];
	char scene_b[SR_REPLAY_SETUP_NAME_MAX];
};

struct sr_replay_setup_result {
	bool changed;
	bool created_scene_a;
	bool created_scene_b;
	bool added_output_a;
	bool added_output_b;
	bool event_transition_ready;
	char scene_a[SR_REPLAY_SETUP_NAME_MAX];
	char scene_b[SR_REPLAY_SETUP_NAME_MAX];
};

/* Replay Setup enumerates and may mutate OBS frontend source/scene topology.
 * These APIs are frontend-thread operations; never call them from render,
 * decode or encoder callbacks. */

/* Snapshot of replay topology plus compatible asynchronous video sources.
 * Storage in snapshot->sources belongs to the caller. */
bool sr_replay_setup_get_snapshot(struct sr_replay_setup_snapshot *snapshot);
void sr_replay_setup_free_snapshot(struct sr_replay_setup_snapshot *snapshot);

/* Add/enable or remove the Pitel Capture filter on one named OBS video source.
 * Other filters on that source are never modified. */
bool sr_replay_setup_set_capture(const char *source_name, bool enabled);

/* Select/deselect final OBS Program/PGM as a persistent replay pseudo-angle. */
bool sr_replay_setup_set_program_output(bool enabled);

/* Create/repair two scene-backed Event Outputs for A/B playback. Existing
 * valid user topology is preserved. If A and B currently resolve to one scene,
 * only the missing separate side is added to a canonical Pitel Instant Replay scene. */
bool sr_replay_setup_ensure_event_scenes(struct sr_replay_setup_result *result);

/* Preferred topology lookup used by TAKE/playlist code. Canonical Setup scenes
 * win when present, while manually-created Event Output scenes remain a valid
 * fallback. Returned strings are bstrdup allocations owned by the caller. */
char *sr_replay_setup_find_output_source_name(enum sr_replay_bus bus);
char *sr_replay_setup_find_output_scene_name(enum sr_replay_bus bus);

#ifdef __cplusplus
}
#endif
