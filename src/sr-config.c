/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/threading.h>

#define DEFAULT_MIN_FREE_BYTES (100ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_PURGE_TARGET_BYTES (110ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_SEGMENT_DURATION_MS 4000u

static pthread_mutex_t g_mutex;
static char *g_save_dir;
static char *g_session_root;
static bool g_session_root_follows_save_dir;
static uint64_t g_min_free_bytes;
static uint64_t g_purge_target_bytes;
static enum sr_storage_low_space_action g_low_space_action;
static uint32_t g_segment_duration_ms;
static char *g_take_in_transition;
static char *g_take_out_transition;

/* Default location when the user hasn't chosen one: <Videos>/Sports Replay,
 * created if needed. Falls back to the plugin config dir. */
static char *default_save_dir(void)
{
	struct dstr d = {0};
	const char *home = getenv("USERPROFILE");
	if (home && *home) {
		dstr_copy(&d, home);
		dstr_replace(&d, "\\", "/");
		dstr_cat(&d, "/Videos/Sports Replay");
	} else {
		char *cfg = obs_module_config_path("replays");
		dstr_copy(&d, cfg ? cfg : "replays");
		bfree(cfg);
	}
	os_mkdirs(d.array);
	char *result = bstrdup(d.array);
	dstr_free(&d);
	return result;
}

static char *default_session_root(const char *save_dir)
{
	struct dstr d = {0};
	dstr_copy(&d, save_dir && *save_dir ? save_dir : "replays");
	dstr_replace(&d, "\\", "/");
	if (d.len && dstr_end(&d) != '/')
		dstr_cat_ch(&d, '/');
	dstr_cat(&d, "Sessions");
	char *result = bstrdup(d.array);
	dstr_free(&d);
	return result;
}

static void save_locked(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "schema_version", SR_CONFIG_SCHEMA_VERSION);
	obs_data_set_string(data, "save_dir", g_save_dir ? g_save_dir : "");
	obs_data_set_string(data, "session_root", g_session_root ? g_session_root : "");
	obs_data_set_bool(data, "session_root_follows_save_dir", g_session_root_follows_save_dir);
	obs_data_set_int(data, "min_free_bytes", (long long)g_min_free_bytes);
	obs_data_set_int(data, "purge_target_bytes", (long long)g_purge_target_bytes);
	obs_data_set_int(data, "low_space_action", (long long)g_low_space_action);
	obs_data_set_int(data, "segment_duration_ms", g_segment_duration_ms);
	obs_data_set_string(data, "take_in_transition", g_take_in_transition ? g_take_in_transition : "");
	obs_data_set_string(data, "take_out_transition", g_take_out_transition ? g_take_out_transition : "");

	char *path = obs_module_config_path("config.json");
	if (path)
		obs_data_save_json(data, path);
	bfree(path);
	obs_data_release(data);
}

void sr_config_init(void)
{
	pthread_mutex_init(&g_mutex, NULL);

	char *path = obs_module_config_path("config.json");
	obs_data_t *data = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);

	const char *saved = data ? obs_data_get_string(data, "save_dir") : "";
	g_save_dir = (saved && *saved) ? bstrdup(saved) : default_save_dir();

	const char *session_root = data ? obs_data_get_string(data, "session_root") : "";
	const bool has_session_root = session_root && *session_root;
	g_session_root_follows_save_dir = !has_session_root || obs_data_get_bool(data, "session_root_follows_save_dir");
	g_session_root = g_session_root_follows_save_dir ? default_session_root(g_save_dir) : bstrdup(session_root);

	const int64_t min_free = data ? obs_data_get_int(data, "min_free_bytes") : 0;
	const int64_t purge_target = data ? obs_data_get_int(data, "purge_target_bytes") : 0;
	const int64_t segment_ms = data ? obs_data_get_int(data, "segment_duration_ms") : 0;
	const int64_t low_space_action = data ? obs_data_get_int(data, "low_space_action") : 0;

	g_min_free_bytes = min_free > 0 ? (uint64_t)min_free : DEFAULT_MIN_FREE_BYTES;
	g_purge_target_bytes = purge_target > 0 ? (uint64_t)purge_target : DEFAULT_PURGE_TARGET_BYTES;
	if (g_purge_target_bytes < g_min_free_bytes)
		g_purge_target_bytes = g_min_free_bytes;

	g_low_space_action = low_space_action >= SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED &&
					     low_space_action <= SR_STORAGE_LOW_SPACE_WARN_ONLY
				     ? (enum sr_storage_low_space_action)low_space_action
				     : SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;

	g_segment_duration_ms = segment_ms >= 1000 && segment_ms <= 60000 ? (uint32_t)segment_ms
									  : DEFAULT_SEGMENT_DURATION_MS;

	const char *take_in = data ? obs_data_get_string(data, "take_in_transition") : "";
	const char *take_out = data ? obs_data_get_string(data, "take_out_transition") : "";
	g_take_in_transition = bstrdup(take_in ? take_in : "");
	g_take_out_transition = bstrdup(take_out ? take_out : "");

	os_mkdirs(g_save_dir);
	os_mkdirs(g_session_root);

	if (data)
		obs_data_release(data);
}

