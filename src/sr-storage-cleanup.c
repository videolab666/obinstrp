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
#include "sr-segment-reader.h"
#include "sr-session.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <string.h>

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

		/* Never remove a boundary segment: it contains recording outside the
		 * range the operator explicitly asked to delete. */
		if (start_ns < range_in_ns || end_ns > range_out_ns) {
			bfree(index_path);
			continue;
		}

		bool pinned = true;
		if (!sr_event_db_has_event_overlap(events, start_ns, end_ns, &pinned)) {
			/* Database uncertainty must keep media, never delete it. */
			result->errors++;
			bfree(index_path);
			continue;
		}
		if (pinned) {
			result->segments_pinned++;
			bfree(index_path);
			continue;
		}

		const int segment_rc = os_unlink(segment_path);
		const int index_rc = os_unlink(index_path);
		if (segment_rc == 0 && index_rc == 0) {
			result->segments_deleted++;
			blog(LOG_INFO, "Sports Replay: permanently deleted unreferenced replay segment '%s'", segment_path);
		} else {
			result->errors++;
			blog(LOG_WARNING, "Sports Replay: could not completely delete replay segment pair '%s' / '%s'",
			     segment_path, index_path);
		}
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
