/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-event-db.h"

#include <obs-module.h>
#include <sqlite3.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct sr_event_db {
	sqlite3 *sql;
	pthread_mutex_t mutex;
	char *path;
	int schema_version;
};

static bool valid_u64(uint64_t value)
{
	return value <= (uint64_t)INT64_MAX;
}

static bool valid_list_id(unsigned list_id)
{
	return list_id >= 1 && list_id <= SR_EVENT_LIST_COUNT;
}

static bool valid_event(const struct sr_event_write *event)
{
	if (!event || !valid_u64(event->in_ns) || !valid_u64(event->out_ns) || event->out_ns < event->in_ns)
		return false;
	if (event->preferred_camera_id && !valid_u64(event->preferred_camera_id))
		return false;
	if (!isfinite(event->speed_percent) || event->speed_percent <= 0.0)
		return false;
	return event->audio_mode >= SR_EVENT_AUDIO_OFF && event->audio_mode <= SR_EVENT_AUDIO_CAMERA;
}

static void log_sql_error(struct sr_event_db *db, const char *context, int rc)
{
	blog(LOG_ERROR, "Sports Replay EventDB: %s failed (%d): %s", context, rc,
	     db && db->sql ? sqlite3_errmsg(db->sql) : "no database");
}

static bool exec_sql(struct sr_event_db *db, const char *sql, const char *context)
{
	char *message = NULL;
	const int rc = sqlite3_exec(db->sql, sql, NULL, NULL, &message);
	if (rc == SQLITE_OK)
		return true;

	blog(LOG_ERROR, "Sports Replay EventDB: %s failed (%d): %s", context, rc,
	     message ? message : sqlite3_errmsg(db->sql));
	sqlite3_free(message);
	return false;
}

static bool begin_transaction(struct sr_event_db *db)
{
	return exec_sql(db, "BEGIN IMMEDIATE", "begin transaction");
}

static bool commit_transaction(struct sr_event_db *db)
{
	return exec_sql(db, "COMMIT", "commit transaction");
}

static void rollback_transaction(struct sr_event_db *db)
{
	if (!exec_sql(db, "ROLLBACK", "rollback transaction"))
		blog(LOG_WARNING, "Sports Replay EventDB: rollback also failed");
}

static bool read_user_version(struct sr_event_db *db, int *version)
{
	sqlite3_stmt *stmt = NULL;
	const int prepare_rc = sqlite3_prepare_v2(db->sql, "PRAGMA user_version", -1, &stmt, NULL);
	if (prepare_rc != SQLITE_OK) {
		log_sql_error(db, "read schema version", prepare_rc);
		return false;
	}

	const int step_rc = sqlite3_step(stmt);
	if (step_rc != SQLITE_ROW) {
		log_sql_error(db, "step schema version", step_rc);
		sqlite3_finalize(stmt);
		return false;
	}

	*version = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return true;
}

