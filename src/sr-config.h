/*
 * Pitel Instant Replay - persistent configuration interface
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

#define SR_CONFIG_SCHEMA_VERSION 8

enum sr_storage_low_space_action {
	SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED = 0,
	SR_STORAGE_LOW_SPACE_STOP_RECORDING = 1,
	SR_STORAGE_LOW_SPACE_WARN_ONLY = 2,
};

enum sr_replay_speed_policy {
	SR_REPLAY_SPEED_GLOBAL = 0,
	SR_REPLAY_SPEED_EVENT = 1,
};

void sr_config_init(void);
void sr_config_free(void);

/* Deprecated compatibility accessors for profiles created by the pre-Session
 * loose-MP4 replay implementation. New UI and recording code use session_root. */
char *sr_config_get_save_dir(void);
void sr_config_set_save_dir(const char *save_dir);

char *sr_config_get_session_root(void);
void sr_config_set_session_root(const char *session_root);

uint64_t sr_config_get_min_free_bytes(void);
void sr_config_set_min_free_bytes(uint64_t bytes);
uint64_t sr_config_get_purge_target_bytes(void);
void sr_config_set_purge_target_bytes(uint64_t bytes);

enum sr_storage_low_space_action sr_config_get_low_space_action(void);
void sr_config_set_low_space_action(enum sr_storage_low_space_action action);

uint32_t sr_config_get_segment_duration_ms(void);
void sr_config_set_segment_duration_ms(uint32_t milliseconds);

char *sr_config_get_take_in_transition(void);
void sr_config_set_take_in_transition(const char *transition_name);
char *sr_config_get_take_out_transition(void);
void sr_config_set_take_out_transition(const char *transition_name);
char *sr_config_get_event_transition(void);
void sr_config_set_event_transition(const char *transition_name);
uint32_t sr_config_get_event_transition_duration_ms(void);
void sr_config_set_event_transition_duration_ms(uint32_t milliseconds);
bool sr_config_get_event_transition_match_replay_speed(void);
void sr_config_set_event_transition_match_replay_speed(bool enabled);

enum sr_replay_speed_policy sr_config_get_replay_speed_policy(void);
void sr_config_set_replay_speed_policy(enum sr_replay_speed_policy policy);

bool sr_config_get_program_output_enabled(void);
void sr_config_set_program_output_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
