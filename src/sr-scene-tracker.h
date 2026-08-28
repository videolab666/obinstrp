/*
 * Pitel Instant Replay - OBS scene/transition bridge
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void sr_scene_tracker_start(void);
void sr_scene_tracker_stop(void);

char *sr_scene_tracker_previous(void);
char *sr_find_scene_with_source(const char *source_name);

void sr_switch_to_scene(const char *scene_name);
void sr_switch_to_scene_with_transition(const char *scene_name, const char *transition_name);
void sr_switch_to_scene_with_transition_duration(const char *scene_name, const char *transition_name,
						 uint32_t duration_ms);

void sr_switch_to_scene_return(const char *scene_name);
void sr_switch_to_scene_return_with_transition(const char *scene_name, const char *transition_name);
void sr_switch_to_scene_of_source_return(const char *source_name);
bool sr_scene_tracker_consume_returning(void);

void sr_scene_tracker_note_replay_launch(void);
void sr_scene_tracker_end_replay_guard(void);

#ifdef __cplusplus
}
#endif
