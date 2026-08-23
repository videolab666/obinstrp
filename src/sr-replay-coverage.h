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

/* Cheap metadata-only camera probe used by the operator UI. It scans the
 * camera segment catalog but does not open a decoder. Coverage deliberately
 * follows the same first/last indexed bounds used by sr_replay_channel_cue(),
 * so an angle advertised as FULL/PARTIAL behaves the same when selected. */
bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
			      struct sr_replay_coverage_info *info);

#ifdef __cplusplus
}
#endif
