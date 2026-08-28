/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-session.h"

#include "sr-audio-format.h"
#include "sr-camera-identity.h"
#include "sr-config.h"
#include "sr-segment-format.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <sqlite3.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SR_SESSION_FORMAT_VERSION 2
#define SR_SESSION_MANAGER_FORMAT_VERSION 1

static pthread_mutex_t g_session_mutex;
static char *g_opened_path;
static char *g_opened_id;
static char *g_record_target_path;
static char *g_recording_path;
static uint64_t g_recording_obs_start_ns;
static uint64_t g_recording_timeline_start_ns;
static uint64_t g_recording_run_id;
static uint64_t g_recording_generation;
static bool g_recording_discontinuity;

static bool same_path(const char *a, const char *b)
{
	if (!a || !b)
		return false;
#ifdef _WIN32
	return _stricmp(a, b) == 0;
#else
	return strcmp(a, b) == 0;
#endif
}

static char *join_path(const char *dir, const char *tail)
{
	if (!dir || !*dir)
		return NULL;
	struct dstr path = {0};
	dstr_copy(&path, dir);
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, tail ? tail : "");
	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

static bool session_valid(const char *path)
{
	char *metadata = join_path(path, "session.json");
	const bool valid = metadata && os_file_exists(metadata);
	bfree(metadata);
	return valid;
}

static void uuid_short_id(const char *uuid, char out[9])
{
	size_t n = 0;
	if (uuid) {
		for (const char *p = uuid; *p && n < 8; p++) {
			if (*p != '-')
				out[n++] = *p;
		}
	}
	while (n < 8)
		out[n++] = '0';
	out[8] = '\0';
}

static bool read_metadata(const char *dir, char **session_id, char **display_name, time_t *created)
{
	if (session_id)
		*session_id = NULL;
	if (display_name)
		*display_name = NULL;
	if (created)
		*created = 0;

	char *path = join_path(dir, "session.json");
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);
	if (!data)
		return false;

	const char *id = obs_data_get_string(data, "session_id");
	const char *name = obs_data_get_string(data, "display_name");
	const int64_t created_value = obs_data_get_int(data, "created_unix");
	if (session_id)
		*session_id = bstrdup(id && *id ? id : "");
	if (display_name)
		*display_name = bstrdup(name && *name ? name : "");
	if (created)
		*created = (time_t)created_value;
	obs_data_release(data);
	return true;
}

static bool write_session_metadata(const char *dir, const char *session_id, const char *display_name, time_t created)
{
	char *path = join_path(dir, "session.json");
	if (!path)
		return false;

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "format_version", SR_SESSION_FORMAT_VERSION);
	obs_data_set_string(data, "session_id", session_id ? session_id : "");
	obs_data_set_string(data, "display_name", display_name ? display_name : "");
	obs_data_set_int(data, "created_unix", (long long)created);
	obs_data_set_string(data, "plugin_version", PLUGIN_VERSION);
	obs_data_set_string(data, "timebase", "nanoseconds");

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		obs_data_set_int(data, "obs_fps_num", ovi.fps_num);
		obs_data_set_int(data, "obs_fps_den", ovi.fps_den);
	}
	const bool ok = obs_data_save_json(data, path);
	obs_data_release(data);
	bfree(path);
	return ok;
}

static char *manager_config_path(void)
{
	return obs_module_config_path("standalone-v1/session-manager.json");
}

static void persist_last_opened_locked(void)
{
	char *dir = obs_module_config_path("standalone-v1");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = manager_config_path();
	if (!path)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "format_version", SR_SESSION_MANAGER_FORMAT_VERSION);
	obs_data_set_string(data, "last_opened_path", g_opened_path ? g_opened_path : "");
	obs_data_save_json(data, path);
	obs_data_release(data);
	bfree(path);
}

static char *load_last_opened(void)
{
	char *path = manager_config_path();
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);
	if (!data)
		return NULL;
	const char *saved = obs_data_get_string(data, "last_opened_path");
	char *result = saved && *saved ? bstrdup(saved) : NULL;
	obs_data_release(data);
	return result;
}

static char *find_latest_session(void)
{
	char *root = sr_config_get_session_root();
	if (!root || !*root) {
		bfree(root);
		return NULL;
	}
	char *pattern = join_path(root, "*/session.json");
	bfree(root);
	if (!pattern)
		return NULL;

	char *best = NULL;
	os_glob_t *glob = NULL;
	if (os_glob(pattern, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;
			const char *metadata = glob->gl_pathv[i].path;
			const size_t len = strlen(metadata);
			const size_t suffix = strlen("/session.json");
			if (len <= suffix)
				continue;
			char *candidate = bstrdup(metadata);
			candidate[len - suffix] = '\0';
			if (!best || strcmp(candidate, best) > 0) {
				bfree(best);
				best = candidate;
			} else {
				bfree(candidate);
			}
		}
		os_globfree(glob);
	}
	bfree(pattern);
	return best;
}

