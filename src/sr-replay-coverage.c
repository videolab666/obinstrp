/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-coverage.h"

#include "sr-segment-catalog.h"
#include "sr-session.h"

#include <util/bmem.h>

#include <limits.h>
#include <string.h>

bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
			      struct sr_replay_coverage_info *info)
{
	if (!info)
		return false;

	memset(info, 0, sizeof(*info));
	info->coverage = SR_REPLAY_COVERAGE_NONE;

	if (!camera_name || !*camera_name || event_out_ns < event_in_ns)
		return false;

	char *session_dir = sr_session_get_or_create_path();
	if (!session_dir)
		return false;

	struct sr_segment_descriptor *segments = NULL;
	size_t count = 0;
	const bool scanned = sr_segment_catalog_scan(session_dir, camera_name, &segments, &count);
	bfree(session_dir);
	if (!scanned)
		return false;

	uint64_t first_ns = UINT64_MAX;
	uint64_t last_ns = 0;
	bool active_overlap = false;
	for (size_t i = 0; i < count; i++) {
		if (segments[i].start_ns < first_ns)
			first_ns = segments[i].start_ns;
		if (segments[i].end_ns > last_ns)
			last_ns = segments[i].end_ns;
		if (segments[i].active && segments[i].end_ns >= event_in_ns && segments[i].start_ns <= event_out_ns)
			active_overlap = true;
	}

	if (first_ns != UINT64_MAX && event_out_ns >= first_ns && event_in_ns <= last_ns) {
		info->playable_in_ns = event_in_ns < first_ns ? first_ns : event_in_ns;
		info->playable_out_ns = event_out_ns > last_ns ? last_ns : event_out_ns;
		info->active = active_overlap;
		info->coverage = info->playable_in_ns == event_in_ns && info->playable_out_ns == event_out_ns
					 ? SR_REPLAY_COVERAGE_FULL
					 : SR_REPLAY_COVERAGE_PARTIAL;
	}

	sr_segment_catalog_free(segments, count);
	return true;
}
