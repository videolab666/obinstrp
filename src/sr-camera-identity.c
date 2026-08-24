/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-camera-identity.h"

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
