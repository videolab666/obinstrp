/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-controller.h"

#include "sr-event-db.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <limits.h>
#include <string.h>

struct sr_event_controller {
	pthread_mutex_t mutex;
	struct sr_event_db *db;
	unsigned current_list;
	uint64_t mark_in_ns;
	bool have_mark_in;
};

static bool valid_list_id(unsigned list_id)
{
	return list_id >= 1 && list_id <= SR_EVENT_LIST_COUNT;
}

static bool valid_time(uint64_t timestamp_ns)
{
	return timestamp_ns <= (uint64_t)INT64_MAX;
}

static bool ensure_db_locked(struct sr_event_controller *controller)
{
	if (controller->db)
		return true;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir)
		return false;

	controller->db = sr_event_db_open(session_dir);
	bfree(session_dir);
	return controller->db != NULL;
}

static struct sr_event_write default_event(uint64_t in_ns, uint64_t out_ns, bool pending)
{
	struct sr_event_write event;
	memset(&event, 0, sizeof(event));
	event.in_ns = in_ns;
	event.out_ns = out_ns;
	event.speed_percent = 100.0;
	event.audio_mode = SR_EVENT_AUDIO_MASTER;
	event.pending = pending;
	event.name = "";
	event.tag = "";
	return event;
}

static bool create_in_current_list_locked(struct sr_event_controller *controller, const struct sr_event_write *event,
					  uint64_t *event_id)
{
	return ensure_db_locked(controller) &&
	       sr_event_db_create_event_in_list(controller->db, event, controller->current_list, -1, event_id);
}

static bool update_flag_locked(struct sr_event_controller *controller, uint64_t event_id, bool played,
			       bool protected_event, bool change_played)
{
	if (!ensure_db_locked(controller))
		return false;

	struct sr_event_record record;
	if (!sr_event_db_get_event(controller->db, event_id, &record))
		return false;

	struct sr_event_write write = {
		.in_ns = record.in_ns,
		.out_ns = record.out_ns,
		.preferred_camera_id = record.preferred_camera_id,
		.speed_percent = record.speed_percent,
		.audio_mode = record.audio_mode,
		.protected_event = change_played ? record.protected_event : protected_event,
		.played = change_played ? played : record.played,
		.pending = record.pending,
		.name = record.name,
		.tag = record.tag,
	};
	const bool ok = sr_event_db_update_event(controller->db, event_id, &write);
	sr_event_record_free(&record);
	return ok;
}

struct sr_event_controller *sr_event_controller_create(void)
{
	struct sr_event_controller *controller = bzalloc(sizeof(*controller));
	pthread_mutex_init(&controller->mutex, NULL);
	controller->current_list = 1;
	return controller;
}

void sr_event_controller_destroy(struct sr_event_controller *controller)
{
	if (!controller)
		return;

	sr_event_db_close(controller->db);
	pthread_mutex_destroy(&controller->mutex);
	bfree(controller);
}

bool sr_event_controller_set_current_list(struct sr_event_controller *controller, unsigned list_id)
{
	if (!controller || !valid_list_id(list_id))
		return false;

	pthread_mutex_lock(&controller->mutex);
	controller->current_list = list_id;
	pthread_mutex_unlock(&controller->mutex);
	return true;
}

unsigned sr_event_controller_get_current_list(struct sr_event_controller *controller)
{
	if (!controller)
		return 0;

	pthread_mutex_lock(&controller->mutex);
	const unsigned list_id = controller->current_list;
	pthread_mutex_unlock(&controller->mutex);
	return list_id;
}

bool sr_event_controller_mark_in(struct sr_event_controller *controller, uint64_t now_ns)
{
	if (!controller || !valid_time(now_ns))
		return false;

	pthread_mutex_lock(&controller->mutex);
	controller->mark_in_ns = now_ns;
	controller->have_mark_in = true;
	pthread_mutex_unlock(&controller->mutex);
	return true;
}

void sr_event_controller_cancel_mark_in(struct sr_event_controller *controller)
{
	if (!controller)
		return;

	pthread_mutex_lock(&controller->mutex);
	controller->mark_in_ns = 0;
	controller->have_mark_in = false;
	pthread_mutex_unlock(&controller->mutex);
}

bool sr_event_controller_get_mark_in(struct sr_event_controller *controller, uint64_t *in_ns)
{
	if (!controller)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool have_mark_in = controller->have_mark_in;
	if (have_mark_in && in_ns)
		*in_ns = controller->mark_in_ns;
	pthread_mutex_unlock(&controller->mutex);
	return have_mark_in;
}

