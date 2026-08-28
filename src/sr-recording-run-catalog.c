/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-recording-run-catalog.h"

#include <sqlite3.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <string.h>

static char *database_path(const char *session_dir)
{
	if (!session_dir || !*session_dir)
		return NULL;
	struct dstr path = {0};
	dstr_copy(&path, session_dir);
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, "session.sqlite");
	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

bool sr_recording_run_catalog_scan(const char *session_dir, struct sr_recording_run_record **runs, size_t *count)
{
	if (!runs || !count)
		return false;
	*runs = NULL;
	*count = 0;

	char *path = database_path(session_dir);
	if (!path)
		return false;

	sqlite3 *db = NULL;
	const int open_rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
	bfree(path);
	if (open_rc != SQLITE_OK) {
		if (db)
			sqlite3_close(db);
		return false;
	}
	sqlite3_busy_timeout(db, 5000);

	const char *query =
		"SELECT id,timeline_start_ns,timeline_end_ns,discontinuity FROM recording_runs "
		"WHERE timeline_end_ns IS NOT NULL AND timeline_end_ns >= timeline_start_ns ORDER BY id ASC";
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return false;
	}

	struct sr_recording_run_record *items = NULL;
	size_t item_count = 0;
	size_t capacity = 0;
	bool ok = true;
	for (;;) {
		const int rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE)
			break;
		if (rc != SQLITE_ROW) {
			ok = false;
			break;
		}

		const sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
		const sqlite3_int64 start = sqlite3_column_int64(stmt, 1);
		const sqlite3_int64 end = sqlite3_column_int64(stmt, 2);
		if (id <= 0 || start < 0 || end < start)
			continue;

		if (item_count == capacity) {
			const size_t next_capacity = capacity ? capacity * 2 : 16;
			struct sr_recording_run_record *next = brealloc(items, next_capacity * sizeof(*next));
			if (!next) {
				ok = false;
				break;
			}
			items = next;
			capacity = next_capacity;
		}

		struct sr_recording_run_record *item = &items[item_count++];
		memset(item, 0, sizeof(*item));
		item->id = (uint64_t)id;
		item->timeline_start_ns = (uint64_t)start;
		item->timeline_end_ns = (uint64_t)end;
		item->discontinuity = sqlite3_column_int(stmt, 3) != 0;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);
	if (!ok) {
		bfree(items);
		return false;
	}
	*runs = items;
	*count = item_count;
	return true;
}
