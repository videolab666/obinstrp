/*
 * Pitel Instant Replay - persistent configuration
 * Copyright (C) 2026 Alexander Pitel
 *
 * This OBS plugin component is licensed under the GNU General Public License
 * version 2 or later. See LICENSE for details.
 */

#include "sr-config.h"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#define DEFAULT_MIN_FREE_BYTES (20ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_PURGE_TARGET_BYTES (25ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_SEGMENT_DURATION_MS 4000u
#define DEFAULT_EVENT_TRANSITION_DURATION_MS 200u

static pthread_mutex_t g_lock;
static bool g_initialized;
static char *g_session_root;
static uint64_t g_min_free_bytes;
static uint64_t g_purge_target_bytes;
static enum sr_storage_low_space_action g_low_space_action;
static uint32_t g_segment_duration_ms;
static char *g_take_in_transition;
static char *g_take_out_transition;
static char *g_event_transition;
static uint32_t g_event_transition_duration_ms;
static bool g_event_transition_match_speed;
static enum sr_replay_speed_policy g_speed_policy;
static bool g_program_output_enabled;

static char *copy_string(const char *value)
{
	return bstrdup(value ? value : "");
}

static char *default_storage_root(void)
{
	char *path = obs_module_config_path("standalone-v1/Sessions");
	if (!path)
		return bstrdup("Sessions");
	return path;
}

static uint64_t positive_or_default(int64_t value, uint64_t fallback)
{
	return value > 0 ? (uint64_t)value : fallback;
}

static void free_strings(void)
{
	bfree(g_session_root);
	bfree(g_take_in_transition);
	bfree(g_take_out_transition);
	bfree(g_event_transition);
	g_session_root = NULL;
	g_take_in_transition = NULL;
	g_take_out_transition = NULL;
	g_event_transition = NULL;
}

static void persist_locked(void)
{
	char *directory = obs_module_config_path("standalone-v1");
	if (directory) {
		os_mkdirs(directory);
		bfree(directory);
	}

	obs_data_t *json = obs_data_create();
	obs_data_set_int(json, "schema_version", SR_CONFIG_SCHEMA_VERSION);
	obs_data_set_string(json, "session_root", g_session_root ? g_session_root : "");
	obs_data_set_int(json, "min_free_bytes", (long long)g_min_free_bytes);
	obs_data_set_int(json, "purge_target_bytes", (long long)g_purge_target_bytes);
	obs_data_set_int(json, "low_space_action", (long long)g_low_space_action);
	obs_data_set_int(json, "segment_duration_ms", g_segment_duration_ms);
	obs_data_set_string(json, "take_in_transition", g_take_in_transition ? g_take_in_transition : "");
	obs_data_set_string(json, "take_out_transition", g_take_out_transition ? g_take_out_transition : "");
	obs_data_set_string(json, "event_transition", g_event_transition ? g_event_transition : "");
	obs_data_set_int(json, "event_transition_duration_ms", g_event_transition_duration_ms);
	obs_data_set_bool(json, "event_transition_match_replay_speed", g_event_transition_match_speed);
	obs_data_set_int(json, "replay_speed_policy", (long long)g_speed_policy);
	obs_data_set_bool(json, "program_output_enabled", g_program_output_enabled);

	char *path = obs_module_config_path("standalone-v1/config.json");
	if (path)
		obs_data_save_json(json, path);
	bfree(path);
	obs_data_release(json);
}

