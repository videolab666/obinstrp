/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-session.h"
#include "sr-config.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>
#include <time.h>

#define SR_SESSION_FORMAT_VERSION 1

static pthread_mutex_t g_session_mutex;
static char *g_session_path;
static char *g_session_id;

static void uuid_short_id(const char *uuid, char out[9])
{
	size_t n = 0;
	if (uuid) {
		for (const char *p = uuid; *p && n < 8; p++) {
			if (*p == '-')
				continue;
			out[n++] = *p;
		}
	}
	while (n < 8)
		out[n++] = '0';
	out[8] = '\0';
}

static bool write_session_metadata(const char *dir, const char *session_id, time_t created)
{
	struct dstr path = {0};
	dstr_copy(&path, dir);
	if (path.len && dstr_end(&path) != '/')
		dstr_cat_ch(&path, '/');
	dstr_cat(&path, "session.json");

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "format_version", SR_SESSION_FORMAT_VERSION);
	obs_data_set_string(data, "session_id", session_id ? session_id : "");
	obs_data_set_int(data, "created_unix", (long long)created);
	obs_data_set_string(data, "plugin_version", PLUGIN_VERSION);
	obs_data_set_string(data, "timebase", "nanoseconds");

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		obs_data_set_int(data, "obs_fps_num", ovi.fps_num);
		obs_data_set_int(data, "obs_fps_den", ovi.fps_den);
	}

	const bool ok = obs_data_save_json(data, path.array);
	obs_data_release(data);
	dstr_free(&path);
	return ok;
}

static bool create_session_locked(void)
{
	if (g_session_path)
		return true;

	char *root = sr_config_get_session_root();
	if (!root || !*root) {
		bfree(root);
		return false;
	}

	if (os_mkdirs(root) == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "Sports Replay: could not create session root '%s'", root);
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

	if (os_mkdirs(dir.array) == MKDIR_ERROR) {
		obs_log(LOG_ERROR, "Sports Replay: could not create session directory '%s'", dir.array);
		dstr_free(&dir);
		bfree(uuid);
		bfree(root);
		return false;
	}

	g_session_path = bstrdup(dir.array);
	g_session_id = uuid ? bstrdup(uuid) : bstrdup(short_id);

	if (!write_session_metadata(g_session_path, g_session_id, now))
		obs_log(LOG_WARNING, "Sports Replay: could not write session metadata in '%s'", g_session_path);

	obs_log(LOG_INFO, "Sports Replay: continuous replay session '%s' at '%s'", g_session_id, g_session_path);

	dstr_free(&dir);
	bfree(uuid);
	bfree(root);
	return true;
}

void sr_session_init(void)
{
	pthread_mutex_init(&g_session_mutex, NULL);
	g_session_path = NULL;
	g_session_id = NULL;
}

void sr_session_free(void)
{
	pthread_mutex_lock(&g_session_mutex);
	bfree(g_session_path);
	bfree(g_session_id);
	g_session_path = NULL;
	g_session_id = NULL;
	pthread_mutex_unlock(&g_session_mutex);
	pthread_mutex_destroy(&g_session_mutex);
}

char *sr_session_get_or_create_path(void)
{
	pthread_mutex_lock(&g_session_mutex);
	const bool ok = create_session_locked();
	char *result = ok ? bstrdup(g_session_path) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}

char *sr_session_get_id(void)
{
	pthread_mutex_lock(&g_session_mutex);
	char *result = g_session_id ? bstrdup(g_session_id) : NULL;
	pthread_mutex_unlock(&g_session_mutex);
	return result;
}
