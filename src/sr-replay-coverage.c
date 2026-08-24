/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-replay-coverage.h"

#include "sr-camera-identity.h"
#include "sr-segment-catalog.h"
#include "sr-segment-format.h"
#include "sr-session.h"

#include <util/bmem.h>

#include <string.h>

#define NS_PER_SECOND 1000000000ULL
#define FALLBACK_HANDOFF_NS 50000000ULL
#define MAX_HANDOFF_NS 250000000ULL

struct coverage_interval {
	uint64_t start_ns;
	uint64_t end_ns;
	bool active;
};

static bool global_to_media(uint64_t global_ns, int64_t offset_ns, uint64_t *media_ns)
{
	if (!media_ns)
		return false;
	if (offset_ns >= 0) {
		const uint64_t magnitude = (uint64_t)offset_ns;
		if (global_ns > UINT64_MAX - magnitude)
			return false;
		*media_ns = global_ns + magnitude;
		return true;
	}

	const uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;
	if (global_ns < magnitude)
		return false;
	*media_ns = global_ns - magnitude;
	return true;
}

static bool media_to_global(uint64_t media_ns, int64_t offset_ns, uint64_t *global_ns)
{
	if (!global_ns)
		return false;
	if (offset_ns >= 0) {
		const uint64_t magnitude = (uint64_t)offset_ns;
		if (media_ns < magnitude)
			return false;
		*global_ns = media_ns - magnitude;
		return true;
	}

	const uint64_t magnitude = (uint64_t)(-(offset_ns + 1)) + 1ULL;
	if (media_ns > UINT64_MAX - magnitude)
		return false;
	*global_ns = media_ns + magnitude;
	return true;
}

static uint64_t frame_interval_ns(const struct sr_segment_descriptor *segment)
{
	if (!segment || !segment->fps_num || !segment->fps_den)
		return FALLBACK_HANDOFF_NS / 2;

	const uint64_t numerator = NS_PER_SECOND * (uint64_t)segment->fps_den;
	return (numerator + segment->fps_num - 1) / segment->fps_num;
}

static uint64_t handoff_tolerance_ns(const struct sr_segment_descriptor *previous,
				     const struct sr_segment_descriptor *next)
{
	uint64_t frame_ns = frame_interval_ns(previous);
	const uint64_t next_frame_ns = frame_interval_ns(next);
	if (next_frame_ns > frame_ns)
		frame_ns = next_frame_ns;
	if (frame_ns > MAX_HANDOFF_NS / 2)
		return MAX_HANDOFF_NS;
	const uint64_t tolerance = frame_ns * 2;
	return tolerance ? tolerance : FALLBACK_HANDOFF_NS;
}

static bool normal_handoff(const struct sr_segment_descriptor *previous, const struct sr_segment_descriptor *next,
			   uint64_t current_end_ns)
{
	if (!previous || !next || (next->flags & SR_SEGMENT_FLAG_DISCONTINUITY))
		return false;
	if (next->start_ns <= current_end_ns)
		return true;
	return next->start_ns - current_end_ns <= handoff_tolerance_ns(previous, next);
}

static void consider_interval(const struct coverage_interval *interval, uint64_t event_in_ns, uint64_t event_out_ns,
			      bool require_timestamp, uint64_t timestamp_ns, struct sr_replay_coverage_info *info,
			      bool *have_best, bool *best_contains_in, uint64_t *best_duration)
{
	if (!interval || interval->end_ns < interval->start_ns)
		return;
	if (require_timestamp && (timestamp_ns < interval->start_ns || timestamp_ns > interval->end_ns))
		return;

	if (interval->start_ns == event_in_ns && interval->end_ns == event_out_ns) {
		info->coverage = SR_REPLAY_COVERAGE_FULL;
		info->playable_in_ns = event_in_ns;
		info->playable_out_ns = event_out_ns;
		info->active = interval->active;
		*have_best = true;
		*best_contains_in = true;
		*best_duration = event_out_ns - event_in_ns;
		return;
	}

	if (info->coverage == SR_REPLAY_COVERAGE_FULL)
		return;

	const bool contains_in = interval->start_ns <= event_in_ns && interval->end_ns >= event_in_ns;
	const uint64_t duration = interval->end_ns - interval->start_ns;
	const bool better = !*have_best || (contains_in && !*best_contains_in) ||
			    (contains_in == *best_contains_in && duration > *best_duration);
	if (!better)
		return;

	info->coverage = SR_REPLAY_COVERAGE_PARTIAL;
	info->playable_in_ns = interval->start_ns;
	info->playable_out_ns = interval->end_ns;
	info->active = interval->active;
	*have_best = true;
	*best_contains_in = contains_in;
	*best_duration = duration;
}