static bool migrate_v1(struct sr_event_db *db)
{
	static const char *schema_sql =
		"CREATE TABLE IF NOT EXISTS cameras ("
		"id INTEGER PRIMARY KEY,"
		"stable_key TEXT NOT NULL UNIQUE,"
		"display_name TEXT NOT NULL,"
		"sync_offset_ns INTEGER NOT NULL DEFAULT 0,"
		"created_unix INTEGER NOT NULL DEFAULT (unixepoch())"
		");"
		"CREATE TABLE IF NOT EXISTS segments ("
		"id INTEGER PRIMARY KEY,"
		"camera_id INTEGER NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,"
		"sequence INTEGER NOT NULL,"
		"start_ns INTEGER NOT NULL,"
		"end_ns INTEGER NOT NULL,"
		"segment_path TEXT NOT NULL,"
		"index_path TEXT NOT NULL,"
		"flags INTEGER NOT NULL DEFAULT 0,"
		"active INTEGER NOT NULL DEFAULT 0,"
		"UNIQUE(camera_id, sequence),"
		"CHECK(start_ns >= 0),"
		"CHECK(end_ns >= start_ns)"
		");"
		"CREATE INDEX IF NOT EXISTS segments_camera_time ON segments(camera_id, start_ns, end_ns);"
		"CREATE TABLE IF NOT EXISTS events ("
		"id INTEGER PRIMARY KEY,"
		"in_ns INTEGER NOT NULL,"
		"out_ns INTEGER NOT NULL,"
		"preferred_camera_id INTEGER REFERENCES cameras(id) ON DELETE SET NULL,"
		"speed_percent REAL NOT NULL DEFAULT 100.0,"
		"audio_mode INTEGER NOT NULL DEFAULT 0,"
		"protected_event INTEGER NOT NULL DEFAULT 0,"
		"played INTEGER NOT NULL DEFAULT 0,"
		"pending INTEGER NOT NULL DEFAULT 0,"
		"name TEXT NOT NULL DEFAULT '',"
		"tag TEXT NOT NULL DEFAULT '',"
		"created_unix INTEGER NOT NULL DEFAULT (unixepoch()),"
		"updated_unix INTEGER NOT NULL DEFAULT (unixepoch()),"
		"CHECK(in_ns >= 0),"
		"CHECK(out_ns >= in_ns),"
		"CHECK(speed_percent > 0.0),"
		"CHECK(audio_mode BETWEEN 0 AND 2),"
		"CHECK(protected_event IN (0, 1)),"
		"CHECK(played IN (0, 1)),"
		"CHECK(pending IN (0, 1))"
		");"
		"CREATE INDEX IF NOT EXISTS events_time ON events(in_ns, out_ns);"
		"CREATE TABLE IF NOT EXISTS event_lists ("
		"id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND 20),"
		"name TEXT NOT NULL"
		");"
		"CREATE TABLE IF NOT EXISTS event_list_items ("
		"list_id INTEGER NOT NULL REFERENCES event_lists(id) ON DELETE CASCADE,"
		"event_id INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,"
		"position INTEGER NOT NULL CHECK(position >= 0),"
		"PRIMARY KEY(list_id, event_id)"
		");"
		"CREATE INDEX IF NOT EXISTS event_list_order ON event_list_items(list_id, position, event_id);"
		"CREATE VIEW IF NOT EXISTS event_camera_coverage AS "
		"SELECT e.id AS event_id, s.camera_id AS camera_id "
		"FROM events AS e JOIN segments AS s ON s.start_ns <= e.out_ns AND s.end_ns >= e.in_ns "
		"GROUP BY e.id, s.camera_id;"
		"WITH RECURSIVE list_numbers(id) AS ("
		"VALUES(1) UNION ALL SELECT id + 1 FROM list_numbers WHERE id < 20"
		") INSERT OR IGNORE INTO event_lists(id, name) "
		"SELECT id, printf('List %d', id) FROM list_numbers;";

	if (!begin_transaction(db))
		return false;
	if (!exec_sql(db, schema_sql, "create schema v1") || !exec_sql(db, "PRAGMA user_version=1", "set schema v1")) {
		rollback_transaction(db);
		return false;
	}
	if (!commit_transaction(db)) {
		rollback_transaction(db);
		return false;
	}
	return true;
}

static bool migrate_v2(struct sr_event_db *db)
{
	static const char *migration_sql =
		"DROP VIEW IF EXISTS event_camera_coverage;"
		"CREATE VIEW IF NOT EXISTS event_segment_overlap AS "
		"SELECT e.id AS event_id, s.camera_id AS camera_id "
		"FROM events AS e JOIN segments AS s ON s.start_ns <= e.out_ns AND s.end_ns >= e.in_ns "
		"GROUP BY e.id, s.camera_id;";

	if (!begin_transaction(db))
		return false;
	if (!exec_sql(db, migration_sql, "migrate schema v2") ||
	    !exec_sql(db, "PRAGMA user_version=2", "set schema v2")) {
		rollback_transaction(db);
		return false;
	}
	if (!commit_transaction(db)) {
		rollback_transaction(db);
		return false;
	}
	return true;
}

static bool migrate_schema(struct sr_event_db *db)
{
	int version = 0;
	if (!read_user_version(db, &version))
		return false;
	if (version > SR_EVENT_DB_SCHEMA_VERSION) {
		blog(LOG_ERROR, "Sports Replay EventDB: database schema %d is newer than supported schema %d", version,
		     SR_EVENT_DB_SCHEMA_VERSION);
		return false;
	}

	if (version == 0) {
		if (!migrate_v1(db))
			return false;
		version = 1;
	}

	if (version == 1) {
		if (!migrate_v2(db))
			return false;
		version = 2;
	}

	db->schema_version = version;
	return true;
}

struct sr_event_db *sr_event_db_open(const char *session_dir)
{
	if (!session_dir || !*session_dir)
		return NULL;