void sr_config_init(void)
{
	if (g_initialized)
		return;
	pthread_mutex_init(&g_lock, NULL);
	g_initialized = true;

	char *path = obs_module_config_path("standalone-v1/config.json");
	obs_data_t *json = path ? obs_data_create_from_json_file(path) : NULL;
	bfree(path);

	const char *saved_session = json ? obs_data_get_string(json, "session_root") : "";
	if (saved_session && *saved_session)
		g_session_root = copy_string(saved_session);
	else
		g_session_root = default_storage_root();

	g_min_free_bytes = positive_or_default(json ? obs_data_get_int(json, "min_free_bytes") : 0,
					       DEFAULT_MIN_FREE_BYTES);
	g_purge_target_bytes = positive_or_default(json ? obs_data_get_int(json, "purge_target_bytes") : 0,
						    DEFAULT_PURGE_TARGET_BYTES);
	if (g_purge_target_bytes < g_min_free_bytes)
		g_purge_target_bytes = g_min_free_bytes;

	const int64_t low_space = json ? obs_data_get_int(json, "low_space_action") : 0;
	if (low_space < SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED || low_space > SR_STORAGE_LOW_SPACE_WARN_ONLY)
		g_low_space_action = SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;
	else
		g_low_space_action = (enum sr_storage_low_space_action)low_space;

	const int64_t segment_ms = json ? obs_data_get_int(json, "segment_duration_ms") : 0;
	g_segment_duration_ms = segment_ms >= 1000 && segment_ms <= 60000 ? (uint32_t)segment_ms
									  : DEFAULT_SEGMENT_DURATION_MS;

	g_take_in_transition = copy_string(json ? obs_data_get_string(json, "take_in_transition") : "");
	g_take_out_transition = copy_string(json ? obs_data_get_string(json, "take_out_transition") : "");
	g_event_transition = copy_string(json ? obs_data_get_string(json, "event_transition") : "");

	const int64_t event_ms = json ? obs_data_get_int(json, "event_transition_duration_ms") : 0;
	g_event_transition_duration_ms = event_ms >= 50 && event_ms <= 10000 ? (uint32_t)event_ms
									      : DEFAULT_EVENT_TRANSITION_DURATION_MS;
	g_event_transition_match_speed = json ? obs_data_get_bool(json, "event_transition_match_replay_speed") : false;
	g_speed_policy = json && obs_data_get_int(json, "replay_speed_policy") == SR_REPLAY_SPEED_EVENT
			 ? SR_REPLAY_SPEED_EVENT
			 : SR_REPLAY_SPEED_GLOBAL;
	g_program_output_enabled = json ? obs_data_get_bool(json, "program_output_enabled") : false;

	if (g_session_root && *g_session_root)
		os_mkdirs(g_session_root);
	if (json)
		obs_data_release(json);
}

void sr_config_free(void)
{
	if (!g_initialized)
		return;
	pthread_mutex_lock(&g_lock);
	free_strings();
	pthread_mutex_unlock(&g_lock);
	pthread_mutex_destroy(&g_lock);
	g_initialized = false;
}


char *sr_config_get_session_root(void)
{
	pthread_mutex_lock(&g_lock);
	char *result = copy_string(g_session_root);
	pthread_mutex_unlock(&g_lock);
	return result;
}

