/*
Pitel Instant Replay
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

struct sr_recording_run_record {
	uint64_t id;
	uint64_t timeline_start_ns;
	uint64_t timeline_end_ns;
	bool discontinuity;
};

/* Returns only closed Recording Runs whose timeline end is known. The caller
 * owns the returned bmalloc/brealloc array and releases it with bfree(). */
bool sr_recording_run_catalog_scan(const char *session_dir, struct sr_recording_run_record **runs, size_t *count);

#ifdef __cplusplus
}
#endif