	struct sr_event_db *db = bzalloc(sizeof(*db));
	pthread_mutex_init(&db->mutex, NULL);

	struct dstr path = {0};
	dstr_copy(&path, session_dir);
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, "session.sqlite");
	db->path = bstrdup(path.array);
	dstr_free(&path);

	const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
	const int open_rc = sqlite3_open_v2(db->path, &db->sql, flags, NULL);
	if (open_rc != SQLITE_OK) {
		log_sql_error(db, "open database", open_rc);
		sr_event_db_close(db);
		return NULL;
	}

	sqlite3_extended_result_codes(db->sql, 1);
	sqlite3_busy_timeout(db->sql, 5000);

	if (!exec_sql(db, "PRAGMA foreign_keys=ON", "enable foreign keys") ||
	    !exec_sql(db, "PRAGMA journal_mode=WAL", "enable WAL") ||
	    !exec_sql(db, "PRAGMA synchronous=NORMAL", "set synchronous mode") || !migrate_schema(db)) {
		sr_event_db_close(db);
		return NULL;
	}

	blog(LOG_INFO, "Sports Replay EventDB: opened schema %d at '%s'", db->schema_version, db->path);
	return db;
}

void sr_event_db_close(struct sr_event_db *db)
{
	if (!db)
		return;

	if (db->sql) {
		const int rc = sqlite3_close(db->sql);
		if (rc != SQLITE_OK)
			blog(LOG_WARNING, "Sports Replay EventDB: close failed (%d): %s", rc, sqlite3_errmsg(db->sql));
		db->sql = NULL;
	}
	bfree(db->path);
	pthread_mutex_destroy(&db->mutex);
	bfree(db);
}

int sr_event_db_schema_version(struct sr_event_db *db)
{
	if (!db)
		return 0;
	pthread_mutex_lock(&db->mutex);
	const int version = db->schema_version;
	pthread_mutex_unlock(&db->mutex);
	return version;
}

void sr_camera_record_free(struct sr_camera_record *camera)
{
	if (!camera)
		return;
	bfree(camera->stable_key);
	bfree(camera->display_name);
	memset(camera, 0, sizeof(*camera));
}

bool sr_event_db_upsert_camera(struct sr_event_db *db, const char *stable_key, const char *display_name,
			       int64_t sync_offset_ns, uint64_t *camera_id)
{
	if (!db || !stable_key || !*stable_key || !display_name || !*display_name)
		return false;

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	const char *sql = "INSERT INTO cameras(stable_key,display_name,sync_offset_ns) VALUES(?,?,?) "
			  "ON CONFLICT(stable_key) DO UPDATE SET display_name=excluded.display_name,"
			  "sync_offset_ns=excluded.sync_offset_ns RETURNING id";
	int rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_text(stmt, 1, stable_key, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
		  sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)sync_offset_ns) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_ROW;
	}
	if (ok && camera_id)
		*camera_id = (uint64_t)sqlite3_column_int64(stmt, 0);
	if (!ok) {
		if (rc == SQLITE_OK)
			rc = sqlite3_errcode(db->sql);
		log_sql_error(db, "upsert camera", rc);
	}
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_get_camera(struct sr_event_db *db, uint64_t camera_id, struct sr_camera_record *camera)
{
	if (!db || !camera || !camera_id || !valid_u64(camera_id))
		return false;
	memset(camera, 0, sizeof(*camera));

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	const char *sql = "SELECT id,stable_key,display_name,sync_offset_ns FROM cameras WHERE id=?";
	int rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int64(stmt, 1, (sqlite3_int64)camera_id) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_ROW;
	}
	if (ok) {
		camera->id = (uint64_t)sqlite3_column_int64(stmt, 0);
		const unsigned char *stable_key = sqlite3_column_text(stmt, 1);
		const unsigned char *display_name = sqlite3_column_text(stmt, 2);
		camera->stable_key = bstrdup(stable_key ? (const char *)stable_key : "");
		camera->display_name = bstrdup(display_name ? (const char *)display_name : "");
		camera->sync_offset_ns = (int64_t)sqlite3_column_int64(stmt, 3);
		ok = camera->stable_key && *camera->stable_key && camera->display_name && *camera->display_name;
	}
	if (!ok && rc != SQLITE_DONE && rc != SQLITE_ROW) {
		if (rc == SQLITE_OK)
			rc = sqlite3_errcode(db->sql);
		log_sql_error(db, "get camera", rc);
	}
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);

	if (!ok)
		sr_camera_record_free(camera);
	return ok;
}

