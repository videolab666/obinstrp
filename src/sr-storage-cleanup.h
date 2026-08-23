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

bool sr_storage_cleanup_init(void);
void sr_storage_cleanup_free(void);

struct sr_storage_cleanup_result {
	size_t camera_dirs_scanned;
	size_t segments_examined;
	size_t segments_deleted;
	size_t segments_pinned;
	size_t errors;
	uint64_t free_bytes_before;
	uint64_t free_bytes_after;
	bool target_reached;
};

bool sr_storage_delete_unreferenced_range(struct sr_event_db *events, uint64_t range_in_ns, uint64_t range_out_ns,
					  struct sr_storage_cleanup_result *result);

bool sr_storage_gc_reclaim_unreferenced(const char *session_dir, const char *volume_path, uint64_t target_free_bytes,
					struct sr_storage_cleanup_result *result);

#ifdef __cplusplus
}
#endif
