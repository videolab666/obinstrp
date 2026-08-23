/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-media-guard.h"

#include <util/threading.h>

static pthread_mutex_t g_media_guard;
static bool g_media_guard_initialized;

bool sr_media_guard_init(void)
{
	if (g_media_guard_initialized)
		return true;
	if (pthread_mutex_init(&g_media_guard, NULL) != 0)
		return false;
	g_media_guard_initialized = true;
	return true;
}

void sr_media_guard_free(void)
{
	if (!g_media_guard_initialized)
		return;
	pthread_mutex_destroy(&g_media_guard);
	g_media_guard_initialized = false;
}

void sr_media_guard_lock(void)
{
	pthread_mutex_lock(&g_media_guard);
}

void sr_media_guard_unlock(void)
{
	pthread_mutex_unlock(&g_media_guard);
}