static bool set_opened_locked(const char *path)
{
	if (!path || !*path || !session_valid(path))
		return false;
	char *id = NULL;
	if (!read_metadata(path, &id, NULL, NULL))
		return false;
	bfree(g_opened_path);
	bfree(g_opened_id);
	g_opened_path = bstrdup(path);
	g_opened_id = id;
	persist_last_opened_locked();
	return g_opened_path != NULL;
}

static sqlite3 *open_session_sqlite(const char *session_dir)
{
	char *path = join_path(session_dir, "session.sqlite");
	if (!path)
		return NULL;
	sqlite3 *sql = NULL;
	const int rc =
		sqlite3_open_v2(path, &sql, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
	bfree(path);
	if (rc != SQLITE_OK) {
		if (sql)
			sqlite3_close(sql);
		return NULL;
	}
	sqlite3_busy_timeout(sql, 5000);
	char *error = NULL;
	const char *schema =
		"PRAGMA foreign_keys=ON;"
		"CREATE TABLE IF NOT EXISTS cameras ("
		"id INTEGER PRIMARY KEY,stable_key TEXT NOT NULL UNIQUE,display_name TEXT NOT NULL,"
		"sync_offset_ns INTEGER NOT NULL DEFAULT 0,created_unix INTEGER NOT NULL DEFAULT (unixepoch()));"
		"CREATE TABLE IF NOT EXISTS recording_runs ("
		"id INTEGER PRIMARY KEY,started_unix INTEGER NOT NULL,ended_unix INTEGER,"
		"timeline_start_ns INTEGER NOT NULL,timeline_end_ns INTEGER,"
		"obs_start_ns INTEGER NOT NULL,obs_end_ns INTEGER,discontinuity INTEGER NOT NULL DEFAULT 0);"
		"CREATE INDEX IF NOT EXISTS recording_runs_timeline ON recording_runs(timeline_start_ns,timeline_end_ns);";
	if (sqlite3_exec(sql, schema, NULL, NULL, &error) != SQLITE_OK) {
		blog(LOG_ERROR, "Pitel Instant Replay: session database setup failed: %s", error ? error : "unknown");
		sqlite3_free(error);
		sqlite3_close(sql);
		return NULL;
	}
	return sql;
}

#define SR_INDEX_MAX_SEGMENT_SPAN_NS (10ULL * 60ULL * 1000000000ULL)
#define SR_AUDIO_VIDEO_SLOP_NS (10ULL * 1000000000ULL)

struct sr_media_bounds {
	bool have;
	uint64_t start_ns;
	uint64_t end_ns;
};

static void media_bounds_add(struct sr_media_bounds *bounds, uint64_t start_ns, uint64_t end_ns)
{
	if (!bounds || end_ns < start_ns)
		return;
	if (!bounds->have) {
		bounds->have = true;
		bounds->start_ns = start_ns;
		bounds->end_ns = end_ns;
		return;
	}
	if (start_ns < bounds->start_ns)
		bounds->start_ns = start_ns;
	if (end_ns > bounds->end_ns)
		bounds->end_ns = end_ns;
}

static bool valid_segment_timestamp(uint64_t segment_start_ns, uint64_t timestamp_ns)
{
	return timestamp_ns >= segment_start_ns && timestamp_ns - segment_start_ns <= SR_INDEX_MAX_SEGMENT_SPAN_NS;
}

static bool scan_video_index_file(const char *path, struct sr_media_bounds *bounds)
{
	FILE *file = os_fopen(path, "rb");
	if (!file)
		return false;

	struct sr_index_file_header header;
	bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
		  memcmp(header.magic, SR_INDEX_MAGIC, sizeof(header.magic)) == 0 &&
		  header.version == SR_SEGMENT_FORMAT_VERSION && os_fseeki64(file, 0, SEEK_END) == 0;
	const int64_t file_size = ok ? os_ftelli64(file) : -1;
	if (!ok || file_size < (int64_t)(sizeof(header) + sizeof(struct sr_index_entry))) {
		fclose(file);
		return false;
	}

	const uint64_t payload = (uint64_t)file_size - sizeof(header);
	const uint64_t entries = payload / sizeof(struct sr_index_entry);
	if (!entries) {
		fclose(file);
		return false;
	}

	struct sr_index_entry first;
	if (os_fseeki64(file, (int64_t)sizeof(header), SEEK_SET) != 0 ||
	    fread(&first, 1, sizeof(first), file) != sizeof(first) ||
	    !valid_segment_timestamp(header.segment_start_ns, first.timestamp_ns)) {
		fclose(file);
		return false;
	}

	struct sr_index_entry last = first;
	bool have_last = false;
	for (uint64_t back = 0; back < entries; back++) {
		const uint64_t index = entries - 1 - back;
		const uint64_t offset = sizeof(header) + index * sizeof(struct sr_index_entry);
		struct sr_index_entry candidate;
		if (offset > INT64_MAX || os_fseeki64(file, (int64_t)offset, SEEK_SET) != 0 ||
		    fread(&candidate, 1, sizeof(candidate), file) != sizeof(candidate))
			continue;
		if (candidate.timestamp_ns < first.timestamp_ns ||
		    !valid_segment_timestamp(header.segment_start_ns, candidate.timestamp_ns))
			continue;
		last = candidate;
		have_last = true;
		break;
	}
	fclose(file);
	if (!have_last)
		return false;
	media_bounds_add(bounds, first.timestamp_ns, last.timestamp_ns);
	return true;
}

static bool scan_audio_index_file(const char *path, struct sr_media_bounds *bounds)
{
	FILE *file = os_fopen(path, "rb");
	if (!file)
		return false;

	struct sr_audio_index_header header;
	bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
		  memcmp(header.magic, SR_AUDIO_INDEX_MAGIC, sizeof(header.magic)) == 0 &&
		  header.version == SR_AUDIO_FORMAT_VERSION && os_fseeki64(file, 0, SEEK_END) == 0;
	const int64_t file_size = ok ? os_ftelli64(file) : -1;
	if (!ok || file_size < (int64_t)(sizeof(header) + sizeof(struct sr_audio_index_entry))) {
		fclose(file);
		return false;
	}

	const uint64_t payload = (uint64_t)file_size - sizeof(header);
	const uint64_t entries = payload / sizeof(struct sr_audio_index_entry);
	if (!entries) {
		fclose(file);
		return false;
	}

	struct sr_audio_index_entry first;
	if (os_fseeki64(file, (int64_t)sizeof(header), SEEK_SET) != 0 ||
	    fread(&first, 1, sizeof(first), file) != sizeof(first) ||
	    !valid_segment_timestamp(header.segment_start_ns, first.timestamp_ns)) {
		fclose(file);
		return false;
	}

	struct sr_audio_index_entry last = first;
	bool have_last = false;
	for (uint64_t back = 0; back < entries; back++) {
		const uint64_t index = entries - 1 - back;
		const uint64_t offset = sizeof(header) + index * sizeof(struct sr_audio_index_entry);
		struct sr_audio_index_entry candidate;
		if (offset > INT64_MAX || os_fseeki64(file, (int64_t)offset, SEEK_SET) != 0 ||
		    fread(&candidate, 1, sizeof(candidate), file) != sizeof(candidate))
			continue;
		if (candidate.timestamp_ns < first.timestamp_ns ||
		    !valid_segment_timestamp(header.segment_start_ns, candidate.timestamp_ns))
			continue;
		last = candidate;
		have_last = true;
		break;
	}
	fclose(file);
	if (!have_last)
		return false;
	media_bounds_add(bounds, first.timestamp_ns, last.timestamp_ns);
	return true;
}

static void scan_index_pattern(const char *pattern, bool audio, struct sr_media_bounds *bounds)
{
	os_glob_t *glob = NULL;
	if (!pattern || os_glob(pattern, 0, &glob) != 0)
		return;
	for (size_t i = 0; i < glob->gl_pathc; i++) {
		if (glob->gl_pathv[i].directory)
			continue;
		if (audio)
			scan_audio_index_file(glob->gl_pathv[i].path, bounds);
		else
			scan_video_index_file(glob->gl_pathv[i].path, bounds);
	}
	os_globfree(glob);
}

static void scan_session_pattern(const char *session_dir, const char *tail, bool audio, struct sr_media_bounds *bounds)
{
	char *pattern = join_path(session_dir, tail);
	if (!pattern)
		return;
	scan_index_pattern(pattern, audio, bounds);
	bfree(pattern);
}

bool sr_session_get_media_bounds(const char *session_dir, uint64_t *start_ns, uint64_t *end_ns)
{
	if (start_ns)
		*start_ns = 0;
	if (end_ns)
		*end_ns = 0;
	if (!session_dir || !*session_dir)
		return false;

	struct sr_media_bounds video = {0};
	struct sr_media_bounds audio = {0};
	scan_session_pattern(session_dir, "cam-*/*.sridx", false, &video);
	scan_session_pattern(session_dir, "cam-*/*.sridx.part", false, &video);
	scan_session_pattern(session_dir, "cam-*/*.sraidx", true, &audio);
	scan_session_pattern(session_dir, "cam-*/*.sraidx.part", true, &audio);
	scan_session_pattern(session_dir, "audio-master/*.sraidx", true, &audio);
	scan_session_pattern(session_dir, "audio-master/*.sraidx.part", true, &audio);

	struct sr_media_bounds result = {0};
	if (video.have) {
		result = video;
		if (audio.have) {
			const uint64_t video_end_slop = video.end_ns > UINT64_MAX - SR_AUDIO_VIDEO_SLOP_NS
								? UINT64_MAX
								: video.end_ns + SR_AUDIO_VIDEO_SLOP_NS;
			const uint64_t audio_end_slop = audio.end_ns > UINT64_MAX - SR_AUDIO_VIDEO_SLOP_NS
								? UINT64_MAX
								: audio.end_ns + SR_AUDIO_VIDEO_SLOP_NS;
			if (audio.start_ns <= video_end_slop && audio_end_slop >= video.start_ns) {
				if (audio.start_ns < result.start_ns &&
				    result.start_ns - audio.start_ns <= SR_AUDIO_VIDEO_SLOP_NS)
					result.start_ns = audio.start_ns;
				if (audio.end_ns > result.end_ns &&
				    audio.end_ns - result.end_ns <= SR_AUDIO_VIDEO_SLOP_NS)
					result.end_ns = audio.end_ns;
				else if (audio.end_ns >
					 result.end_ns + (result.end_ns <= UINT64_MAX - SR_AUDIO_VIDEO_SLOP_NS
								  ? SR_AUDIO_VIDEO_SLOP_NS
								  : 0))
					blog(LOG_WARNING,
					     "Pitel Instant Replay: ignoring audio timestamp tail outside video timeline in '%s'",
					     session_dir);
			}
		}
	} else if (audio.have) {
		result = audio;
	}

	if (!result.have)
		return false;
	if (start_ns)
		*start_ns = result.start_ns;
	if (end_ns)
		*end_ns = result.end_ns;
	return true;
}

static void recover_stale_recording_runs(const char *session_dir, uint64_t media_end_ns)
{
	sqlite3 *sql = open_session_sqlite(session_dir);
	if (!sql)
		return;

	sqlite3_stmt *stmt = NULL;
	const char *query =
		"UPDATE recording_runs SET ended_unix=COALESCE(ended_unix,unixepoch()),"
		"timeline_end_ns=CASE WHEN timeline_end_ns IS NULL AND ? >= timeline_start_ns THEN ? "
		"ELSE timeline_end_ns END "
		"WHERE id=(SELECT id FROM recording_runs WHERE ended_unix IS NULL ORDER BY id DESC LIMIT 1)";
	bool recovered = false;
	if (sqlite3_prepare_v2(sql, query, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)media_end_ns);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)media_end_ns);
		if (sqlite3_step(stmt) == SQLITE_DONE)
			recovered = sqlite3_changes(sql) > 0;
	}
	sqlite3_finalize(stmt);

	/* Older interrupted rows are historical bookkeeping only. Mark them closed
	 * without inventing an OBS end timestamp or media end that we cannot prove. */
	char *error = NULL;
	if (sqlite3_exec(sql,
			 "UPDATE recording_runs SET ended_unix=COALESCE(ended_unix,unixepoch()) "
			 "WHERE ended_unix IS NULL",
			 NULL, NULL, &error) != SQLITE_OK) {
		blog(LOG_WARNING, "Pitel Instant Replay: could not close stale recording runs: %s",
		     error ? error : "unknown");
	}
	sqlite3_free(error);
	sqlite3_close(sql);

	if (recovered)
		blog(LOG_INFO, "Pitel Instant Replay: recovered an interrupted recording run at session %.3f s",
		     (double)media_end_ns / 1e9);
}

