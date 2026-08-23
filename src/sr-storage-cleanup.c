/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-storage-cleanup.h"

#include "sr-event-db.h"
#include "sr-media-guard.h"
#include "sr-segment-reader.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

struct sr_gc_candidate {
	char *segment_path;
	char *index_path;
	uint64_t start_ns;
	uint64_t end_ns;
};

static pthread_mutex_t g_gc_mutex;
static bool g_gc_mutex_initialized;

bool sr_storage_cleanup_init(void)
{
	if (g_gc_mutex_initialized)
		return true;
	if (!sr_media_guard_init())
		return false;
	if (pthread_mutex_init(&g_gc_mutex, NULL) != 0) {
		sr_media_guard_free();
		return false;
	}
	g_gc_mutex_initialized = true;
	return true;
}

void sr_storage_cleanup_free(void)
{
	if (g_gc_mutex_initialized) {
		pthread_mutex_destroy(&g_gc_mutex);
		g_gc_mutex_initialized = false;
	}
	sr_media_guard_free();
}

static char *join_path(const char *dir, const char *tail)
{
	struct dstr path = {0};
	dstr_copy(&path, dir ? dir : "");
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, tail ? tail : "");
	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

static char *index_path_for_segment(const char *segment_path)
{
	if (!segment_path)
		return NULL;
	const size_t len = strlen(segment_path);
	static const char suffix[] = ".srseg";
	if (len < sizeof(suffix) - 1 || strcmp(segment_path + len - (sizeof(suffix) - 1), suffix) != 0)
		return NULL;

	struct dstr path = {0};
	dstr_ncopy(&path, segment_path, len - (sizeof(suffix) - 1));
	dstr_cat(&path, ".sridx");
	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

static bool segment_range(const char *segment_path, const char *index_path, uint64_t *start_ns, uint64_t *end_ns)
{
	struct sr_segment_reader *reader = sr_segment_reader_open(segment_path, index_path);
	if (!reader)
		return false;

	struct sr_segment_stream_info info;
	bool ok = sr_segment_reader_get_info(reader, &info);
	uint64_t end = info.segment_start_ns;
	if (ok && info.indexed_packets) {
		struct sr_index_entry last;
		ok = sr_segment_reader_find(reader, UINT64_MAX, false, &last);
		if (ok)
			end = last.timestamp_ns;
	}

	if (ok) {
		if (start_ns)
			*start_ns = info.segment_start_ns;
		if (end_ns)
			*end_ns = end;
	}
	sr_segment_reader_close(reader);
	return ok;
}

static bool delete_pair_if_unreferenced(struct sr_event_db *events, const char *segment_path, const char *index_path,
					uint64_t start_ns, uint64_t end_ns, struct sr_storage_cleanup_result *result)
{
	bool pinned = true;
	sr_media_guard_lock();
	const bool query_ok = sr_event_db_has_event_overlap(events, start_ns, end_ns, &pinned);
	if (!query_ok) {
		sr_media_guard_unlock();
		result->errors++;
		return false;
	}
	if (pinned) {
		sr_media_guard_unlock();
		result->segments_pinned++;
		return true;
	}

	const int segment_rc = os_unlink(segment_path);
	int index_rc = 0;
	if (segment_rc == 0)
		index_rc = os_unlink(index_path);
	sr_media_guard_unlock();

	if (segment_rc == 0 && index_rc == 0) {
		result->segments_deleted++;
		blog(LOG_INFO, "Sports Replay: permanently deleted unreferenced replay segment '%s'", segment_path);
		return true;
	}

	result->errors++;
	blog(LOG_WARNING, "Sports Replay: could not completely delete replay segment pair '%s' / '%s'", segment_path,
	     index_path);
	return false;
}

static void cleanup_camera_dir(struct sr_event_db *events, const char *camera_dir, uint64_t range_in_ns,
			       uint64_t range_out_ns, struct sr_storage_cleanup_result *result)
{
	char *pattern = join_path(camera_dir, "*.srseg");
	if (!pattern) {
		result->errors++;
		return;
	}

	os_glob_t *glob = NULL;
	if (os_glob(pattern, 0, &glob) != 0) {
		bfree(pattern);
		return;
	}
	bfree(pattern);

	for (size_t i = 0; i < glob->gl_pathc; i++) {
		if (glob->gl_pathv[i].directory)
			continue;

		const char *segment_path = glob->gl_pathv[i].path;
		char *index_path = index_path_for_segment(segment_path);
		if (!index_path || !os_file_exists(index_path)) {
			bfree(index_path);
			result->errors++;
			continue;
		}

		result->segments_examined++;
		uint64_t start_ns = 0;
		uint64_t end_ns = 0;
		if (!segment_range(segment_path, index_path, &start_ns, &end_ns)) {
			bfree(index_path);
			result->errors++;
			continue;
		}

		if (start_ns < range_in_ns || end_ns > range_out_ns) {
			bfree(index_path);
			continue;
		}

		delete_pair_if_unreferenced(events, segment_path, index_path, start_ns, end_ns, result);
		bfree(index_path);
	}

	os_globfree(glob);
}

bool sr_storage_delete_unreferenced_range(struct sr_event_db *events, uint64_t range_in_ns, uint64_t range_out_ns,
					  struct sr_storage_cleanup_result *result)
{
	if (!events || range_out_ns < range_in_ns)
		return false;

	struct sr_storage_cleanup_result local = {0};
	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir)
		return false;

	char *pattern = join_path(session_dir, "cam-*");
	if (!pattern) {
		bfree(session_dir);
		return false;
	}

	os_glob_t *glob = NULL;
	if (os_glob(pattern, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (!glob->gl_pathv[i].directory)
				continue;
			local.camera_dirs_scanned++;
			cleanup_camera_dir(events, glob->gl_pathv[i].path, range_in_ns, range_out_ns, &local);
		}
		os_globfree(glob);
	}

	bfree(pattern);
	bfree(session_dir);
	if (result)
		*result = local;
	return true;
}

