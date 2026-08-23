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

struct sr_event_db;

struct sr_storage_cleanup_result {
	size_t camera_dirs_scanned;
	size_t segments_examined;
	size_t segments_deleted;
	size_t segments_pinned;
	size_t errors;
};

/* Permanently removes finalized segment/index pairs that are wholly contained
 * inside [range_in_ns, range_out_ns] and are not referenced by any Event that
 * still exists in the supplied Event database. Active .part files are never
 * touched.
 *
 * The caller that coordinates destructive Event deletion must prevent new
 * Event mutations for the duration of this call. The Event controller does so
 * by keeping its mutex held around delete + cleanup, preventing a newly-marked
 * Event from racing with a physical segment unlink.
 *
 * This is deliberately conservative: boundary segments are retained because
 * they contain media outside the requested range, and any database-query or
 * file-system error causes that segment to be kept. */
bool sr_storage_delete_unreferenced_range(struct sr_event_db *events, uint64_t range_in_ns, uint64_t range_out_ns,
					  struct sr_storage_cleanup_result *result);

#ifdef __cplusplus
}
#endif
