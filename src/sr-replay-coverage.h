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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum sr_replay_coverage {
	SR_REPLAY_COVERAGE_NONE = 0,
	SR_REPLAY_COVERAGE_PARTIAL = 1,
	SR_REPLAY_COVERAGE_FULL = 2,
};

struct sr_replay_coverage_info {
	enum sr_replay_coverage coverage;
	uint64_t playable_in_ns;
	uint64_t playable_out_ns;
	bool active;
};

/* Metadata-only camera probe used by the operator UI and playlist selector.
 * Consecutive segment ranges are merged only across a normal frame-to-frame
 * handoff. A segment explicitly marked discontinuous always starts a new
 * playable interval, so a camera with an internal recording hole is never
 * advertised as FULL merely because its first and last timestamps span the
 * whole Event. For PARTIAL coverage, the interval containing Event IN is
 * preferred; otherwise the longest available interval is returned. */
bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
			      struct sr_replay_coverage_info *info);

#ifdef __cplusplus
}
#endif