static uint64_t frame_interval_ns(void)
{
	struct obs_video_info ovi = {0};
	if (!obs_get_video_info(&ovi) || !ovi.fps_num || !ovi.fps_den)
		return 33333333ULL;
	return (1000000000ULL * (uint64_t)ovi.fps_den + ovi.fps_num - 1) / ovi.fps_num;
}

static bool begin_recording_run_locked(uint64_t obs_now_ns, uint64_t timeline_start_ns, bool discontinuity)
{
	sqlite3 *sql = open_session_sqlite(g_recording_path);
	if (!sql)
		return false;
	sqlite3_stmt *stmt = NULL;
	const char *query =
		"INSERT INTO recording_runs(started_unix,timeline_start_ns,obs_start_ns,discontinuity) VALUES(unixepoch(),?,?,?)";
	int rc = sqlite3_prepare_v2(sql, query, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_int64(stmt, 1, (sqlite3_int64)timeline_start_ns) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)obs_now_ns) == SQLITE_OK &&
		  sqlite3_bind_int(stmt, 3, discontinuity ? 1 : 0) == SQLITE_OK;
	if (ok)
		ok = sqlite3_step(stmt) == SQLITE_DONE;
	if (ok)
		g_recording_run_id = (uint64_t)sqlite3_last_insert_rowid(sql);
	sqlite3_finalize(stmt);
	sqlite3_close(sql);
	return ok;
}