static bool bind_event(sqlite3_stmt *stmt, int first_parameter, const struct sr_event_write *event)
{
	int index = first_parameter;
	if (sqlite3_bind_int64(stmt, index++, (sqlite3_int64)event->in_ns) != SQLITE_OK ||
	    sqlite3_bind_int64(stmt, index++, (sqlite3_int64)event->out_ns) != SQLITE_OK)
		return false;

	int rc;
	if (event->preferred_camera_id)
		rc = sqlite3_bind_int64(stmt, index++, (sqlite3_int64)event->preferred_camera_id);
	else
		rc = sqlite3_bind_null(stmt, index++);
	if (rc != SQLITE_OK || sqlite3_bind_double(stmt, index++, event->speed_percent) != SQLITE_OK ||
	    sqlite3_bind_int(stmt, index++, event->audio_mode) != SQLITE_OK ||
	    sqlite3_bind_int(stmt, index++, event->protected_event ? 1 : 0) != SQLITE_OK ||
	    sqlite3_bind_int(stmt, index++, event->played ? 1 : 0) != SQLITE_OK ||
	    sqlite3_bind_int(stmt, index++, event->pending ? 1 : 0) != SQLITE_OK ||
	    sqlite3_bind_text(stmt, index++, event->name ? event->name : "", -1, SQLITE_TRANSIENT) != SQLITE_OK ||
	    sqlite3_bind_text(stmt, index, event->tag ? event->tag : "", -1, SQLITE_TRANSIENT) != SQLITE_OK)
		return false;
	return true;
}

bool sr_event_db_create_event(struct sr_event_db *db, const struct sr_event_write *event, uint64_t *event_id)
{
	if (!db || !valid_event(event))
		return false;

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	const char *sql =
		"INSERT INTO events(in_ns,out_ns,preferred_camera_id,speed_percent,audio_mode,protected_event,played,pending,name,tag) "
		"VALUES(?,?,?,?,?,?,?,?,?,?)";
	int rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && bind_event(stmt, 1, event);
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_DONE;
	}
	if (!ok)
		log_sql_error(db, "create event", rc);
	if (ok && event_id)
		*event_id = (uint64_t)sqlite3_last_insert_rowid(db->sql);
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_get_event(struct sr_event_db *db, uint64_t event_id, struct sr_event_record *event)
{
	if (!db || !event || !event_id || !valid_u64(event_id))
		return false;
	memset(event, 0, sizeof(*event));

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	const char *sql =
		"SELECT id,in_ns,out_ns,preferred_camera_id,speed_percent,audio_mode,protected_event,played,pending,name,tag "
		"FROM events WHERE id=?";
	int rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_ROW;
	}
	if (ok) {
		event->id = (uint64_t)sqlite3_column_int64(stmt, 0);
		event->in_ns = (uint64_t)sqlite3_column_int64(stmt, 1);
		event->out_ns = (uint64_t)sqlite3_column_int64(stmt, 2);
		event->preferred_camera_id =
			sqlite3_column_type(stmt, 3) == SQLITE_NULL ? 0 : (uint64_t)sqlite3_column_int64(stmt, 3);
		event->speed_percent = sqlite3_column_double(stmt, 4);
		event->audio_mode = sqlite3_column_int(stmt, 5);
		event->protected_event = sqlite3_column_int(stmt, 6) != 0;
		event->played = sqlite3_column_int(stmt, 7) != 0;
		event->pending = sqlite3_column_int(stmt, 8) != 0;
		const unsigned char *name = sqlite3_column_text(stmt, 9);
		const unsigned char *tag = sqlite3_column_text(stmt, 10);
		event->name = bstrdup(name ? (const char *)name : "");
		event->tag = bstrdup(tag ? (const char *)tag : "");
		ok = event->name && event->tag;
	}
	if (!ok && rc != SQLITE_DONE && rc != SQLITE_ROW)
		log_sql_error(db, "get event", rc);
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);

	if (!ok)
		sr_event_record_free(event);
	return ok;
}

