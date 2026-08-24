/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-segment-catalog.h"
#include "sr-camera-identity.h"
#include "sr-segment-reader.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdlib.h>
#include <string.h>

static bool ends_with(const char *value, const char *suffix)
{
	if (!value || !suffix)
		return false;
	const size_t value_len = strlen(value);
	const size_t suffix_len = strlen(suffix);
	return value_len >= suffix_len && memcmp(value + value_len - suffix_len, suffix, suffix_len) == 0;
}

static char *replace_suffix(const char *path, const char *old_suffix, const char *new_suffix)
{
	if (!ends_with(path, old_suffix))
		return NULL;

	const size_t path_len = strlen(path);
	const size_t old_len = strlen(old_suffix);

	struct dstr out = {0};
	dstr_ncopy(&out, path, path_len - old_len);
	dstr_cat(&out, new_suffix);
	char *result = bstrdup(out.array);
	dstr_free(&out);
	return result;
}

static int descriptor_compare(const void *a, const void *b)
{
	const struct sr_segment_descriptor *sa = a;
	const struct sr_segment_descriptor *sb = b;
	if (sa->start_ns < sb->start_ns)
		return -1;
	if (sa->start_ns > sb->start_ns)
		return 1;
	if (sa->sequence < sb->sequence)
		return -1;
	if (sa->sequence > sb->sequence)
		return 1;
	if (sa->active != sb->active)
		return sa->active ? 1 : -1;
	return 0;
}

static bool append_descriptor(struct sr_segment_descriptor **items, size_t *count, size_t *capacity,
			      const char *segment_path, const char *index_path, bool active)
{
	struct sr_segment_reader *reader = sr_segment_reader_open(segment_path, index_path);
	if (!reader)
		return false;

	struct sr_segment_stream_info info;
	if (!sr_segment_reader_get_info(reader, &info)) {
		sr_segment_reader_close(reader);
		return false;
	}

	uint64_t end_ns = info.segment_start_ns;
	if (info.indexed_packets) {
		struct sr_index_entry last;
		if (sr_segment_reader_find(reader, UINT64_MAX, false, &last))
			end_ns = last.timestamp_ns;
	}

	if (*count == *capacity) {
		const size_t next_capacity = *capacity ? *capacity * 2 : 32;
		struct sr_segment_descriptor *next = brealloc(*items, next_capacity * sizeof(**items));
		if (!next) {
			sr_segment_reader_close(reader);
			return false;
		}
		*items = next;
		*capacity = next_capacity;
	}

	struct sr_segment_descriptor *dst = &(*items)[(*count)++];
	memset(dst, 0, sizeof(*dst));
	dst->sequence = info.sequence;
	dst->start_ns = info.segment_start_ns;
	dst->end_ns = end_ns;
	dst->fps_num = info.fps_num;
	dst->fps_den = info.fps_den;
	dst->flags = info.segment_flags;
	dst->active = active;
	dst->segment_path = bstrdup(segment_path);
	dst->index_path = bstrdup(index_path);

	sr_segment_reader_close(reader);
	return true;
}

static void scan_directory(const char *camera_dir, struct sr_segment_descriptor **items, size_t *count,
			   size_t *capacity)
{
	if (!camera_dir || !*camera_dir)
		return;

	struct dstr pattern = {0};
	dstr_copy(&pattern, camera_dir);
	dstr_replace(&pattern, "\\", "/");
	if (pattern.len && dstr_end(&pattern) != '/')
		dstr_cat_ch(&pattern, '/');
	dstr_cat(&pattern, "*.srseg*");

	os_glob_t *glob = NULL;
	if (os_glob(pattern.array, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;

			const char *segment_path = glob->gl_pathv[i].path;
			const bool active = ends_with(segment_path, ".srseg.part");
			const bool finalized = ends_with(segment_path, ".srseg") && !active;
			if (!active && !finalized)
				continue;

			char *index_path = active ? replace_suffix(segment_path, ".srseg.part", ".sridx.part")
						  : replace_suffix(segment_path, ".srseg", ".sridx");
			if (!index_path)
				continue;

			if (os_file_exists(index_path))
				append_descriptor(items, count, capacity, segment_path, index_path, active);
			bfree(index_path);
		}
		os_globfree(glob);
	}
	dstr_free(&pattern);
}

bool sr_segment_catalog_scan(const char *session_dir, const char *camera_name, struct sr_segment_descriptor **segments,
			     size_t *count)
{
	if (!segments || !count)
		return false;
	*segments = NULL;
	*count = 0;

	if (!session_dir || !*session_dir || !camera_name || !*camera_name)
		return false;

	struct sr_segment_descriptor *items = NULL;
	size_t item_count = 0;
	size_t capacity = 0;

	char key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	char *stable_dir = NULL;
	if (sr_camera_key_from_name(camera_name, key, sizeof(key)))
		stable_dir = sr_camera_directory_for_key(session_dir, key);
	char *legacy_dir = sr_camera_legacy_directory(session_dir, camera_name);

	/* New recordings are keyed by the persistent OBS source UUID. Also scan
     * the old display-name hash directory so a session started with an older
     * plugin remains replayable after upgrading. */
	scan_directory(stable_dir, &items, &item_count, &capacity);
	if (!stable_dir || !legacy_dir || strcmp(stable_dir, legacy_dir) != 0)
		scan_directory(legacy_dir, &items, &item_count, &capacity);

	bfree(stable_dir);
	bfree(legacy_dir);

	if (item_count > 1)
		qsort(items, item_count, sizeof(*items), descriptor_compare);

	*segments = items;
	*count = item_count;
	return true;
}

void sr_segment_catalog_free(struct sr_segment_descriptor *segments, size_t count)
{
	if (!segments)
		return;
	for (size_t i = 0; i < count; i++) {
		bfree(segments[i].segment_path);
		bfree(segments[i].index_path);
	}
	bfree(segments);
}

const struct sr_segment_descriptor *sr_segment_catalog_find(const struct sr_segment_descriptor *segments, size_t count,
							    uint64_t timestamp_ns)
{
	if (!segments || !count)
		return NULL;

	/* Sorted by start time. Binary-search the newest segment whose start is
     * at/before the requested timestamp, then walk backward across rare
     * overlapping ranges until a containing segment is found. */
	size_t lo = 0;
	size_t hi = count;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (segments[mid].start_ns <= timestamp_ns)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return NULL;

	for (size_t i = lo; i > 0; i--) {
		const struct sr_segment_descriptor *candidate = &segments[i - 1];
		if (candidate->start_ns <= timestamp_ns && candidate->end_ns >= timestamp_ns)
			return candidate;
	}
	return NULL;
}