static void finish_recording_run_locked(uint64_t obs_now_ns)
{
	if (!g_recording_path || !g_recording_run_id)
		return;

	uint64_t media_start_ns = 0;
	uint64_t media_end_ns = 0;
	const bool have_media = sr_session_get_media_bounds(g_recording_path, &media_start_ns, &media_end_ns);
	uint64_t timeline_end = g_recording_timeline_start_ns;
	if (have_media && media_end_ns >= g_recording_timeline_start_ns)
		timeline_end = media_end_ns;

	uint64_t projected_end = g_recording_timeline_start_ns;
	if (obs_now_ns >= g_recording_obs_start_ns &&
	    obs_now_ns - g_recording_obs_start_ns <= UINT64_MAX - projected_end)
		projected_end += obs_now_ns - g_recording_obs_start_ns;
	if (projected_end > timeline_end + 2000000000ULL)
		blog(LOG_WARNING,
		     "Pitel Instant Replay: REC clock advanced to %.3f s but committed media ends at %.3f s; run end follows media",
		     (double)projected_end / 1e9, (double)timeline_end / 1e9);

	sqlite3 *sql = open_session_sqlite(g_recording_path);
	if (!sql)
		return;
	sqlite3_stmt *stmt = NULL;
	const char *query =
		"UPDATE recording_runs SET ended_unix=unixepoch(),timeline_end_ns=?,obs_end_ns=? WHERE id=?";
	if (sqlite3_prepare_v2(sql, query, -1, &stmt, NULL) == SQLITE_OK) {
		sqlite3_bind_int64(stmt, 1, (sqlite3_int64)timeline_end);
		sqlite3_bind_int64(stmt, 2, (sqlite3_int64)obs_now_ns);
		sqlite3_bind_int64(stmt, 3, (sqlite3_int64)g_recording_run_id);
		sqlite3_step(stmt);
	}
	sqlite3_finalize(stmt);
	sqlite3_close(sql);
}