bool sr_event_db_update_event(struct sr_event_db *db, uint64_t event_id, const struct sr_event_write *event)
{
	if (!db || !event_id || !valid_u64(event_id) || !valid_event(event))
		return false;

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	const char *sql =
		"UPDATE events SET in_ns=?,out_ns=?,preferred_camera_id=?,speed_percent=?,audio_mode=?,protected_event=?,"
		"played=?,pending=?,name=?,tag=?,updated_unix=unixepoch() WHERE id=?";
	int rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && bind_event(stmt, 1, event) &&
		  sqlite3_bind_int64(stmt, 11, (sqlite3_int64)event_id) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_DONE && sqlite3_changes(db->sql) == 1;
	}
	if (!ok)
		log_sql_error(db, "update event", rc);
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

void sr_event_record_free(struct sr_event_record *event)
{
	if (!event)
		return;
	bfree(event->name);
	bfree(event->tag);
	memset(event, 0, sizeof(*event));
}

static bool list_position_locked(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int *position,
				 bool *found)
{
	sqlite3_stmt *stmt = NULL;
	const int prepare_rc = sqlite3_prepare_v2(
		db->sql, "SELECT position FROM event_list_items WHERE list_id=? AND event_id=?", -1, &stmt, NULL);
	if (prepare_rc != SQLITE_OK) {
		log_sql_error(db, "prepare list position", prepare_rc);
		return false;
	}

	bool ok = sqlite3_bind_int(stmt, 1, (int)list_id) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id) == SQLITE_OK;
	int rc = ok ? sqlite3_step(stmt) : SQLITE_ERROR;
	if (rc == SQLITE_ROW) {
		*found = true;
		*position = sqlite3_column_int(stmt, 0);
	} else if (rc == SQLITE_DONE) {
		*found = false;
		*position = -1;
	} else {
		ok = false;
		log_sql_error(db, "read list position", rc);
	}
	sqlite3_finalize(stmt);
	return ok;
}

static bool event_exists_locked(struct sr_event_db *db, uint64_t event_id)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db->sql, "SELECT 1 FROM events WHERE id=?", -1, &stmt, NULL) != SQLITE_OK)
		return false;
	if (sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id) != SQLITE_OK) {
		sqlite3_finalize(stmt);
		return false;
	}
	const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
	sqlite3_finalize(stmt);
	return exists;
}

static bool list_count_locked(struct sr_event_db *db, unsigned list_id, int *count)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->sql, "SELECT COUNT(*) FROM event_list_items WHERE list_id=?", -1, &stmt, NULL);
	if (rc != SQLITE_OK || sqlite3_bind_int(stmt, 1, (int)list_id) != SQLITE_OK) {
		log_sql_error(db, "prepare list count", rc);
		sqlite3_finalize(stmt);
		return false;
	}
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_ROW) {
		log_sql_error(db, "read list count", rc);
		sqlite3_finalize(stmt);
		return false;
	}
	*count = sqlite3_column_int(stmt, 0);
	sqlite3_finalize(stmt);
	return true;
}

static bool shift_positions_locked(struct sr_event_db *db, unsigned list_id, int delta, int lower, bool lower_inclusive,
				   int upper, bool have_upper, bool upper_inclusive)
{
	const char *lower_op = lower_inclusive ? ">=" : ">";
	const char *upper_op = upper_inclusive ? "<=" : "<";
	char sql[256];
	if (have_upper) {
		snprintf(
			sql, sizeof(sql),
			"UPDATE event_list_items SET position=position+? WHERE list_id=? AND position %s ? AND position %s ?",
			lower_op, upper_op);
	} else {
		snprintf(sql, sizeof(sql),
			 "UPDATE event_list_items SET position=position+? WHERE list_id=? AND position %s ?", lower_op);
	}

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->sql, sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, delta) == SQLITE_OK &&
		  sqlite3_bind_int(stmt, 2, (int)list_id) == SQLITE_OK && sqlite3_bind_int(stmt, 3, lower) == SQLITE_OK;
	if (ok && have_upper)
		ok = sqlite3_bind_int(stmt, 4, upper) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_DONE;
	}
	if (!ok)
		log_sql_error(db, "shift list positions", rc);
	sqlite3_finalize(stmt);
	return ok;
}

static bool set_position_row_locked(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int position)
{
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->sql, "UPDATE event_list_items SET position=? WHERE list_id=? AND event_id=?",
				    -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, position) == SQLITE_OK &&
		  sqlite3_bind_int(stmt, 2, (int)list_id) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)event_id) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_DONE && sqlite3_changes(db->sql) == 1;
	}
	if (!ok)
		log_sql_error(db, "set event position", rc);
	sqlite3_finalize(stmt);
	return ok;
}

