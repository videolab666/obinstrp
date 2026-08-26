/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-storage-manager.h"

#include "sr-config.h"
#include "sr-recovery.h"
#include "sr-session.h"
#include "sr-storage-cleanup.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <string.h>
#include <time.h>

static pthread_mutex_t g_manager_mutex;
static pthread_t g_manager_thread;
static bool g_manager_mutex_initialized;
static bool g_manager_thread_started;
static bool g_manager_stopping;
static struct sr_storage_manager_status g_manager_status;

static bool manager_should_stop(void)
{
	pthread_mutex_lock(&g_manager_mutex);
	const bool stopping = g_manager_stopping;
	pthread_mutex_unlock(&g_manager_mutex);
	return stopping;
}

static bool recovery_should_stop(void *unused)
{
	UNUSED_PARAMETER(unused);
	return manager_should_stop();
}

static void run_recovery_once(void)
{
	char *session_root = sr_config_get_session_root();
	if (!session_root)
		return;

	struct sr_recovery_result result = {0};
	const bool ok = sr_recovery_scan_root(session_root, recovery_should_stop, NULL, &result);
	if (result.video_segments_recovered || result.audio_segments_recovered || result.errors) {
		blog(ok ? LOG_INFO : LOG_WARNING,
		     "Pitel Instant Replay: crash recovery finalized %zu video and %zu audio segment(s), discarded %.1f KiB of incomplete tails, errors %zu",
		     result.video_segments_recovered, result.audio_segments_recovered,
		     (double)result.bytes_discarded / 1024.0, result.errors);
	}
	bfree(session_root);
}

static void run_policy_once(void)
{
	if (sr_config_get_low_space_action() != SR_STORAGE_LOW_SPACE_DELETE_UNREFERENCED)
		return;

	/* Do not create an empty replay session merely because the manager is
	 * polling. A session ID exists only after the continuous recorder (or
	 * Event system) has created the active session. */
	char *session_id = sr_session_get_id();
	if (!session_id)
		return;
	bfree(session_id);

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir)
		return;

	const uint64_t min_free = sr_config_get_min_free_bytes();
	const uint64_t purge_target = sr_config_get_purge_target_bytes();
	const uint64_t free_bytes = os_get_free_disk_space(session_dir);
	if (min_free && free_bytes < min_free) {
		struct sr_storage_cleanup_result result = {0};
		const uint64_t target = purge_target >= min_free ? purge_target : min_free;
		const bool ok = sr_storage_gc_reclaim_unreferenced(session_dir, session_dir, target, &result);
		pthread_mutex_lock(&g_manager_mutex);
		g_manager_status.cleanup_passes++;
		g_manager_status.last_cleanup_unix = (uint64_t)time(NULL);
		g_manager_status.last_cleanup = result;
		pthread_mutex_unlock(&g_manager_mutex);
		blog(ok && !result.errors ? LOG_INFO : LOG_WARNING,
		     "Pitel Instant Replay: storage manager GC deleted %zu segment(s), pinned %zu, errors %zu, free %.1f -> %.1f GB%s",
		     result.segments_deleted, result.segments_pinned, result.errors,
		     (double)result.free_bytes_before / (1024.0 * 1024.0 * 1024.0),
		     (double)result.free_bytes_after / (1024.0 * 1024.0 * 1024.0),
		     result.target_reached ? ", target reached" : ", reserve still low");
	}
	bfree(session_dir);
}

static void *manager_thread(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("pitel-replay-storage");
	run_recovery_once();

	while (!manager_should_stop()) {
		run_policy_once();
		/* Keep stop latency modest without putting disk polling on realtime
		 * callbacks. A low-space GC pass is serialized by sr-storage-cleanup. */
		for (unsigned i = 0; i < 10 && !manager_should_stop(); i++)
			os_sleep_ms(100);
	}
	return NULL;
}

bool sr_storage_manager_start(void)
{
	if (g_manager_thread_started)
		return true;
	if (!g_manager_mutex_initialized) {
		if (pthread_mutex_init(&g_manager_mutex, NULL) != 0)
			return false;
		g_manager_mutex_initialized = true;
	}

	pthread_mutex_lock(&g_manager_mutex);
	g_manager_stopping = false;
	memset(&g_manager_status, 0, sizeof(g_manager_status));
	pthread_mutex_unlock(&g_manager_mutex);
	if (pthread_create(&g_manager_thread, NULL, manager_thread, NULL) != 0)
		return false;
	g_manager_thread_started = true;
	return true;
}

void sr_storage_manager_get_status(struct sr_storage_manager_status *status)
{
	if (!status)
		return;
	memset(status, 0, sizeof(*status));
	if (!g_manager_mutex_initialized)
		return;
	pthread_mutex_lock(&g_manager_mutex);
	*status = g_manager_status;
	pthread_mutex_unlock(&g_manager_mutex);
}

void sr_storage_manager_stop(void)
{
	if (g_manager_thread_started) {
		pthread_mutex_lock(&g_manager_mutex);
		g_manager_stopping = true;
		pthread_mutex_unlock(&g_manager_mutex);
		pthread_join(g_manager_thread, NULL);
		g_manager_thread_started = false;
	}
	if (g_manager_mutex_initialized) {
		pthread_mutex_destroy(&g_manager_mutex);
		g_manager_mutex_initialized = false;
	}
}
