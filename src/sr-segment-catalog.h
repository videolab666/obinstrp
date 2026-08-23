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

struct sr_segment_descriptor {
	uint32_t sequence;
	uint64_t start_ns;
	uint64_t end_ns;
	uint32_t flags;
	bool active;
	char *segment_path;
	char *index_path;
};

/* Scans one camera directory in a replay session. The returned array and all
 * descriptor paths are owned by the caller and released with
 * sr_segment_catalog_free(). Finalized and readable active .part pairs are
 * included and sorted by start timestamp. */
bool sr_segment_catalog_scan(const char *session_dir, const char *camera_name, struct sr_segment_descriptor **segments,
			     size_t *count);

void sr_segment_catalog_free(struct sr_segment_descriptor *segments, size_t count);

/* Finds the descriptor whose indexed timestamp range contains timestamp_ns.
 * If ranges overlap, the newest-starting matching descriptor wins. */
const struct sr_segment_descriptor *sr_segment_catalog_find(const struct sr_segment_descriptor *segments, size_t count,
							    uint64_t timestamp_ns);

#ifdef __cplusplus
}
#endif