static int gc_candidate_compare(const void *a, const void *b)
{
	const struct sr_gc_candidate *ca = a;
	const struct sr_gc_candidate *cb = b;
	if (ca->end_ns < cb->end_ns)
		return -1;
	if (ca->end_ns > cb->end_ns)
		return 1;
	if (ca->start_ns < cb->start_ns)
		return -1;
	if (ca->start_ns > cb->start_ns)
		return 1;
	return strcmp(ca->segment_path, cb->segment_path);
}

static bool append_gc_candidate(struct sr_gc_candidate **items, size_t *count, size_t *capacity,
				const char *segment_path, const char *index_path,
				struct sr_storage_cleanup_result *result)
{
	uint64_t start_ns = 0;
	uint64_t end_ns = 0;
	if (!segment_range(segment_path, index_path, &start_ns, &end_ns)) {
		result->errors++;
		return false;
	}

	if (*count == *capacity) {
		const size_t next_capacity = *capacity ? *capacity * 2 : 256;
		struct sr_gc_candidate *next = brealloc(*items, next_capacity * sizeof(**items));
		if (!next) {
			result->errors++;
			return false;
		}
		*items = next;
		*capacity = next_capacity;
	}

	char *segment_copy = bstrdup(segment_path);
	char *index_copy = bstrdup(index_path);
	if (!segment_copy || !index_copy) {
		bfree(segment_copy);
		bfree(index_copy);
		result->errors++;
		return false;
	}

	struct sr_gc_candidate *dst = &(*items)[(*count)++];
	dst->segment_path = segment_copy;
	dst->index_path = index_copy;
	dst->start_ns = start_ns;
	dst->end_ns = end_ns;
	return true;
}

static void free_gc_candidates(struct sr_gc_candidate *items, size_t count)
{
	if (!items)
		return;
	for (size_t i = 0; i < count; i++) {
		bfree(items[i].segment_path);
		bfree(items[i].index_path);
	}
	bfree(items);
}

static bool collect_gc_candidates(const char *session_dir, struct sr_gc_candidate **items, size_t *count,
				  struct sr_storage_cleanup_result *result)
{
	*items = NULL;
	*count = 0;
	size_t capacity = 0;

	char *camera_pattern = join_path(session_dir, "cam-*");
	if (!camera_pattern)
		return false;

	os_glob_t *cameras = NULL;
	if (os_glob(camera_pattern, 0, &cameras) != 0) {
		bfree(camera_pattern);
		return true;
	}
	bfree(camera_pattern);

	for (size_t c = 0; c < cameras->gl_pathc; c++) {
		if (!cameras->gl_pathv[c].directory)
			continue;
		result->camera_dirs_scanned++;
		char *segment_pattern = join_path(cameras->gl_pathv[c].path, "*.srseg");
		if (!segment_pattern) {
			result->errors++;
			continue;
		}
		os_glob_t *segments = NULL;
		if (os_glob(segment_pattern, 0, &segments) == 0) {
			for (size_t i = 0; i < segments->gl_pathc; i++) {
				if (segments->gl_pathv[i].directory)
					continue;
				char *index_path = index_path_for_segment(segments->gl_pathv[i].path);
				if (!index_path || !os_file_exists(index_path)) {
					bfree(index_path);
					result->errors++;
					continue;
				}
				append_gc_candidate(items, count, &capacity, segments->gl_pathv[i].path, index_path,
						    result);
				bfree(index_path);
			}
			os_globfree(segments);
		}
		bfree(segment_pattern);
	}
	os_globfree(cameras);

	if (*count > 1)
		qsort(*items, *count, sizeof(**items), gc_candidate_compare);
	return true;
}

bool sr_storage_gc_reclaim_unreferenced(const char *session_dir, const char *volume_path, uint64_t target_free_bytes,
					struct sr_storage_cleanup_result *result)
{
	if (!session_dir || !*session_dir || !volume_path || !*volume_path || !target_free_bytes)
		return false;

	struct sr_storage_cleanup_result local = {0};
	if (!g_gc_mutex_initialized)
		return false;
	pthread_mutex_lock(&g_gc_mutex);
	local.free_bytes_before = os_get_free_disk_space(volume_path);
	local.free_bytes_after = local.free_bytes_before;
	if (local.free_bytes_before >= target_free_bytes) {
		local.target_reached = true;
		pthread_mutex_unlock(&g_gc_mutex);
		if (result)
			*result = local;
		return true;
	}

	struct sr_event_db *events = sr_event_db_open(session_dir);
	if (!events) {
		local.errors++;
		pthread_mutex_unlock(&g_gc_mutex);
		if (result)
			*result = local;
		return false;
	}

	struct sr_gc_candidate *items = NULL;
	size_t count = 0;
	const bool scan_ok = collect_gc_candidates(session_dir, &items, &count, &local);
	if (scan_ok) {
		for (size_t i = 0; i < count && local.free_bytes_after < target_free_bytes; i++) {
			local.segments_examined++;
			delete_pair_if_unreferenced(events, items[i].segment_path, items[i].index_path,
						    items[i].start_ns, items[i].end_ns, &local);
			local.free_bytes_after = os_get_free_disk_space(volume_path);
		}
	} else {
		local.errors++;
	}

	local.target_reached = local.free_bytes_after >= target_free_bytes;
	free_gc_candidates(items, count);
	sr_event_db_close(events);
	pthread_mutex_unlock(&g_gc_mutex);
	if (result)
		*result = local;
	return scan_ok;
}
