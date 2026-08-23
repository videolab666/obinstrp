/*
Sports Replay
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

#define SR_EVENT_LIST_COUNT 20
#define SR_EVENT_DB_SCHEMA_VERSION 1

enum sr_event_audio_mode {
	SR_EVENT_AUDIO_OFF = 0,
	SR_EVENT_AUDIO_MASTER = 1,
	SR_EVENT_AUDIO_CAMERA = 2,
};

struct sr_event_db;

struct sr_event_write {
	uint64_t in_ns;
	uint64_t out_ns;
	uint64_t preferred_camera_id; /* 0 means automatic / unset */
	double speed_percent;
	int audio_mode;
	bool protected_event;
	bool played;
	bool pending;
	const char *name;
	const char *tag;
};

struct sr_event_record {
	uint64_t id;
	uint64_t in_ns;
	uint64_t out_ns;
	uint64_t preferred_camera_id; /* 0 means automatic / unset */
	double speed_percent;
	int audio_mode;
	bool protected_event;
	bool played;
	bool pending;
	char *name;
	char *tag;
};

/* Opens <session_dir>/session.sqlite and migrates it to the newest supported
 * schema. WAL, foreign keys and a busy timeout are configured on the
 * connection. The object is internally mutex-protected because dock actions,
 * hotkeys and future controllers may arrive from different threads. */
struct sr_event_db *sr_event_db_open(const char *session_dir);
void sr_event_db_close(struct sr_event_db *db);

int sr_event_db_schema_version(struct sr_event_db *db);

bool sr_event_db_create_event(struct sr_event_db *db, const struct sr_event_write *event, uint64_t *event_id);
bool sr_event_db_get_event(struct sr_event_db *db, uint64_t event_id, struct sr_event_record *event);
bool sr_event_db_update_event(struct sr_event_db *db, uint64_t event_id, const struct sr_event_write *event);
bool sr_event_db_delete_event(struct sr_event_db *db, uint64_t event_id);
void sr_event_record_free(struct sr_event_record *event);

/* Event creation and its first list membership are one SQLite transaction.
 * This is the preferred operator path: a crash cannot leave an orphan Event
 * between the two operations. */
bool sr_event_db_create_event_in_list(struct sr_event_db *db, const struct sr_event_write *event, unsigned list_id,
				      int position, uint64_t *event_id);

/* List positions are zero-based and dense. position < 0 appends. Adding an
 * event already present in the list reorders it instead of duplicating it. */
bool sr_event_db_add_event_to_list(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int position);
bool sr_event_db_remove_event_from_list(struct sr_event_db *db, unsigned list_id, uint64_t event_id);
bool sr_event_db_set_event_position(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int position);

/* Moves one membership atomically. If the Event is already in target_list,
 * that target membership is reordered and the source membership is removed in
 * the same transaction. source_list == target_list is a normal reorder. */
bool sr_event_db_move_event_between_lists(struct sr_event_db *db, uint64_t event_id, unsigned source_list,
					  unsigned target_list, int position);

/* Allocates an ordered event-id array with bmalloc/brealloc semantics. Caller
 * releases it with bfree(). An empty list returns true with *event_ids=NULL. */
bool sr_event_db_get_list_events(struct sr_event_db *db, unsigned list_id, uint64_t **event_ids, size_t *count);

#ifdef __cplusplus
}
#endif
