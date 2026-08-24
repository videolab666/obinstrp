from pathlib import Path

path = Path("src/sr-event-db.c")
text = path.read_text(encoding="utf-8")
marker = "static bool bind_event(sqlite3_stmt *stmt, int first_parameter, const struct sr_event_write *event)\n"
if "bool sr_event_db_upsert_camera(" not in text:
    if marker not in text:
        raise SystemExit("bind_event marker not found")
    block = r'''void sr_camera_record_free(struct sr_camera_record *camera)
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
	const char *sql =
		"INSERT INTO cameras(stable_key,display_name,sync_offset_ns) VALUES(?,?,?) "
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

'''
    text = text.replace(marker, block + marker, 1)
    path.write_text(text, encoding="utf-8")
print("camera registry implementation applied")
