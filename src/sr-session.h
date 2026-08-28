/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Replay session manager.
 *
 * Three identities are deliberately separate:
 *   opened session       - media/Event database currently being edited/played;
 *   recording target     - session selected for the next START RECORD;
 *   recording session    - immutable target of the currently active REC run.
 */
void sr_session_init(void);
void sr_session_free(void);

/* Compatibility read path. Normal replay/editor code receives the Opened
 * Session. Producer translation units are built with
 * SR_SESSION_RECORDING_PRODUCER, which aliases this legacy call to the active
 * Recording Session without invasive changes to the stable capture code. */
char *sr_session_get_or_create_path(void);
char *sr_session_get_opened_path(void);
char *sr_session_get_record_target_path(void);
char *sr_session_get_recording_path(void);
char *sr_session_get_id(void); /* opened session id */

bool sr_session_create_new(const char *display_name, bool make_opened, bool make_record_target, char **created_path);
bool sr_session_open(const char *path);
bool sr_session_set_record_target(const char *path);
void sr_session_clear_record_target(void);

bool sr_session_prepare_recording(uint64_t obs_now_ns);
void sr_session_finish_recording(uint64_t obs_now_ns);
bool sr_session_recording_is_active(void);
uint64_t sr_session_map_recording_timestamp(uint64_t obs_timestamp_ns);
uint64_t sr_session_recording_start_ns(void);
uint64_t sr_session_recording_generation(void);
bool sr_session_recording_starts_with_discontinuity(void);
bool sr_session_get_media_bounds(const char *session_dir, uint64_t *start_ns, uint64_t *end_ns);

bool sr_session_path_is_active(const char *path);
bool sr_session_path_is_opened(const char *path);
bool sr_session_path_is_record_target(const char *path);

char *sr_session_get_display_name(const char *path);
bool sr_session_rename(const char *path, const char *display_name);

bool sr_session_register_camera(const char *session_dir, const char *stable_key, const char *display_name,
				int64_t sync_offset_ns);
bool sr_session_resolve_camera(const char *session_dir, const char *camera_name, char *stable_key, size_t key_size,
			       int64_t *sync_offset_ns);
bool sr_session_list_camera_names(const char *session_dir, char ***names, size_t *count);
void sr_session_free_camera_names(char **names, size_t count);

#ifdef SR_SESSION_RECORDING_PRODUCER
#define sr_session_get_or_create_path sr_session_get_recording_path
#endif

#ifdef __cplusplus
}
#endif