static bool reorder_existing_locked(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int old_position,
				    int new_position)
{
	int count = 0;
	if (!list_count_locked(db, list_id, &count) || count <= 0)
		return false;
	if (new_position < 0)
		new_position = count - 1;
	if (new_position >= count)
		new_position = count - 1;
	if (old_position == new_position)
		return true;

	bool ok;
	if (old_position < new_position)
		ok = shift_positions_locked(db, list_id, -1, old_position, false, new_position, true, true);
	else
		ok = shift_positions_locked(db, list_id, 1, new_position, true, old_position, true, false);
	return ok && set_position_row_locked(db, list_id, event_id, new_position);
}

bool sr_event_db_add_event_to_list(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int position)
{
	if (!db || !valid_list_id(list_id) || !event_id || !valid_u64(event_id))
		return false;

	pthread_mutex_lock(&db->mutex);
	if (!event_exists_locked(db, event_id) || !begin_transaction(db)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	int old_position = -1;
	bool found = false;
	bool ok = list_position_locked(db, list_id, event_id, &old_position, &found);
	if (ok && found) {
		ok = reorder_existing_locked(db, list_id, event_id, old_position, position);
	} else if (ok) {
		int count = 0;
		ok = list_count_locked(db, list_id, &count);
		if (ok) {
			if (position < 0 || position > count)
				position = count;
			ok = shift_positions_locked(db, list_id, 1, position, true, 0, false, false);
		}
		if (ok) {
			sqlite3_stmt *stmt = NULL;
			int rc = sqlite3_prepare_v2(
				db->sql, "INSERT INTO event_list_items(list_id,event_id,position) VALUES(?,?,?)", -1,
				&stmt, NULL);
			ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, (int)list_id) == SQLITE_OK &&
			     sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id) == SQLITE_OK &&
			     sqlite3_bind_int(stmt, 3, position) == SQLITE_OK;
			if (ok) {
				rc = sqlite3_step(stmt);
				ok = rc == SQLITE_DONE;
			}
			if (!ok)
				log_sql_error(db, "add event to list", rc);
			sqlite3_finalize(stmt);
		}
	}

	if (ok)
		ok = commit_transaction(db);
	else
		rollback_transaction(db);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_remove_event_from_list(struct sr_event_db *db, unsigned list_id, uint64_t event_id)
{
	if (!db || !valid_list_id(list_id) || !event_id || !valid_u64(event_id))
		return false;

	pthread_mutex_lock(&db->mutex);
	int old_position = -1;
	bool found = false;
	if (!list_position_locked(db, list_id, event_id, &old_position, &found)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}
	if (!found) {
		pthread_mutex_unlock(&db->mutex);
		return true;
	}
	if (!begin_transaction(db)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->sql, "DELETE FROM event_list_items WHERE list_id=? AND event_id=?", -1, &stmt,
				    NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, (int)list_id) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_DONE;
	}
	sqlite3_finalize(stmt);
	if (ok)
		ok = shift_positions_locked(db, list_id, -1, old_position, false, 0, false, false);
	if (ok)
		ok = commit_transaction(db);
	else
		rollback_transaction(db);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_set_event_position(struct sr_event_db *db, unsigned list_id, uint64_t event_id, int position)
{
	if (!db || !valid_list_id(list_id) || !event_id || !valid_u64(event_id))
		return false;

	pthread_mutex_lock(&db->mutex);
	int old_position = -1;
	bool found = false;
	if (!list_position_locked(db, list_id, event_id, &old_position, &found) || !found) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}
	if (!begin_transaction(db)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	bool ok = reorder_existing_locked(db, list_id, event_id, old_position, position);
	if (ok)
		ok = commit_transaction(db);
	else
		rollback_transaction(db);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_delete_event(struct sr_event_db *db, uint64_t event_id)
{
	if (!db || !event_id || !valid_u64(event_id))
		return false;

	pthread_mutex_lock(&db->mutex);
	if (!begin_transaction(db)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	unsigned lists[SR_EVENT_LIST_COUNT];
	int positions[SR_EVENT_LIST_COUNT];
	size_t membership_count = 0;
	sqlite3_stmt *members = NULL;
	int rc = sqlite3_prepare_v2(db->sql,
				    "SELECT list_id,position FROM event_list_items WHERE event_id=? ORDER BY list_id",
				    -1, &members, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int64(members, 1, (sqlite3_int64)event_id) == SQLITE_OK;
	while (ok && (rc = sqlite3_step(members)) == SQLITE_ROW) {
		if (membership_count >= SR_EVENT_LIST_COUNT) {
			ok = false;
			break;
		}
		lists[membership_count] = (unsigned)sqlite3_column_int(members, 0);
		positions[membership_count] = sqlite3_column_int(members, 1);
		membership_count++;
	}
	if (ok && rc != SQLITE_DONE)
		ok = false;
	sqlite3_finalize(members);

	if (ok) {
		sqlite3_stmt *stmt = NULL;
		rc = sqlite3_prepare_v2(db->sql, "DELETE FROM events WHERE id=?", -1, &stmt, NULL);
		ok = rc == SQLITE_OK && sqlite3_bind_int64(stmt, 1, (sqlite3_int64)event_id) == SQLITE_OK;
		if (ok) {
			rc = sqlite3_step(stmt);
			ok = rc == SQLITE_DONE && sqlite3_changes(db->sql) == 1;
		}
		sqlite3_finalize(stmt);
	}

	for (size_t i = 0; ok && i < membership_count; i++)
		ok = shift_positions_locked(db, lists[i], -1, positions[i], false, 0, false, false);

	if (ok)
		ok = commit_transaction(db);
	else
		rollback_transaction(db);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_create_event_in_list(struct sr_event_db *db, const struct sr_event_write *event, unsigned list_id,
				      int position, uint64_t *event_id)
{
	if (!db || !valid_event(event) || !valid_list_id(list_id))
		return false;

	pthread_mutex_lock(&db->mutex);
	if (!begin_transaction(db)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	sqlite3_stmt *stmt = NULL;
	const char *event_sql =
		"INSERT INTO events(in_ns,out_ns,preferred_camera_id,speed_percent,audio_mode,protected_event,played,pending,name,tag) "
		"VALUES(?,?,?,?,?,?,?,?,?,?)";
	int rc = sqlite3_prepare_v2(db->sql, event_sql, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && bind_event(stmt, 1, event);
	if (ok) {
		rc = sqlite3_step(stmt);
		ok = rc == SQLITE_DONE;
	}
	if (!ok)
		log_sql_error(db, "create event in list", rc);
	sqlite3_finalize(stmt);

	uint64_t id = 0;
	if (ok) {
		id = (uint64_t)sqlite3_last_insert_rowid(db->sql);
		int count = 0;
		ok = list_count_locked(db, list_id, &count);
		if (ok) {
			if (position < 0 || position > count)
				position = count;
			ok = shift_positions_locked(db, list_id, 1, position, true, 0, false, false);
		}
	}

	if (ok) {
		stmt = NULL;
		rc = sqlite3_prepare_v2(db->sql,
					"INSERT INTO event_list_items(list_id,event_id,position) VALUES(?,?,?)", -1,
					&stmt, NULL);
		ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, (int)list_id) == SQLITE_OK &&
		     sqlite3_bind_int64(stmt, 2, (sqlite3_int64)id) == SQLITE_OK &&
		     sqlite3_bind_int(stmt, 3, position) == SQLITE_OK;
		if (ok) {
			rc = sqlite3_step(stmt);
			ok = rc == SQLITE_DONE;
		}
		if (!ok)
			log_sql_error(db, "insert first event list membership", rc);
		sqlite3_finalize(stmt);
	}

	if (ok)
		ok = commit_transaction(db);
	else
		rollback_transaction(db);
	if (ok && event_id)
		*event_id = id;
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_move_event_between_lists(struct sr_event_db *db, uint64_t event_id, unsigned source_list,
					  unsigned target_list, int position)
{
	if (!db || !event_id || !valid_u64(event_id) || !valid_list_id(source_list) || !valid_list_id(target_list))
		return false;

	pthread_mutex_lock(&db->mutex);
	if (!event_exists_locked(db, event_id)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	int source_position = -1;
	bool source_found = false;
	if (!list_position_locked(db, source_list, event_id, &source_position, &source_found) || !source_found) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}
	if (!begin_transaction(db)) {
		pthread_mutex_unlock(&db->mutex);
		return false;
	}

	bool ok = true;
	if (source_list == target_list) {
		ok = reorder_existing_locked(db, source_list, event_id, source_position, position);
	} else {
		int target_position = -1;
		bool target_found = false;
		ok = list_position_locked(db, target_list, event_id, &target_position, &target_found);
		if (ok && target_found) {
			ok = reorder_existing_locked(db, target_list, event_id, target_position, position);
		} else if (ok) {
			int count = 0;
			ok = list_count_locked(db, target_list, &count);
			if (ok) {
				if (position < 0 || position > count)
					position = count;
				ok = shift_positions_locked(db, target_list, 1, position, true, 0, false, false);
			}
			if (ok) {
				sqlite3_stmt *stmt = NULL;
				int rc = sqlite3_prepare_v2(
					db->sql,
					"INSERT INTO event_list_items(list_id,event_id,position) VALUES(?,?,?)", -1,
					&stmt, NULL);
				ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, (int)target_list) == SQLITE_OK &&
				     sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id) == SQLITE_OK &&
				     sqlite3_bind_int(stmt, 3, position) == SQLITE_OK;
				if (ok) {
					rc = sqlite3_step(stmt);
					ok = rc == SQLITE_DONE;
				}
				if (!ok)
					log_sql_error(db, "move event: insert target membership", rc);
				sqlite3_finalize(stmt);
			}
		}

		if (ok) {
			sqlite3_stmt *stmt = NULL;
			int rc = sqlite3_prepare_v2(db->sql,
						    "DELETE FROM event_list_items WHERE list_id=? AND event_id=?", -1,
						    &stmt, NULL);
			ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, (int)source_list) == SQLITE_OK &&
			     sqlite3_bind_int64(stmt, 2, (sqlite3_int64)event_id) == SQLITE_OK;
			if (ok) {
				rc = sqlite3_step(stmt);
				ok = rc == SQLITE_DONE && sqlite3_changes(db->sql) == 1;
			}
			if (!ok)
				log_sql_error(db, "move event: remove source membership", rc);
			sqlite3_finalize(stmt);
		}
		if (ok)
			ok = shift_positions_locked(db, source_list, -1, source_position, false, 0, false, false);
	}

	if (ok)
		ok = commit_transaction(db);
	else
		rollback_transaction(db);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_has_event_overlap(struct sr_event_db *db, uint64_t start_ns, uint64_t end_ns, bool *overlap)
{
	if (!db || !overlap || end_ns < start_ns || !valid_u64(start_ns) || !valid_u64(end_ns))
		return false;
	*overlap = true;

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->sql, "SELECT 1 FROM events WHERE in_ns<=? AND out_ns>=? LIMIT 1", -1, &stmt,
				    NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int64(stmt, 1, (sqlite3_int64)end_ns) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)start_ns) == SQLITE_OK;
	if (ok) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_ROW)
			*overlap = true;
		else if (rc == SQLITE_DONE)
			*overlap = false;
		else
			ok = false;
	}
	if (!ok)
		log_sql_error(db, "query Event overlap", rc);
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);
	return ok;
}

