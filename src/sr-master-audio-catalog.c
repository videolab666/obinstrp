/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-master-audio-catalog.h"

#include "sr-master-audio-reader.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <stdlib.h>
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
	struct dstr result = {0};
	dstr_ncopy(&result, path, path_len - old_len);
	dstr_cat(&result, new_suffix);
	char *copy = bstrdup(result.array);
	dstr_free(&result);
	return copy;
}

static uint64_t samples_to_ns(uint32_t sample_rate, uint32_t samples)
{
	return sample_rate ? ((uint64_t)samples * 1000000000ULL) / sample_rate : 0;
}

static int descriptor_compare(const void *a, const void *b)
{
	const struct sr_master_audio_descriptor *aa = a;
	const struct sr_master_audio_descriptor *bb = b;
	if (aa->start_ns < bb->start_ns)
		return -1;
	if (aa->start_ns > bb->start_ns)
		return 1;
	if (aa->sequence < bb->sequence)
		return -1;
	if (aa->sequence > bb->sequence)
		return 1;
	if (aa->active != bb->active)
		return aa->active ? 1 : -1;
	return 0;
}

static bool append_descriptor(struct sr_master_audio_descriptor **items, size_t *count, size_t *capacity,
			      const char *audio_path, const char *index_path, bool active)
{
	struct sr_master_audio_reader *reader = sr_master_audio_reader_open(audio_path, index_path);
	if (!reader)
		return false;

	struct sr_master_audio_segment_info info;
	if (!sr_master_audio_reader_get_info(reader, &info)) {
		sr_master_audio_reader_close(reader);
		return false;
	}

	uint64_t end_ns = info.segment_start_ns;
	if (info.indexed_packets) {
		struct sr_audio_index_entry last;
		if (sr_master_audio_reader_entry_at(reader, info.indexed_packets - 1, &last))
			end_ns = last.timestamp_ns + samples_to_ns(info.sample_rate, last.samples);
	}

	if (*count == *capacity) {
		const size_t next_capacity = *capacity ? *capacity * 2 : 32;
		struct sr_master_audio_descriptor *next = brealloc(*items, next_capacity * sizeof(**items));
		if (!next) {
			sr_master_audio_reader_close(reader);
			return false;
		}
		*items = next;
		*capacity = next_capacity;
	}

	char *audio_copy = bstrdup(audio_path);
	char *index_copy = bstrdup(index_path);
	if (!audio_copy || !index_copy) {
		bfree(audio_copy);
		bfree(index_copy);
		sr_master_audio_reader_close(reader);
		return false;
	}

	struct sr_master_audio_descriptor *dst = &(*items)[(*count)++];
	memset(dst, 0, sizeof(*dst));
	dst->sequence = info.sequence;
	dst->flags = info.segment_flags;
	dst->sample_rate = info.sample_rate;
	dst->start_ns = info.segment_start_ns;
	dst->end_ns = end_ns;
	dst->active = active;
	dst->audio_path = audio_copy;
	dst->index_path = index_copy;

	sr_master_audio_reader_close(reader);
	return true;
}

bool sr_master_audio_catalog_scan(const char *session_dir, struct sr_master_audio_descriptor **segments,
				  size_t *count)
{
	if (!segments || !count)
		return false;
	*segments = NULL;
	*count = 0;
	if (!session_dir || !*session_dir)
		return false;

	char *audio_dir = join_path(session_dir, "audio-master");
	if (!audio_dir)
		return false;
	char *pattern = join_path(audio_dir, "*.sraud*");
	bfree(audio_dir);
	if (!pattern)
		return false;

	struct sr_master_audio_descriptor *items = NULL;
	size_t item_count = 0;
	size_t capacity = 0;
	os_glob_t *glob = NULL;
	if (os_glob(pattern, 0, &glob) == 0) {
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;

			const char *audio_path = glob->gl_pathv[i].path;
			const bool active = ends_with(audio_path, ".sraud.part");
			const bool finalized = ends_with(audio_path, ".sraud") && !active;
			if (!active && !finalized)
				continue;

			char *index_path = active ? replace_suffix(audio_path, ".sraud.part", ".sraidx.part")
						  : replace_suffix(audio_path, ".sraud", ".sraidx");
			if (!index_path)
				continue;
			if (os_file_exists(index_path))
				append_descriptor(&items, &item_count, &capacity, audio_path, index_path, active);
			bfree(index_path);
		}
		os_globfree(glob);
	}
	bfree(pattern);

	if (item_count > 1)
		qsort(items, item_count, sizeof(*items), descriptor_compare);
	*segments = items;
	*count = item_count;
	return true;
}

void sr_master_audio_catalog_free(struct sr_master_audio_descriptor *segments, size_t count)
{
	if (!segments)
		return;
	for (size_t i = 0; i < count; i++) {
		bfree(segments[i].audio_path);
		bfree(segments[i].index_path);
	}
	bfree(segments);
}

const struct sr_master_audio_descriptor *sr_master_audio_catalog_find(const struct sr_master_audio_descriptor *segments,
							      size_t count, uint64_t timestamp_ns)
{
	if (!segments || !count)
		return NULL;

	size_t lo = 0;
	size_t hi = count;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		if (segments[mid].start_ns <= timestamp_ns)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (!lo)
		return NULL;

	for (size_t i = lo; i > 0; i--) {
		const struct sr_master_audio_descriptor *candidate = &segments[i - 1];
		if (candidate->start_ns <= timestamp_ns && candidate->end_ns >= timestamp_ns)
			return candidate;
	}
	return NULL;
}