void sr_config_set_session_root(const char *session_root)
{
	char *replacement = session_root && *session_root ? copy_string(session_root) : default_storage_root();
	if (replacement && *replacement)
		os_mkdirs(replacement);

	pthread_mutex_lock(&g_lock);
	bfree(g_session_root);
	g_session_root = replacement;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

uint64_t sr_config_get_min_free_bytes(void)
{
	pthread_mutex_lock(&g_lock);
	const uint64_t value = g_min_free_bytes;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_min_free_bytes(uint64_t bytes)
{
	pthread_mutex_lock(&g_lock);
	g_min_free_bytes = bytes;
	if (g_purge_target_bytes < bytes)
		g_purge_target_bytes = bytes;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

uint64_t sr_config_get_purge_target_bytes(void)
{
	pthread_mutex_lock(&g_lock);
	const uint64_t value = g_purge_target_bytes;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_purge_target_bytes(uint64_t bytes)
{
	pthread_mutex_lock(&g_lock);
	g_purge_target_bytes = bytes < g_min_free_bytes ? g_min_free_bytes : bytes;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

enum sr_storage_low_space_action sr_config_get_low_space_action(void)
{
	pthread_mutex_lock(&g_lock);
	const enum sr_storage_low_space_action value = g_low_space_action;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_low_space_action(enum sr_storage_low_space_action action)
{
	if (action < SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED || action > SR_STORAGE_LOW_SPACE_WARN_ONLY)
		action = SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED;
	pthread_mutex_lock(&g_lock);
	g_low_space_action = action;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

uint32_t sr_config_get_segment_duration_ms(void)
{
	pthread_mutex_lock(&g_lock);
	const uint32_t value = g_segment_duration_ms;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_segment_duration_ms(uint32_t milliseconds)
{
	if (milliseconds < 1000)
		milliseconds = 1000;
	else if (milliseconds > 60000)
		milliseconds = 60000;
	pthread_mutex_lock(&g_lock);
	g_segment_duration_ms = milliseconds;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

static char *get_transition(char *value)
{
	return copy_string(value);
}

char *sr_config_get_take_in_transition(void)
{
	pthread_mutex_lock(&g_lock);
	char *result = get_transition(g_take_in_transition);
	pthread_mutex_unlock(&g_lock);
	return result;
}

void sr_config_set_take_in_transition(const char *transition_name)
{
	pthread_mutex_lock(&g_lock);
	bfree(g_take_in_transition);
	g_take_in_transition = copy_string(transition_name);
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

char *sr_config_get_take_out_transition(void)
{
	pthread_mutex_lock(&g_lock);
	char *result = get_transition(g_take_out_transition);
	pthread_mutex_unlock(&g_lock);
	return result;
}

void sr_config_set_take_out_transition(const char *transition_name)
{
	pthread_mutex_lock(&g_lock);
	bfree(g_take_out_transition);
	g_take_out_transition = copy_string(transition_name);
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

char *sr_config_get_event_transition(void)
{
	pthread_mutex_lock(&g_lock);
	char *result = get_transition(g_event_transition);
	pthread_mutex_unlock(&g_lock);
	return result;
}

void sr_config_set_event_transition(const char *transition_name)
{
	pthread_mutex_lock(&g_lock);
	bfree(g_event_transition);
	g_event_transition = copy_string(transition_name);
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

uint32_t sr_config_get_event_transition_duration_ms(void)
{
	pthread_mutex_lock(&g_lock);
	const uint32_t value = g_event_transition_duration_ms;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_event_transition_duration_ms(uint32_t milliseconds)
{
	if (milliseconds < 50)
		milliseconds = 50;
	else if (milliseconds > 10000)
		milliseconds = 10000;
	pthread_mutex_lock(&g_lock);
	g_event_transition_duration_ms = milliseconds;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

bool sr_config_get_event_transition_match_replay_speed(void)
{
	pthread_mutex_lock(&g_lock);
	const bool value = g_event_transition_match_speed;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_event_transition_match_replay_speed(bool enabled)
{
	pthread_mutex_lock(&g_lock);
	g_event_transition_match_speed = enabled;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

enum sr_replay_speed_policy sr_config_get_replay_speed_policy(void)
{
	pthread_mutex_lock(&g_lock);
	const enum sr_replay_speed_policy value = g_speed_policy;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_replay_speed_policy(enum sr_replay_speed_policy policy)
{
	if (policy != SR_REPLAY_SPEED_EVENT)
		policy = SR_REPLAY_SPEED_GLOBAL;
	pthread_mutex_lock(&g_lock);
	g_speed_policy = policy;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}

bool sr_config_get_program_output_enabled(void)
{
	pthread_mutex_lock(&g_lock);
	const bool value = g_program_output_enabled;
	pthread_mutex_unlock(&g_lock);
	return value;
}

void sr_config_set_program_output_enabled(bool enabled)
{
	pthread_mutex_lock(&g_lock);
	g_program_output_enabled = enabled;
	persist_locked();
	pthread_mutex_unlock(&g_lock);
}
