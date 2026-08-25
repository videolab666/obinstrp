/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CONFIG_SCHEMA_VERSION 5

enum sr_storage_low_space_action {
	SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED = 0,
	SR_STORAGE_LOW_SPACE_STOP_RECORDING = 1,
	SR_STORAGE_LOW_SPACE_WARN_ONLY = 2,
};

void sr_config_init(void);
void sr_config_free(void);

/* Legacy exported replay MP4 folder. Returned strings are bstrdup allocations
 * owned by the caller. */
char *sr_config_get_save_dir(void);
void sr_config_set_save_dir(const char *save_dir);

/* Root directory for long-running continuous replay sessions. */
char *sr_config_get_session_root(void);
void sr_config_set_session_root(const char *session_root);

/* Storage safety defaults used by the future storage manager. They are
 * persisted now so the disk recorder and later GC share one config schema. */
uint64_t sr_config_get_min_free_bytes(void);
void sr_config_set_min_free_bytes(uint64_t bytes);

uint64_t sr_config_get_purge_target_bytes(void);
void sr_config_set_purge_target_bytes(uint64_t bytes);

enum sr_storage_low_space_action sr_config_get_low_space_action(void);
void sr_config_set_low_space_action(enum sr_storage_low_space_action action);

uint32_t sr_config_get_segment_duration_ms(void);
void sr_config_set_segment_duration_ms(uint32_t milliseconds);

/* Optional names of existing native OBS Stinger transitions used when the
 * operator enters replay from live and explicitly returns to live. An empty
 * string means keep the transition currently selected in OBS. Returned
 * strings are bstrdup allocations owned by the caller. */
char *sr_config_get_take_in_transition(void);
void sr_config_set_take_in_transition(const char *transition_name);
char *sr_config_get_take_out_transition(void);
void sr_config_set_take_out_transition(const char *transition_name);

/* Optional non-Stinger OBS transition used between Events/angles while
 * replay is already on Program. Empty means a direct same-bus cut. */
char *sr_config_get_event_transition(void);
void sr_config_set_event_transition(const char *transition_name);
uint32_t sr_config_get_event_transition_duration_ms(void);
void sr_config_set_event_transition_duration_ms(uint32_t milliseconds);

#ifdef __cplusplus
}
#endif
