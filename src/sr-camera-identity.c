/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-camera-identity.h"
#include "sr-capture.h"

#include <util/bmem.h>
#include <util/dstr.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool valid_key_char(unsigned char c)
{
	return isalnum(c) || c == '-' || c == '_';
}

static bool copy_key(const char *value, char *key, size_t key_size)
{
	if (!value || !*value || !key || key_size < 2)
		return false;

	const size_t length = strlen(value);
	if (length >= key_size)
		return false;
	for (size_t i = 0; i < length; i++) {
		if (!valid_key_char((unsigned char)value[i]))
			return false;
	}

	memcpy(key, value, length + 1);
	return true;
}

bool sr_camera_key_from_source(const obs_source_t *source, char *key, size_t key_size)
{
	if (!source)
		return false;
	return copy_key(obs_source_get_uuid(source), key, key_size);
}

bool sr_camera_key_from_name(const char *camera_name, char *key, size_t key_size)
{
	if (!camera_name || !*camera_name)
		return false;

	obs_source_t *source = obs_get_source_by_name(camera_name);
	if (!source)
		return false;
	const bool ok = sr_camera_key_from_source(source, key, key_size);
	obs_source_release(source);
	return ok;
}

char *sr_camera_name_from_key(const char *key)
{
	char checked[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!copy_key(key, checked, sizeof(checked)))
		return NULL;

	obs_source_t *source = obs_get_source_by_uuid(checked);
	if (!source)
		return NULL;
	const char *name = obs_source_get_name(source);
	char *result = name && *name ? bstrdup(name) : NULL;
	obs_source_release(source);
	return result;
}

struct sync_offset_query {
	bool found;
	bool ambiguous;
	int64_t offset_ms;
};

static void read_sync_offset(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct sync_offset_query *query = param;
	if (!query || query->ambiguous || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;

	if (query->found) {
		query->ambiguous = true;
		return;
	}

	obs_data_t *settings = obs_source_get_settings(child);
	if (!settings)
		return;
	query->offset_ms = obs_data_get_int(settings, S_SYNC_OFFSET_MS);
	query->found = true;
	obs_data_release(settings);
}

bool sr_camera_sync_offset_ns(const char *camera_name, int64_t *offset_ns)
{
	if (!camera_name || !*camera_name || !offset_ns)
		return false;
	*offset_ns = 0;

	obs_source_t *source = obs_get_source_by_name(camera_name);
	if (!source)
		return false;

	struct sync_offset_query query = {0};
	obs_source_enum_filters(source, read_sync_offset, &query);
	obs_source_release(source);
	if (!query.found || query.ambiguous || query.offset_ms < -SR_CAMERA_SYNC_MAX_MS ||
	    query.offset_ms > SR_CAMERA_SYNC_MAX_MS)
		return false;

	*offset_ns = query.offset_ms * 1000000LL;
	return true;
}

uint32_t sr_camera_key_hash(const char *key)
{
	uint32_t hash = 2166136261u;
	if (!key)
		return hash;
	while (*key) {
		hash ^= (uint8_t)*key++;
		hash *= 16777619u;
	}
	return hash;
}

static char *append_camera_folder(const char *session_dir, const char *folder)
{
	if (!session_dir || !*session_dir || !folder || !*folder)
		return NULL;

	struct dstr path = {0};
	dstr_copy(&path, session_dir);
	dstr_replace(&path, "\\", "/");
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, folder);

	char *result = bstrdup(path.array);
	dstr_free(&path);
	return result;
}

char *sr_camera_directory_for_key(const char *session_dir, const char *key)
{
	char checked[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!copy_key(key, checked, sizeof(checked)))
		return NULL;

	struct dstr folder = {0};
	dstr_copy(&folder, "cam-");
	dstr_cat(&folder, checked);
	char *result = append_camera_folder(session_dir, folder.array);
	dstr_free(&folder);
	return result;
}

char *sr_camera_legacy_directory(const char *session_dir, const char *camera_name)
{
	if (!camera_name || !*camera_name)
		return NULL;
	char folder[32];
	snprintf(folder, sizeof(folder), "cam-%08x", sr_camera_key_hash(camera_name));
	return append_camera_folder(session_dir, folder);
}