static bool create_session_locked(const char *requested_name, bool make_opened, bool make_target, char **created_path)
{
	if (created_path)
		*created_path = NULL;
	char *root = sr_config_get_session_root();
	if (!root || !*root) {
		bfree(root);
		return false;
	}
	if (os_mkdirs(root) == MKDIR_ERROR) {
		bfree(root);
		return false;
	}

	const time_t now = time(NULL);
	struct tm tmv;
#ifdef _WIN32
	localtime_s(&tmv, &now);
#else
	localtime_r(&now, &tmv);
#endif
	char stamp[32];
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
	char friendly[96];
	if (requested_name && *requested_name) {
		snprintf(friendly, sizeof(friendly), "%s", requested_name);
	} else {
		strftime(friendly, sizeof(friendly), "Session %Y-%m-%d %H:%M:%S", &tmv);
	}

	char *uuid = os_generate_uuid();
	char short_id[9];
	uuid_short_id(uuid, short_id);
	struct dstr dir = {0};
	dstr_copy(&dir, root);
	dstr_replace(&dir, "\\", "/");
	if (dir.len && dstr_end(&dir) != '/')
		dstr_cat_ch(&dir, '/');
	dstr_cat(&dir, stamp);
	dstr_cat_ch(&dir, '-');
	dstr_cat(&dir, short_id);

	bool ok = os_mkdirs(dir.array) != MKDIR_ERROR;
	const char *id = uuid && *uuid ? uuid : short_id;
	if (ok)
		ok = write_session_metadata(dir.array, id, friendly, now);
	if (ok && make_opened)
		ok = set_opened_locked(dir.array);
	if (ok && make_target) {
		bfree(g_record_target_path);
		g_record_target_path = bstrdup(dir.array);
		ok = g_record_target_path != NULL;
	}
	if (ok && created_path)
		*created_path = bstrdup(dir.array);
	if (ok)
		blog(LOG_INFO, "Pitel Instant Replay: created replay session '%s' at '%s'", friendly, dir.array);

	dstr_free(&dir);
	bfree(uuid);
	bfree(root);
	return ok;
}