static bool coverage_query_internal(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
				    bool require_timestamp, uint64_t timestamp_ns, struct sr_replay_coverage_info *info)
{
	if (!info)
		return false;

	memset(info, 0, sizeof(*info));
	info->coverage = SR_REPLAY_COVERAGE_NONE;

	if (!camera_name || !*camera_name || event_out_ns < event_in_ns)
		return false;
	if (require_timestamp && (timestamp_ns < event_in_ns || timestamp_ns > event_out_ns))
		return false;

	int64_t sync_offset_ns = 0;
	if (!sr_camera_sync_offset_ns(camera_name, &sync_offset_ns))
		return false;
	info->sync_offset_ns = sync_offset_ns;

	uint64_t media_event_in_ns = 0;
	uint64_t media_event_out_ns = 0;
	uint64_t media_timestamp_ns = 0;
	if (!global_to_media(event_in_ns, sync_offset_ns, &media_event_in_ns) ||
	    !global_to_media(event_out_ns, sync_offset_ns, &media_event_out_ns) ||
	    (require_timestamp && !global_to_media(timestamp_ns, sync_offset_ns, &media_timestamp_ns)))
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

	struct coverage_interval current = {0};
	const struct sr_segment_descriptor *previous = NULL;
	bool have_current = false;
	bool have_best = false;
	bool best_contains_in = false;
	uint64_t best_duration = 0;

	for (size_t i = 0; i < count; i++) {
		const struct sr_segment_descriptor *segment = &segments[i];
		if (segment->end_ns < media_event_in_ns || segment->start_ns > media_event_out_ns)
			continue;

		const uint64_t start_ns = segment->start_ns < media_event_in_ns ? media_event_in_ns : segment->start_ns;
		const uint64_t end_ns = segment->end_ns > media_event_out_ns ? media_event_out_ns : segment->end_ns;
		if (!have_current) {
			current.start_ns = start_ns;
			current.end_ns = end_ns;
			current.active = segment->active;
			have_current = true;
			previous = segment;
			continue;
		}

		if (normal_handoff(previous, segment, current.end_ns)) {
			if (end_ns > current.end_ns)
				current.end_ns = end_ns;
			current.active = current.active || segment->active;
			previous = segment;
			continue;
		}

		consider_interval(&current, media_event_in_ns, media_event_out_ns, require_timestamp,
				  media_timestamp_ns, info, &have_best, &best_contains_in, &best_duration);
		current.start_ns = start_ns;
		current.end_ns = end_ns;
		current.active = segment->active;
		previous = segment;
	}

	if (have_current)
		consider_interval(&current, media_event_in_ns, media_event_out_ns, require_timestamp,
				  media_timestamp_ns, info, &have_best, &best_contains_in, &best_duration);

	sr_segment_catalog_free(segments, count);

	if (info->coverage != SR_REPLAY_COVERAGE_NONE) {
		uint64_t global_in_ns = 0;
		uint64_t global_out_ns = 0;
		if (!media_to_global(info->playable_in_ns, sync_offset_ns, &global_in_ns) ||
		    !media_to_global(info->playable_out_ns, sync_offset_ns, &global_out_ns)) {
			memset(info, 0, sizeof(*info));
			return false;
		}
		info->playable_in_ns = global_in_ns;
		info->playable_out_ns = global_out_ns;
		info->sync_offset_ns = sync_offset_ns;
	}
	return true;
}

bool sr_replay_coverage_query(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
			      struct sr_replay_coverage_info *info)
{
	return coverage_query_internal(camera_name, event_in_ns, event_out_ns, false, 0, info);
}

bool sr_replay_coverage_query_at(const char *camera_name, uint64_t event_in_ns, uint64_t event_out_ns,
				 uint64_t timestamp_ns, struct sr_replay_coverage_info *info)
{
	return coverage_query_internal(camera_name, event_in_ns, event_out_ns, true, timestamp_ns, info);
}
