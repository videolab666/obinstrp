/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct sr_recovery_result {
	size_t video_segments_recovered;
	size_t audio_segments_recovered;
	size_t segments_skipped;
	size_t errors;
	uint64_t bytes_discarded;
};

typedef bool (*sr_recovery_stop_cb)(void *data);

/* Scans existing session directories below session_root and repairs stale
 * video/master-audio .part tails left by an interrupted OBS process. Recovery
 * copies only complete validated packet records, rebuilds the index from the
 * media file, and publishes the repaired pair with the normal final suffixes.
 * The original .part pair is retained until both recovered files are ready. */
bool sr_recovery_scan_root(const char *session_root, sr_recovery_stop_cb should_stop, void *stop_data,
			   struct sr_recovery_result *result);

#ifdef __cplusplus
}
#endif