void sr_session_init(void)
{
	pthread_mutex_init(&g_session_mutex, NULL);
	g_opened_path = NULL;
	g_opened_id = NULL;
	g_record_target_path = NULL;
	g_recording_path = NULL;
	g_recording_generation = 1;

	char *last = load_last_opened();
	if (!last || !session_valid(last)) {
		bfree(last);
		last = find_latest_session();
	}
	if (last) {
		pthread_mutex_lock(&g_session_mutex);
		if (set_opened_locked(last))
			blog(LOG_INFO, "Pitel Instant Replay: restored last opened session '%s'", last);
		pthread_mutex_unlock(&g_session_mutex);
		bfree(last);
	}
}

void sr_session_free(void)
{
	pthread_mutex_lock(&g_session_mutex);
	if (g_recording_path)
		finish_recording_run_locked(obs_get_video_frame_time());
	bfree(g_opened_path);
	bfree(g_opened_id);
	bfree(g_record_target_path);
	bfree(g_recording_path);
	g_opened_path = NULL;
	g_opened_id = NULL;
	g_record_target_path = NULL;
	g_recording_path = NULL;
	pthread_mutex_unlock(&g_session_mutex);
	pthread_mutex_destroy(&g_session_mutex);
}

char *sr_session_get_or_create_path(void)
{
	pthread_mutex_lock(&g_session_mutex);
	if (!g_opened_path)
		create_session_locked(NULL, true, false, NULL);
	char *result = g_opened_path ? bstrdup(g_opened_path) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

char *sr_session_get_opened_path(void)
{
	pthread_mutex_lock(&g_session_mutex);
	char *result = g_opened_path ? bstrdup(g_opened_path) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

char *sr_session_get_record_target_path(void)
{
	pthread_mutex_lock(&g_session_mutex);
	char *result = g_record_target_path ? bstrdup(g_record_target_path) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

char *sr_session_get_recording_path(void)
{
	pthread_mutex_lock(&g_session_mutex);
	char *result = g_recording_path ? bstrdup(g_recording_path) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

char *sr_session_get_id(void)
{
	pthread_mutex_lock(&g_session_mutex);
	char *result = g_opened_id ? bstrdup(g_opened_id) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

bool sr_session_create_new(const char *display_name, bool make_opened, bool make_record_target, char **created_path)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool ok = !g_recording_path &&
			create_session_locked(display_name, make_opened, make_record_target, created_path);
	pthread_mutex_unlock(&g_session_mutex);
	return ok;
}

bool sr_session_open(const char *path)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool ok = set_opened_locked(path);
	pthread_mutex_unlock(&g_session_mutex);
	return ok;
}

bool sr_session_set_record_target(const char *path)
{
	pthread_mutex_lock(&g_session_mutex);
	bool ok = !g_recording_path && path && *path && session_valid(path);
	if (ok) {
		bfree(g_record_target_path);
		g_record_target_path = bstrdup(path);
		ok = g_record_target_path != NULL;
		g_recording_generation++;
	}
	pthread_mutex_unlock(&g_session_mutex);
	return ok;
}

void sr_session_clear_record_target(void)
{
	pthread_mutex_lock(&g_session_mutex);
	if (!g_recording_path) {
		bfree(g_record_target_path);
		g_record_target_path = NULL;
		g_recording_generation++;
	}
	pthread_mutex_unlock(&g_session_mutex);
}

bool sr_session_prepare_recording(uint64_t obs_now_ns)
{
	pthread_mutex_lock(&g_session_mutex);
	if (g_recording_path) {
		pthread_mutex_unlock(&g_session_mutex);
		return true;
	}
	if (g_record_target_path && g_opened_path && !same_path(g_record_target_path, g_opened_path)) {
		blog(LOG_WARNING,
		     "Pitel Instant Replay: START REC refused because Opened Session and Recording Target differ; use Resume Recording explicitly");
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}
	if (!g_record_target_path && !create_session_locked(NULL, true, true, NULL)) {
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}

	g_recording_path = bstrdup(g_record_target_path);
	if (!g_recording_path) {
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}

	uint64_t media_start_ns = 0;
	uint64_t previous_end = 0;
	const bool have_media = sr_session_get_media_bounds(g_recording_path, &media_start_ns, &previous_end);
	recover_stale_recording_runs(g_recording_path, have_media ? previous_end : 0);
	g_recording_discontinuity = have_media;
	g_recording_obs_start_ns = obs_now_ns;
	if (have_media) {
		const uint64_t interval = frame_interval_ns();
		if (previous_end > UINT64_MAX - interval) {
			bfree(g_recording_path);
			g_recording_path = NULL;
			pthread_mutex_unlock(&g_session_mutex);
			return false;
		}
		g_recording_timeline_start_ns = previous_end + interval;
	} else {
		/* Session time is independent of OBS uptime. A new session always starts
		 * at zero; only deltas inside a Run are derived from the native OBS clock. */
		g_recording_timeline_start_ns = 0;
	}
	g_recording_run_id = 0;
	if (!begin_recording_run_locked(obs_now_ns, g_recording_timeline_start_ns, g_recording_discontinuity)) {
		bfree(g_recording_path);
		g_recording_path = NULL;
		pthread_mutex_unlock(&g_session_mutex);
		return false;
	}
	g_recording_generation++;
	blog(LOG_INFO, "Pitel Instant Replay: recording run %llu started at session %.3f s%s",
	     (unsigned long long)g_recording_run_id, (double)g_recording_timeline_start_ns / 1e9,
	     g_recording_discontinuity ? " (resume/discontinuity)" : "");
	pthread_mutex_unlock(&g_session_mutex);
	return true;
}

void sr_session_finish_recording(uint64_t obs_now_ns)
{
	pthread_mutex_lock(&g_session_mutex);
	if (g_recording_path) {
		finish_recording_run_locked(obs_now_ns);
		blog(LOG_INFO, "Pitel Instant Replay: recording run %llu closed",
		     (unsigned long long)g_recording_run_id);
		bfree(g_recording_path);
		g_recording_path = NULL;
		g_recording_obs_start_ns = 0;
		g_recording_timeline_start_ns = 0;
		g_recording_run_id = 0;
		g_recording_discontinuity = false;
		g_recording_generation++;
	}
	pthread_mutex_unlock(&g_session_mutex);
}

bool sr_session_recording_is_active(void)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool result = g_recording_path != NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

uint64_t sr_session_map_recording_timestamp(uint64_t obs_timestamp_ns)
{
	pthread_mutex_lock(&g_session_mutex);
	uint64_t result = 0;
	if (g_recording_path) {
		result = g_recording_timeline_start_ns;
		if (obs_timestamp_ns >= g_recording_obs_start_ns &&
		    obs_timestamp_ns - g_recording_obs_start_ns <= UINT64_MAX - result)
			result += obs_timestamp_ns - g_recording_obs_start_ns;
	}
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

uint64_t sr_session_recording_start_ns(void)
{
	pthread_mutex_lock(&g_session_mutex);
	const uint64_t result = g_recording_path ? g_recording_timeline_start_ns : 0;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

uint64_t sr_session_recording_generation(void)
{
	pthread_mutex_lock(&g_session_mutex);
	const uint64_t result = g_recording_generation;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

bool sr_session_recording_starts_with_discontinuity(void)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool result = g_recording_path && g_recording_discontinuity;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

bool sr_session_path_is_active(const char *path)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool result = g_recording_path && same_path(g_recording_path, path);
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

bool sr_session_path_is_opened(const char *path)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool result = g_opened_path && same_path(g_opened_path, path);
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

bool sr_session_path_is_record_target(const char *path)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool result = g_record_target_path && same_path(g_record_target_path, path);
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

char *sr_session_get_display_name(const char *path)
{
	char *name = NULL;
	read_metadata(path, NULL, &name, NULL);
	if (name && *name)
		return name;
	bfree(name);
	const char *base = path ? strrchr(path, '/') : NULL;
#ifdef _WIN32
	const char *base2 = path ? strrchr(path, '\\') : NULL;
	if (!base || (base2 && base2 > base))
		base = base2;
#endif
	return bstrdup(base ? base + 1 : (path ? path : "Session"));
}

bool sr_session_rename(const char *path, const char *display_name)
{
	if (!path || !*path || !display_name || !*display_name || !session_valid(path))
		return false;
	char *id = NULL;
	time_t created = 0;
	if (!read_metadata(path, &id, NULL, &created))
		return false;
	const bool ok = write_session_metadata(path, id, display_name, created ? created : time(NULL));
	bfree(id);
	return ok;
}

bool sr_session_register_camera(const char *session_dir, const char *stable_key, const char *display_name,
				int64_t sync_offset_ns)
{
	if (!session_dir || !stable_key || !*stable_key || !display_name || !*display_name)
		return false;
	sqlite3 *sql = open_session_sqlite(session_dir);
	if (!sql)
		return false;
	sqlite3_stmt *stmt = NULL;
	const char *query =
		"INSERT INTO cameras(stable_key,display_name,sync_offset_ns) VALUES(?,?,?) "
		"ON CONFLICT(stable_key) DO UPDATE SET display_name=excluded.display_name,sync_offset_ns=excluded.sync_offset_ns";
	int rc = sqlite3_prepare_v2(sql, query, -1, &stmt, NULL);
	bool ok = rc == SQLITE_OK && sqlite3_bind_text(stmt, 1, stable_key, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
		  sqlite3_bind_text(stmt, 2, display_name, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
		  sqlite3_bind_int64(stmt, 3, (sqlite3_int64)sync_offset_ns) == SQLITE_OK;
	if (ok)
		ok = sqlite3_step(stmt) == SQLITE_DONE;
	sqlite3_finalize(stmt);
	sqlite3_close(sql);
	return ok;
}

bool sr_session_resolve_camera(const char *session_dir, const char *camera_name, char *stable_key, size_t key_size,
			       int64_t *sync_offset_ns)
{
	if (!session_dir || !camera_name || !*camera_name || !stable_key || key_size < 2)
		return false;
	stable_key[0] = '\0';
	if (sync_offset_ns)
		*sync_offset_ns = 0;

	char current_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	const bool have_current_key = sr_camera_key_from_name(camera_name, current_key, sizeof(current_key));
	sqlite3 *sql = open_session_sqlite(session_dir);
	if (sql) {
		sqlite3_stmt *stmt = NULL;
		const char *query =
			have_current_key
				? "SELECT stable_key,sync_offset_ns FROM cameras WHERE stable_key=? OR display_name=? ORDER BY stable_key=? DESC LIMIT 1"
				: "SELECT stable_key,sync_offset_ns FROM cameras WHERE display_name=? LIMIT 1";
		if (sqlite3_prepare_v2(sql, query, -1, &stmt, NULL) == SQLITE_OK) {
			int index = 1;
			if (have_current_key)
				sqlite3_bind_text(stmt, index++, current_key, -1, SQLITE_TRANSIENT);
			sqlite3_bind_text(stmt, index++, camera_name, -1, SQLITE_TRANSIENT);
			if (have_current_key)
				sqlite3_bind_text(stmt, index, current_key, -1, SQLITE_TRANSIENT);
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const char *key = (const char *)sqlite3_column_text(stmt, 0);
				if (key && strlen(key) < key_size) {
					strcpy(stable_key, key);
					if (sync_offset_ns)
						*sync_offset_ns = (int64_t)sqlite3_column_int64(stmt, 1);
				}
			}
		}
		sqlite3_finalize(stmt);
		sqlite3_close(sql);
	}
	if (stable_key[0])
		return true;
	if (!have_current_key || strlen(current_key) >= key_size)
		return false;
	strcpy(stable_key, current_key);
	if (sync_offset_ns)
		sr_camera_sync_offset_ns(camera_name, sync_offset_ns);
	return true;
}

bool sr_session_list_camera_names(const char *session_dir, char ***names, size_t *count)
{
	if (!names || !count)
		return false;
	*names = NULL;
	*count = 0;
	if (!session_dir || !*session_dir)
		return true;
	sqlite3 *sql = open_session_sqlite(session_dir);
	if (!sql)
		return false;
	sqlite3_stmt *stmt = NULL;
	bool ok = sqlite3_prepare_v2(sql, "SELECT display_name FROM cameras ORDER BY id", -1, &stmt, NULL) == SQLITE_OK;
	char **items = NULL;
	size_t item_count = 0;
	while (ok && sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 0);
		if (!name || !*name)
			continue;
		char **next = brealloc(items, (item_count + 1) * sizeof(*items));
		if (!next) {
			ok = false;
			break;
		}
		items = next;
		items[item_count] = bstrdup(name);
		if (!items[item_count]) {
			ok = false;
			break;
		}
		item_count++;
	}
	sqlite3_finalize(stmt);
	sqlite3_close(sql);
	if (!ok) {
		sr_session_free_camera_names(items, item_count);
		return false;
	}
	*names = items;
	*count = item_count;
	return true;
}

void sr_session_free_camera_names(char **names, size_t count)
{
	if (!names)
		return;
	for (size_t i = 0; i < count; i++)
		bfree(names[i]);
	bfree(names);
}