bool sr_event_controller_mark_out(struct sr_event_controller *controller, uint64_t now_ns, uint64_t *event_id)
{
	if (!controller || !valid_time(now_ns))
		return false;

	pthread_mutex_lock(&controller->mutex);
	if (!controller->have_mark_in || now_ns < controller->mark_in_ns) {
		pthread_mutex_unlock(&controller->mutex);
		return false;
	}

	const struct sr_event_write event = default_event(controller->mark_in_ns, now_ns, false);
	const bool ok = create_in_current_list_locked(controller, &event, event_id);
	if (ok) {
		controller->mark_in_ns = 0;
		controller->have_mark_in = false;
	}
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_quick_mark(struct sr_event_controller *controller, uint64_t now_ns, uint64_t pre_roll_ns,
				    uint64_t post_roll_ns, uint64_t *event_id)
{
	if (!controller || !valid_time(now_ns))
		return false;
	if (post_roll_ns > (uint64_t)INT64_MAX - now_ns)
		return false;

	const uint64_t in_ns = now_ns > pre_roll_ns ? now_ns - pre_roll_ns : 0;
	const uint64_t out_ns = now_ns + post_roll_ns;
	const struct sr_event_write event = default_event(in_ns, out_ns, post_roll_ns != 0);

	pthread_mutex_lock(&controller->mutex);
	const bool ok = create_in_current_list_locked(controller, &event, event_id);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_get_event(struct sr_event_controller *controller, uint64_t event_id,
				   struct sr_event_record *event)
{
	if (!controller || !event_id || !event)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = ensure_db_locked(controller) && sr_event_db_get_event(controller->db, event_id, event);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_update_event(struct sr_event_controller *controller, uint64_t event_id,
				      const struct sr_event_write *event)
{
	if (!controller || !event_id || !event)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = ensure_db_locked(controller) && sr_event_db_update_event(controller->db, event_id, event);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_delete_event(struct sr_event_controller *controller, uint64_t event_id)
{
	if (!controller || !event_id)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = ensure_db_locked(controller) && sr_event_db_delete_event(controller->db, event_id);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

void sr_event_controller_free_event(struct sr_event_record *event)
{
	sr_event_record_free(event);
}

bool sr_event_controller_get_list_events(struct sr_event_controller *controller, unsigned list_id, uint64_t **event_ids,
					 size_t *count)
{
	if (!controller || !valid_list_id(list_id) || !event_ids || !count)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = ensure_db_locked(controller) &&
			sr_event_db_get_list_events(controller->db, list_id, event_ids, count);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_copy_to_list(struct sr_event_controller *controller, uint64_t event_id, unsigned target_list,
				      int position)
{
	if (!controller || !event_id || !valid_list_id(target_list))
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = ensure_db_locked(controller) &&
			sr_event_db_add_event_to_list(controller->db, target_list, event_id, position);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_move_to_list(struct sr_event_controller *controller, uint64_t event_id, unsigned source_list,
				      unsigned target_list, int position)
{
	if (!controller || !event_id || !valid_list_id(source_list) || !valid_list_id(target_list))
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok =
		ensure_db_locked(controller) &&
		sr_event_db_move_event_between_lists(controller->db, event_id, source_list, target_list, position);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_reorder(struct sr_event_controller *controller, uint64_t event_id, unsigned list_id,
				 int position)
{
	if (!controller || !event_id || !valid_list_id(list_id))
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = ensure_db_locked(controller) &&
			sr_event_db_set_event_position(controller->db, list_id, event_id, position);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_duplicate(struct sr_event_controller *controller, uint64_t event_id, unsigned target_list,
				   int position, uint64_t *new_event_id)
{
	if (!controller || !event_id || !valid_list_id(target_list))
		return false;

	pthread_mutex_lock(&controller->mutex);
	if (!ensure_db_locked(controller)) {
		pthread_mutex_unlock(&controller->mutex);
		return false;
	}

	struct sr_event_record record;
	if (!sr_event_db_get_event(controller->db, event_id, &record)) {
		pthread_mutex_unlock(&controller->mutex);
		return false;
	}

	const struct sr_event_write write = {
		.in_ns = record.in_ns,
		.out_ns = record.out_ns,
		.preferred_camera_id = record.preferred_camera_id,
		.speed_percent = record.speed_percent,
		.audio_mode = record.audio_mode,
		.protected_event = record.protected_event,
		.played = record.played,
		.pending = record.pending,
		.name = record.name,
		.tag = record.tag,
	};

	uint64_t duplicate_id = 0;
	const bool ok = sr_event_db_create_event_in_list(controller->db, &write, target_list, position, &duplicate_id);
	sr_event_record_free(&record);

	if (ok && new_event_id)
		*new_event_id = duplicate_id;
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_set_played(struct sr_event_controller *controller, uint64_t event_id, bool played)
{
	if (!controller || !event_id)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = update_flag_locked(controller, event_id, played, false, true);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}

bool sr_event_controller_set_protected(struct sr_event_controller *controller, uint64_t event_id, bool protected_event)
{
	if (!controller || !event_id)
		return false;

	pthread_mutex_lock(&controller->mutex);
	const bool ok = update_flag_locked(controller, event_id, false, protected_event, false);
	pthread_mutex_unlock(&controller->mutex);
	return ok;
}
