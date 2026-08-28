/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "sr-event-db.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_event_controller;
struct sr_storage_cleanup_result;

struct sr_event_controller *sr_event_controller_create(void);
void sr_event_controller_destroy(struct sr_event_controller *controller);

/* Switch the browse/edit DB without changing the recording target. Passing a
 * valid Session Manager path persists it as the opened session. */
bool sr_event_controller_open_session(struct sr_event_controller *controller, const char *session_path);
char *sr_event_controller_get_opened_session_path(struct sr_event_controller *controller);

bool sr_event_controller_set_current_list(struct sr_event_controller *controller, unsigned list_id);
unsigned sr_event_controller_get_current_list(struct sr_event_controller *controller);

/* Mark-IN state is bound to a recording-run generation. A STOP/START, New
 * Session or Resume invalidates an unfinished IN mark instead of allowing it
 * to cross a run/session boundary. now_ns is the OBS clock and is mapped into
 * the active session timeline internally. */
bool sr_event_controller_mark_in(struct sr_event_controller *controller, uint64_t now_ns);
void sr_event_controller_cancel_mark_in(struct sr_event_controller *controller);
bool sr_event_controller_get_mark_in(struct sr_event_controller *controller, uint64_t *in_ns);

bool sr_event_controller_mark_out(struct sr_event_controller *controller, uint64_t now_ns, uint64_t *event_id);

/* Creates a quick replay spanning [now-pre_roll, now+post_roll] in the active
 * Recording Session, even when another archive is currently opened for edit. */
bool sr_event_controller_quick_mark(struct sr_event_controller *controller, uint64_t now_ns, uint64_t pre_roll_ns,
				    uint64_t post_roll_ns, uint64_t *event_id);

/* CRUD/list operations below always address the Opened Session. */
bool sr_event_controller_get_event(struct sr_event_controller *controller, uint64_t event_id,
				   struct sr_event_record *event);
bool sr_event_controller_update_event(struct sr_event_controller *controller, uint64_t event_id,
				      const struct sr_event_write *event);
bool sr_event_controller_delete_event(struct sr_event_controller *controller, uint64_t event_id);
void sr_event_controller_free_event(struct sr_event_record *event);

bool sr_event_controller_set_preferred_camera(struct sr_event_controller *controller, uint64_t event_id,
					      const char *camera_name);
bool sr_event_controller_get_camera_name(struct sr_event_controller *controller, uint64_t camera_id,
					 char **camera_name);

bool sr_event_controller_has_event_overlap(struct sr_event_controller *controller, uint64_t start_ns, uint64_t end_ns,
					   bool *overlap);

bool sr_event_controller_delete_event_with_media(struct sr_event_controller *controller, uint64_t event_id,
						 struct sr_storage_cleanup_result *result);

bool sr_event_controller_get_list_events(struct sr_event_controller *controller, unsigned list_id, uint64_t **event_ids,
					 size_t *count);

bool sr_event_controller_copy_to_list(struct sr_event_controller *controller, uint64_t event_id, unsigned target_list,
				      int position);
bool sr_event_controller_move_to_list(struct sr_event_controller *controller, uint64_t event_id, unsigned source_list,
				      unsigned target_list, int position);
bool sr_event_controller_reorder(struct sr_event_controller *controller, uint64_t event_id, unsigned list_id,
				 int position);
bool sr_event_controller_duplicate(struct sr_event_controller *controller, uint64_t event_id, unsigned target_list,
				   int position, uint64_t *new_event_id);

bool sr_event_controller_set_played(struct sr_event_controller *controller, uint64_t event_id, bool played);
bool sr_event_controller_set_protected(struct sr_event_controller *controller, uint64_t event_id, bool protected_event);

#ifdef __cplusplus
}
#endif