bool sr_event_db_get_list_events(struct sr_event_db *db, unsigned list_id, uint64_t **event_ids, size_t *count)
{
	if (!db || !event_ids || !count || !valid_list_id(list_id))
		return false;
	*event_ids = NULL;
	*count = 0;

	pthread_mutex_lock(&db->mutex);
	sqlite3_stmt *stmt = NULL;
	int rc = sqlite3_prepare_v2(db->sql,
				    "SELECT event_id FROM event_list_items WHERE list_id=? ORDER BY position,event_id",
				    -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int(stmt, 1, (int)list_id) == SQLITE_OK;

	uint64_t *items = NULL;
	size_t item_count = 0;
	size_t capacity = 0;
	while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
		if (item_count == capacity) {
			const size_t next_capacity = capacity ? capacity * 2 : 32;
			if (next_capacity < capacity || next_capacity > SIZE_MAX / sizeof(*items)) {
				ok = false;
				break;
			}
			uint64_t *next = brealloc(items, next_capacity * sizeof(*items));
			if (!next) {
				ok = false;
				break;
			}
			items = next;
			capacity = next_capacity;
		}
		items[item_count++] = (uint64_t)sqlite3_column_int64(stmt, 0);
	}
	if (ok && rc != SQLITE_DONE)
		ok = false;
	if (!ok)
		log_sql_error(db, "read event list", rc);
	sqlite3_finalize(stmt);
	pthread_mutex_unlock(&db->mutex);

	if (!ok) {
		bfree(items);
		return false;
	}
	*event_ids = items;
	*count = item_count;
	return true;
}