void sr_config_free(void)
{
	bfree(g_save_dir);
	bfree(g_session_root);
	bfree(g_take_in_transition);
	bfree(g_take_out_transition);
	g_save_dir = NULL;
	g_session_root = NULL;
	g_take_in_transition = NULL;
	g_take_out_transition = NULL;
	pthread_mutex_destroy(&g_mutex);
}

char *sr_config_get_save_dir(void)
{
	pthread_mutex_lock(&g_mutex);
	char *r = bstrdup(g_save_dir ? g_save_dir : "");
	pthread_mutex_unlock(&g_mutex);
	return r;
}

void sr_config_set_save_dir(const char *save_dir)
{
	pthread_mutex_lock(&g_mutex);
	bfree(g_save_dir);
	g_save_dir = bstrdup(save_dir ? save_dir : "");
	if (g_save_dir && *g_save_dir)
		os_mkdirs(g_save_dir);

	if (g_session_root_follows_save_dir) {
		bfree(g_session_root);
		g_session_root = default_session_root(g_save_dir);
		os_mkdirs(g_session_root);
	}

	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

char *sr_config_get_session_root(void)
{
	pthread_mutex_lock(&g_mutex);
	char *r = bstrdup(g_session_root ? g_session_root : "");
	pthread_mutex_unlock(&g_mutex);
	return r;
}

void sr_config_set_session_root(const char *session_root)
{
	pthread_mutex_lock(&g_mutex);
	bfree(g_session_root);
	if (session_root && *session_root) {
		g_session_root = bstrdup(session_root);
		g_session_root_follows_save_dir = false;
	} else {
		g_session_root = default_session_root(g_save_dir);
		g_session_root_follows_save_dir = true;
	}
	if (g_session_root && *g_session_root)
		os_mkdirs(g_session_root);
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

uint64_t sr_config_get_min_free_bytes(void)
{
	pthread_mutex_lock(&g_mutex);
	const uint64_t value = g_min_free_bytes;
	pthread_mutex_unlock(&g_mutex);
	return value;
}

void sr_config_set_min_free_bytes(uint64_t bytes)
{
	pthread_mutex_lock(&g_mutex);
	g_min_free_bytes = bytes;
	if (g_purge_target_bytes < g_min_free_bytes)
		g_purge_target_bytes = g_min_free_bytes;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

uint64_t sr_config_get_purge_target_bytes(void)
{
	pthread_mutex_lock(&g_mutex);
	const uint64_t value = g_purge_target_bytes;
	pthread_mutex_unlock(&g_mutex);
	return value;
}

void sr_config_set_purge_target_bytes(uint64_t bytes)
{
	pthread_mutex_lock(&g_mutex);
	g_purge_target_bytes = bytes < g_min_free_bytes ? g_min_free_bytes : bytes;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

enum sr_storage_low_space_action sr_config_get_low_space_action(void)
{
	pthread_mutex_lock(&g_mutex);
	const enum sr_storage_low_space_action value = g_low_space_action;
	pthread_mutex_unlock(&g_mutex);
	return value;
}

void sr_config_set_low_space_action(enum sr_storage_low_space_action action)
{
	if (action < SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED || action > SR_STORAGE_LOW_SPACE_WARN_ONLY)
		action = SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;

	pthread_mutex_lock(&g_mutex);
	g_low_space_action = action;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

uint32_t sr_config_get_segment_duration_ms(void)
{
	pthread_mutex_lock(&g_mutex);
	const uint32_t value = g_segment_duration_ms;
	pthread_mutex_unlock(&g_mutex);
	return value;
}

void sr_config_set_segment_duration_ms(uint32_t milliseconds)
{
	if (milliseconds < 1000)
		milliseconds = 1000;
	if (milliseconds > 60000)
		milliseconds = 60000;

	pthread_mutex_lock(&g_mutex);
	g_segment_duration_ms = milliseconds;
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

static char *get_transition_name(char *value)
{
	return bstrdup(value ? value : "");
}

char *sr_config_get_take_in_transition(void)
{
	pthread_mutex_lock(&g_mutex);
	char *result = get_transition_name(g_take_in_transition);
	pthread_mutex_unlock(&g_mutex);
	return result;
}

void sr_config_set_take_in_transition(const char *transition_name)
{
	pthread_mutex_lock(&g_mutex);
	bfree(g_take_in_transition);
	g_take_in_transition = bstrdup(transition_name ? transition_name : "");
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}

char *sr_config_get_take_out_transition(void)
{
	pthread_mutex_lock(&g_mutex);
	char *result = get_transition_name(g_take_out_transition);
	pthread_mutex_unlock(&g_mutex);
	return result;
}

void sr_config_set_take_out_transition(const char *transition_name)
{
	pthread_mutex_lock(&g_mutex);
	bfree(g_take_out_transition);
	g_take_out_transition = bstrdup(transition_name ? transition_name : "");
	save_locked();
	pthread_mutex_unlock(&g_mutex);
}
