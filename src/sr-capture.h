/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include "sr-buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_CAPTURE_ID "sports_replay_capture"
#define SR_PLAYBACK_ID "sports_replay"

/* Settings key on a playback source naming the camera source it captures
 * from. Shared with the dock, which needs it to route a saved replay file
 * back to the playback source/scene for the camera it came from. */
#define S_CAPTURE_SOURCE "capture_source"

/* Per-camera replay timing correction. Positive means the camera signal
 * arrives late relative to the global/master timeline, so replay selects a
 * later camera-media timestamp (global + offset). */
#define S_SYNC_OFFSET_MS "sync_offset_ms"
#define SR_CAMERA_SYNC_MAX_MS 5000

/* Returns the ring buffer of a capture filter instance (obs_obj_get_data
 * of a source whose id is SR_CAPTURE_ID). Used by the playback source to
 * take replay snapshots. */
struct sr_buffer *sr_capture_get_buffer(void *capture_data);

/* Aggregate health of all capture filters. This is intentionally a snapshot:
 * the dock uses it for the vMix-style global recording controls without
 * touching writer objects owned by the video callbacks. */
struct sr_capture_recording_summary {
	size_t camera_count;
	size_t requested_count;
	size_t active_count;
	size_t failed_count;
	size_t reserve_blocked_count;
	uint64_t packets_written;
	uint64_t bytes_written;
};

/* Enables/disables continuous disk recording on every Pitel capture filter by
 * updating the persistent OBS filter setting. Returns false only if source
 * enumeration failed; a successful call may still report zero cameras. */
bool sr_capture_set_all_disk_recording(bool enabled, size_t *camera_count);

/* Reads the last video-thread-published recorder state for every capture
 * filter. Safe to call from the Qt frontend thread. */
bool sr_capture_get_recording_summary(struct sr_capture_recording_summary *summary);

/* Loads a saved replay file into the given Pitel Instant Replay playback source and
 * starts playing it (with the same controls as a live replay). Used by the
 * dock's double-click action. No-op if source is not a Pitel Instant Replay source. */
void sr_playback_play_file(obs_source_t *source, const char *path);

#ifdef __cplusplus
}
#endif
